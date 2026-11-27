/**
 * @file turn_auth.c
 * @brief Stateless challenges and TURN long-term message integrity.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <string.h>
#include <syslog.h>

#include <openssl/crypto.h>

#include "net_addr.h"
#include <ufsrvcorelib/ufsrv_core/turn/codec/ns_turn_msg.h>
#include "turn/auth/turn_auth.h"
#include "turn/auth/turn_credential.h"

static const char nonce_cookies[][14] = {"obMatJos2gAAA", "obMatJos2wAAA"};

enum { NONCE_COOKIE_LENGTH = sizeof(nonce_cookies[0]) - 1 };

static const uint8_t password_algorithms[] = {0, TURN_PASSWORD_SHA256, 0, 0, 0, TURN_PASSWORD_MD5, 0, 0};

/*! @brief Immutable authentication configuration shared by protocol workers. */
struct TurnAuthContext
{
  TurnCredentialProvider provider;                                 ///< Borrowed credential resolver.
  char                   realm[STUN_MAX_REALM_SIZE + 1];           ///< Configured realm advertised in challenges.
  size_t                 realm_length;                             ///< Length of the realm in bytes.
  uint32_t               nonce_lifetime;                           ///< Maximum nonce age in seconds.
  uint8_t                nonce_key[TURN_STATELESS_NONCE_KEY_SIZE]; ///< Derived secret used for stateless nonces.
};

TurnAuthContext *
TurnAuthCreate(const char *           realm, const uint8_t *secret, size_t secret_length, uint32_t nonce_lifetime,
               TurnCredentialProvider provider)
{
  if (!realm || !*realm || !secret || secret_length < 32 || !nonce_lifetime || !provider.resolve) {
    syslog(LOG_ERR, "%s: incomplete auth context {realm:'%s', secret:'%s', secret_length:'%zu', nonce_lifetime:'%u'}",
           __func__, realm ? realm : "(null)", secret ? "present" : "missing", secret_length, (unsigned)nonce_lifetime);
    return NULL;
  }
  size_t realm_length = strlen(realm);
  if (realm_length > STUN_MAX_REALM_SIZE) {
    syslog(LOG_ERR, "%s: realm too long {length:'%zu', max:'%d'}", __func__, realm_length, STUN_MAX_REALM_SIZE);
    return NULL;
  }

  TurnAuthContext *context = OPENSSL_zalloc(sizeof(*context));
  if (!context) {
    syslog(LOG_ERR, "%s: could not allocate the auth context", __func__);
    return NULL;
  }
  if (!turn_derive_stateless_nonce_key(secret, secret_length, context->nonce_key, sizeof(context->nonce_key))) {
    TurnAuthDestroy(context);
    return NULL;
  }
  memcpy(context->realm, realm, realm_length + 1);
  context->realm_length   = realm_length;
  context->provider       = provider;
  context->nonce_lifetime = nonce_lifetime;
  return context;
}

void
TurnAuthDestroy(TurnAuthContext *context)
{
  OPENSSL_clear_free(context, sizeof(*context));
}

void
TurnAuthResultClear(TurnAuthResult *result)
{
  if (result) OPENSSL_cleanse(result, sizeof(*result));
}

static size_t
AttributeStorage(size_t length)
{
  return 4 + ((length + 3) & ~(size_t)3);
}

static TurnAuthStatus
MakeAuthError(const TurnAuthContext *context, const TurnDatagram *frame, const ioa_addr *peer, const uint8_t *packet,
              size_t                 packet_length, uint32_t      now, uint16_t error, uint8_t *reply, size_t capacity,
              TurnAuthResult *       result)
{
  const bool challenge = error == 401 || error == 438;
  enum { NONCE_LENGTH = NONCE_COOKIE_LENGTH + TURN_STATELESS_NONCE_LENGTH };
  size_t required = STUN_HEADER_LENGTH + AttributeStorage(4);
  if (challenge)
    required += AttributeStorage(context->realm_length) + AttributeStorage(NONCE_LENGTH) + AttributeStorage(
      sizeof(password_algorithms));
  if (frame->fingerprint) required += AttributeStorage(4);
  if (!reply || required > capacity) return TURN_AUTH_DROP;

  char nonce[NONCE_LENGTH + 1];
  if (challenge) {
    memcpy(nonce, nonce_cookies[context->provider.supports_userhash], NONCE_COOKIE_LENGTH);
    if (!turn_generate_stateless_nonce(context->nonce_key, sizeof(context->nonce_key), peer, now,
                                       nonce + NONCE_COOKIE_LENGTH, TURN_STATELESS_NONCE_SIZE))
      return TURN_AUTH_UNAVAILABLE;
  }

  stun_tid transaction;
  stun_tid_from_message_str(packet, packet_length, &transaction);
  size_t length = 0;
  stun_init_error_response_str(frame->policy->method, reply, &length, error, NULL, &transaction, false);
  if (challenge && (!
    stun_attr_add_str(reply, &length, STUN_ATTRIBUTE_REALM, (const uint8_t*)context->realm, (int)context->realm_length)
    || !stun_attr_add_str(reply, &length, STUN_ATTRIBUTE_NONCE, (const uint8_t*)nonce, NONCE_LENGTH) || !
    stun_attr_add_str(reply, &length, STUN_ATTRIBUTE_PASSWORD_ALGORITHMS, password_algorithms,
                      sizeof(password_algorithms)))) return TURN_AUTH_UNAVAILABLE;
  if (frame->fingerprint && !stun_attr_add_fingerprint_str(reply, &length)) return TURN_AUTH_UNAVAILABLE;

  result->response_length = length;
  return TURN_AUTH_CHALLENGED;
}

static stun_attr_ref
CoveredAttribute(const uint8_t *packet, size_t length, size_t attributes_end, uint16_t type)
{
  stun_attr_ref attribute = stun_attr_get_first_by_type_str(packet, length, type);
  if (!attribute || (size_t)((const uint8_t*)attribute - packet) >= attributes_end) return NULL;
  return attribute;
}

static bool
GetPasswordAlgorithm(stun_attr_ref algorithm, stun_attr_ref algorithms, TurnPasswordAlgorithm *selected)
{
  *selected = TURN_PASSWORD_MD5;
  if (!algorithm && !algorithms) return true;
  if (!algorithm || !algorithms || stun_attr_get_len(algorithm) != 4 || stun_attr_get_len(algorithms) != sizeof(
      password_algorithms) || memcmp(stun_attr_get_value(algorithms), password_algorithms,
                                     sizeof(password_algorithms)) !=
    0) return false;

  const uint8_t *value = stun_attr_get_value(algorithm);
  if (turn_read_u16(value + 2) != 0) return false;
  const uint16_t id = turn_read_u16(value);
  if (id != TURN_PASSWORD_MD5 && id != TURN_PASSWORD_SHA256) return false;
  *selected = (TurnPasswordAlgorithm)id;
  return true;
}

static int
VerifyIntegrity(const uint8_t *packet, stun_attr_ref attribute, bool sha256, const TurnCredential *credential)
{
  const size_t offset     = (const uint8_t*)attribute - packet;
  const size_t mac_length = (size_t)stun_attr_get_len(attribute);
  uint8_t *    covered    = OPENSSL_memdup(packet, offset);
  if (!covered) return -1;

  turn_write_u16(covered + 2, (uint16_t)(offset + AttributeStorage(mac_length) - STUN_HEADER_LENGTH));
  uint8_t mac[32];
  unsigned int produced = sizeof(mac);
  const bool calculated = stun_calculate_hmac(covered, offset, credential->key, credential->key_length, mac, &produced,
                                              sha256 ? SHATYPE_SHA256 : SHATYPE_SHA1);
  const bool matches = calculated && mac_length <= produced && CRYPTO_memcmp(
    mac, stun_attr_get_value(attribute), mac_length) == 0;
  OPENSSL_cleanse(mac, sizeof(mac));
  OPENSSL_free(covered);
  return calculated ? matches : -1;
}

static bool
NonceValid(const TurnAuthContext *context, const ioa_addr *peer, uint32_t now, stun_attr_ref attribute)
{
  char           nonce[TURN_STATELESS_NONCE_SIZE];
  const size_t   cookie_length = NONCE_COOKIE_LENGTH;
  const uint8_t *value         = stun_attr_get_value(attribute);
  if ((size_t)stun_attr_get_len(attribute) != cookie_length + sizeof(nonce) - 1 || memcmp(
    value, nonce_cookies[context->provider.supports_userhash], cookie_length) != 0) return false;
  memcpy(nonce, value + cookie_length, sizeof(nonce) - 1);
  nonce[sizeof(nonce) - 1] = '\0';
  return turn_check_stateless_nonce(context->nonce_key, sizeof(context->nonce_key), peer, now, context->nonce_lifetime,
                                    nonce, NULL);
}

TurnAuthStatus
TurnAuthenticateRequest(const TurnAuthContext *context, const TurnDatagram *frame, const struct sockaddr *peer,
                        socklen_t peer_len, const uint8_t *packet, size_t packet_length, time_t now, uint8_t *reply,
                        size_t reply_capacity, TurnAuthResult *result)
{
  if (!result) return TURN_AUTH_DROP;
  TurnAuthResultClear(result);
  if (!context) return TURN_AUTH_UNAVAILABLE;
  if (!frame || !frame->policy || frame->policy->auth_mode != TURN_AUTH_LONG_TERM || !packet || now < 0 || (uintmax_t)
    now > UINT32_MAX || !peer || peer_len < sizeof(peer->sa_family)) return TURN_AUTH_DROP;
  size_t address_length = NetAddrLength(peer);
  if (!address_length || peer_len < address_length) return TURN_AUTH_DROP;
  ioa_addr peer_address = {0};
  memcpy(&peer_address, peer, address_length);

  stun_attr_ref sha256 =
    stun_attr_get_first_by_type_str(packet, packet_length, STUN_ATTRIBUTE_MESSAGE_INTEGRITY_SHA256);
  stun_attr_ref sha1       = stun_attr_get_first_by_type_str(packet, packet_length, STUN_ATTRIBUTE_MESSAGE_INTEGRITY);
  const size_t  end        = frame->attributes_end;
  stun_attr_ref username   = CoveredAttribute(packet, packet_length, end, STUN_ATTRIBUTE_USERNAME);
  stun_attr_ref userhash   = CoveredAttribute(packet, packet_length, end, STUN_ATTRIBUTE_USERHASH);
  stun_attr_ref realm      = CoveredAttribute(packet, packet_length, end, STUN_ATTRIBUTE_REALM);
  stun_attr_ref nonce      = CoveredAttribute(packet, packet_length, end, STUN_ATTRIBUTE_NONCE);
  stun_attr_ref algorithm  = CoveredAttribute(packet, packet_length, end, STUN_ATTRIBUTE_PASSWORD_ALGORITHM);
  stun_attr_ref algorithms = CoveredAttribute(packet, packet_length, end, STUN_ATTRIBUTE_PASSWORD_ALGORITHMS);

  TurnPasswordAlgorithm selected;
  uint16_t              error = 0;
  if (!sha1 && !sha256) {
    error = 401;
  }
  else if (((username == NULL) == (userhash == NULL)) || !realm || !nonce || !GetPasswordAlgorithm(
    algorithm, algorithms, &selected)) {
    error = 400;
  }
  else if ((size_t)stun_attr_get_len(realm) != context->realm_length || memcmp(
    stun_attr_get_value(realm), context->realm, context->realm_length) != 0) {
    error = 401;
  }
  else {
    stun_attr_ref       identity = username ? username : userhash;
    TurnCredentialQuery query    = {
      .realm           = context->realm, .identity = stun_attr_get_value(identity),
      .identity_length = (size_t)stun_attr_get_len(identity),
      .kind            = username ? TURN_CREDENTIAL_USERNAME : TURN_CREDENTIAL_USERHASH, .password_algorithm = selected,
      .now             = now,
    };
    const TurnCredentialStatus status = TurnCredentialResolve(&context->provider, &query, &result->credential);
    if (status == TURN_CREDENTIAL_UNAVAILABLE) return TURN_AUTH_UNAVAILABLE;
    const int integrity = status == TURN_CREDENTIAL_FOUND ?
                            VerifyIntegrity(packet, sha256 ? sha256 : sha1, sha256 != NULL, &result->credential) :
                            0;
    if (integrity < 0) {
      TurnCredentialClear(&result->credential);
      return TURN_AUTH_UNAVAILABLE;
    }
    if (integrity == 0) {
      error = 401;
    }
    else if (!NonceValid(context, &peer_address, (uint32_t)now, nonce)) {
      error = 438;
    }
    else {
      result->sign_sha256 = algorithm != NULL;
      return TURN_AUTH_VERIFIED;
    }
  }

  TurnCredentialClear(&result->credential);
  return MakeAuthError(context, frame, &peer_address, packet, packet_length, (uint32_t)now, error, reply,
                       reply_capacity, result);
}

bool
TurnAuthSignResponse(const TurnAuthResult *result, uint8_t *reply, size_t *reply_length, size_t reply_capacity)
{
  if (!result || !reply || !reply_length || *reply_length > reply_capacity || !result->credential.key_length || result->
    credential.key_length > sizeof(result->credential.key) || !stun_is_command_message_str(reply, *reply_length) || !
    stun_is_response_str(reply, *reply_length) ||
    stun_attr_get_first_by_type_str(reply, *reply_length, STUN_ATTRIBUTE_MESSAGE_INTEGRITY) ||
    stun_attr_get_first_by_type_str(reply, *reply_length, STUN_ATTRIBUTE_MESSAGE_INTEGRITY_SHA256) ||
    stun_attr_get_first_by_type_str(reply, *reply_length, STUN_ATTRIBUTE_FINGERPRINT)) return false;

  const size_t       offset     = *reply_length;
  const unsigned int mac_length = result->sign_sha256 ? 32 : 20;
  const uint16_t     type       = result->sign_sha256 ?
                                    STUN_ATTRIBUTE_MESSAGE_INTEGRITY_SHA256 :
                                    STUN_ATTRIBUTE_MESSAGE_INTEGRITY;
  if (AttributeStorage(mac_length) > reply_capacity - offset) return false;

  uint8_t mac[32] = {0};
  size_t  length  = offset;
  if (!stun_attr_add_str(reply, &length, type, mac, (int)mac_length)) return false;

  unsigned int produced = sizeof(mac);
  const bool   ok       = stun_calculate_hmac(reply, offset, result->credential.key, result->credential.key_length, mac,
                                              &produced, result->sign_sha256 ? SHATYPE_SHA256 : SHATYPE_SHA1) &&
    produced ==
    mac_length;
  if (ok) {
    memcpy(reply + offset + 4, mac, mac_length);
    *reply_length = length;
  }
  else {
    turn_write_u16(reply + 2, (uint16_t)(offset - STUN_HEADER_LENGTH));
  }
  OPENSSL_cleanse(mac, sizeof(mac));
  return ok;
}
