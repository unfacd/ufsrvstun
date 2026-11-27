#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include "turn/auth/turn_unfacd_credential.h"
#include <ufsrvcorelib/ufsrv_core/turn/codec/ns_turn_msg_defs.h>

struct TurnUnfacdCredentialContext
{
  uint8_t  secret[TURN_UNFACD_SECRET_LENGTH];
  uint32_t lifetime_seconds;
  char     realm[STUN_MAX_REALM_SIZE + 1];
};

TurnUnfacdCredentialContext *
TurnUnfacdCredentialCreate(const char *realm, const uint8_t secret[TURN_UNFACD_SECRET_LENGTH],
                           uint32_t    lifetime_seconds)
{
  if (!realm || !*realm || strlen(realm) > STUN_MAX_REALM_SIZE || !secret || !lifetime_seconds) return NULL;

  TurnUnfacdCredentialContext *context = OPENSSL_zalloc(sizeof(*context));
  if (!context) return NULL;
  strcpy(context->realm, realm);
  memcpy(context->secret, secret, sizeof(context->secret));
  context->lifetime_seconds = lifetime_seconds;
  return context;
}

void
TurnUnfacdCredentialDestroy(TurnUnfacdCredentialContext *context)
{
  OPENSSL_clear_free(context, sizeof(*context));
}

void
TurnUnfacdIssuedCredentialClear(TurnUnfacdIssuedCredential *issued)
{
  TurnEphemeralCredentialClear(issued);
}

bool
TurnUnfacdCredentialIssue(const TurnUnfacdCredentialContext *context, uint64_t authenticated_user_id, time_t now,
                          TurnUnfacdIssuedCredential *       issued)
{
  if (!context) {
    TurnUnfacdIssuedCredentialClear(issued);
    return false;
  }

  return TurnEphemeralCredentialIssue(context->realm, context->secret, context->lifetime_seconds, authenticated_user_id,
                                      now, issued);
}

static bool
ReadHex64(const uint8_t *text, uint64_t *value)
{
  char number[17];
  memcpy(number, text, sizeof(number) - 1);
  number[sizeof(number) - 1] = '\0';
  if (strspn(number, "0123456789abcdef") != sizeof(number) - 1) return false;
  *value = (uint64_t)strtoumax(number, NULL, 16);
  return true;
}

static TurnCredentialStatus
ResolveUnfacd(void *data, const TurnCredentialQuery *query, TurnCredential *credential)
{
  const TurnUnfacdCredentialContext *context = data;
  if (!context) return TURN_CREDENTIAL_UNAVAILABLE;

  uint64_t expiry, user_id;
  if (strcmp(query->realm, context->realm) != 0 || query->kind != TURN_CREDENTIAL_USERNAME || query->identity_length !=
    TURN_UNFACD_USERNAME_SIZE - 1 || query->identity[16] != ':' || !ReadHex64(query->identity, &expiry) || !
    ReadHex64(query->identity + 17, &user_id) || !user_id || !expiry) return TURN_CREDENTIAL_NOT_FOUND;

  time_t expires_at = (time_t)expiry;
  if (expires_at < 0 || (uintmax_t)expires_at != expiry) return TURN_CREDENTIAL_NOT_FOUND;
  if (expires_at <= query->now) return TURN_CREDENTIAL_DISABLED;
  if (expiry - (uint64_t)query->now > context->lifetime_seconds) return TURN_CREDENTIAL_NOT_FOUND;

  char username[TURN_UNFACD_USERNAME_SIZE] = {0};
  memcpy(username, query->identity, query->identity_length);
  char          password[TURN_UNFACD_PASSWORD_SIZE] = {0};
  char          input[TURN_UNFACD_USERNAME_SIZE + STUN_MAX_REALM_SIZE + TURN_UNFACD_PASSWORD_SIZE + 1];
  const EVP_MD *digest     = query->password_algorithm == TURN_PASSWORD_MD5 ? EVP_md5() : EVP_sha256();
  unsigned int  key_length = 0;
  bool          ok         = TurnEphemeralCredentialDerivePassword(context->realm, context->secret, username, password);
  if (ok) {
    int length = snprintf(input, sizeof(input), "%s:%s:%s", username, context->realm, password);
    ok = length > 0 && (size_t)length < sizeof(input) && EVP_Digest(input, (size_t)length, credential->key, &key_length,
                                                                    digest, NULL) == 1;
  }
  OPENSSL_cleanse(input, sizeof(input));
  OPENSSL_cleanse(password, sizeof(password));
  if (!ok) return TURN_CREDENTIAL_UNAVAILABLE;

  for (size_t i = 0; i < sizeof(user_id); i++) credential->principal.bytes[i] = (uint8_t)(
    user_id >> (8 * (sizeof(user_id) - 1 - i)));
  credential->principal.length = sizeof(user_id);
  credential->expires_at       = expires_at;
  credential->key_length       = key_length;
  return TURN_CREDENTIAL_FOUND;
}

TurnCredentialProvider
TurnUnfacdCredentialProvider(TurnUnfacdCredentialContext *context)
{
  return (TurnCredentialProvider)
  {
    .resolve = ResolveUnfacd, .context = context
  };
}
