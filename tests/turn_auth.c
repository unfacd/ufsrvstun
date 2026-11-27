#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

#include <openssl/hmac.h>

#include <ufsrvcorelib/ufsrv_core/turn/codec/ns_turn_msg.h>
#include "turn/auth/turn_auth.h"
#include "turn/auth/turn_credential.h"

static const char realm[] = "relay.example";
static const char username[] = "alice";
static const uint8_t supported_algorithms[] = {0, 2, 0, 0, 0, 1, 0, 0};
static const uint8_t sha1_key[TURN_MD5_KEY_LENGTH] = {1, 2, 3};
static const uint8_t sha256_key[TURN_SHA256_KEY_LENGTH] = {4, 5, 6};

static TurnAuthStatus
Authenticate(TurnAuthContext *auth, const ioa_addr *peer, const uint8_t *request, size_t length,
             time_t now, uint8_t *reply, size_t capacity, TurnAuthResult *result)
{
  TurnDatagram frame;
  assert(TurnDecodeDatagram(request, length, true, &frame));
  return TurnAuthenticateRequest(auth, &frame, &peer->ss, sizeof(peer->s4), request, length,
                                 now, reply, capacity, result);
}

static void
CheckError(const uint8_t *request, const uint8_t *reply, size_t length, int expected)
{
  int error = 0;
  assert(stun_is_error_response_str(reply, length, &error, NULL, 0));
  assert(error == expected && memcmp(request + 8, reply + 8, STUN_TID_SIZE) == 0);
}

static void
AppendTestIntegrity(uint8_t *request, size_t *length, bool sha256)
{
  size_t offset = *length;
  unsigned int mac_len = sha256 ? 32 : 20;
  uint8_t mac[32] = {0};
  const uint16_t type = sha256 ? STUN_ATTRIBUTE_MESSAGE_INTEGRITY_SHA256 : STUN_ATTRIBUTE_MESSAGE_INTEGRITY;
  assert(stun_attr_add_str(request, length, type, mac, (int)mac_len));
  assert(HMAC(sha256 ? EVP_sha256() : EVP_sha1(),
              sha256 ? sha256_key : sha1_key, sha256 ? sizeof(sha256_key) : sizeof(sha1_key),
              request, offset, mac, &mac_len));
  memcpy(request + offset + 4, mac, mac_len);
}

static void
CheckResponseSignature(const TurnAuthResult *result, bool sha256)
{
  uint8_t response[128];
  stun_tid tid = {{1}};
  size_t length;
  stun_init_success_response_str(STUN_METHOD_ALLOCATE, response, &length, &tid);
  const size_t unsigned_length = length;
  assert(!TurnAuthSignResponse(result, response, &length, length));
  assert(length == unsigned_length);
  assert(TurnAuthSignResponse(result, response, &length, sizeof(response)));
  assert(!TurnAuthSignResponse(result, response, &length, sizeof(response)));

  uint8_t expected[32];
  unsigned int expected_len = sizeof(expected);
  assert(HMAC(sha256 ? EVP_sha256() : EVP_sha1(),
              sha256 ? sha256_key : sha1_key, sha256 ? sizeof(sha256_key) : sizeof(sha1_key),
              response, unsigned_length, expected, &expected_len));
  stun_attr_ref attribute = stun_attr_get_first_by_type_str(response, length,
      sha256 ? STUN_ATTRIBUTE_MESSAGE_INTEGRITY_SHA256 : STUN_ATTRIBUTE_MESSAGE_INTEGRITY);
  assert(attribute && stun_attr_get_len(attribute) == (int)expected_len);
  assert(memcmp(stun_attr_get_value(attribute), expected, expected_len) == 0);
}

static TurnCredentialStatus
Unavailable(void *context, const TurnCredentialQuery *query, TurnCredential *credential)
{
  (void)context;
  (void)query;
  (void)credential;
  return TURN_CREDENTIAL_UNAVAILABLE;
}

int
main(void)
{
  uint8_t request[256], reply[256], untouched[256];
  const uint8_t transport[4] = {17};
  const uint8_t sha256_algorithm[4] = {0, 2, 0, 0};
  const uint8_t secret[32] = {0x18, 0x34};
  const TurnProvisionedCredential provisioned = {
    .username = username, .realm = realm,
    .principal = {.bytes = {0x23}, .length = 1},
    .md5_key = sha1_key, .sha256_key = sha256_key,
  };
  TurnCredentialStore *store = TurnCredentialStoreCreate(&provisioned, 1);
  TurnAuthContext *auth = TurnAuthCreate(realm, secret, sizeof(secret), 60, TurnCredentialStoreProvider(store));
  assert(store && auth);
  ioa_addr peer = {.s4 = {.sin_family = AF_INET, .sin_port = htons(35000),
                        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)}};
  size_t length;
  TurnAuthResult result;

  stun_init_request_str(STUN_METHOD_ALLOCATE, request, &length);
  assert(stun_attr_add_str(request, &length, STUN_ATTRIBUTE_REQUESTED_TRANSPORT, transport, 4));
  assert(Authenticate(auth, &peer, request, length, 100, reply, 20, &result) == TURN_AUTH_DROP);
  assert(!result.response_length);
  assert(Authenticate(auth, &peer, request, length, 100, reply, sizeof(reply), &result) == TURN_AUTH_CHALLENGED);
  CheckError(request, reply, result.response_length, 401);
  stun_attr_ref nonce = stun_attr_get_first_by_type_str(reply, result.response_length, STUN_ATTRIBUTE_NONCE);
  stun_attr_ref algorithms = stun_attr_get_first_by_type_str(reply, result.response_length, STUN_ATTRIBUTE_PASSWORD_ALGORITHMS);
  assert(nonce && algorithms);
  assert(stun_attr_get_len(algorithms) == sizeof(supported_algorithms));
  assert(memcmp(stun_attr_get_value(algorithms), supported_algorithms, sizeof(supported_algorithms)) == 0);
  uint8_t nonce_value[80];
  const int nonce_len = stun_attr_get_len(nonce);
  assert(nonce_len > 12 && (size_t)nonce_len <= sizeof(nonce_value));
  memcpy(nonce_value, stun_attr_get_value(nonce), (size_t)nonce_len);
  assert(memcmp(nonce_value, "obMatJos2wAAA", 13) == 0);

  const size_t initial_length = length;
  // Integrity without the identity/nonce envelope is a bad request.
  AppendTestIntegrity(request, &length, false);
  assert(Authenticate(auth, &peer, request, length, 101, reply, sizeof(reply), &result) == TURN_AUTH_CHALLENGED);
  CheckError(request, reply, result.response_length, 400);

  length = initial_length;
  turn_write_u16(request + 2, (uint16_t)(length - STUN_HEADER_LENGTH));
  assert(stun_attr_add_str(request, &length, STUN_ATTRIBUTE_USERNAME, (const uint8_t *)username, sizeof(username) - 1));
  assert(stun_attr_add_str(request, &length, STUN_ATTRIBUTE_REALM, (const uint8_t *)realm, sizeof(realm) - 1));
  assert(stun_attr_add_str(request, &length, STUN_ATTRIBUTE_NONCE, nonce_value, nonce_len));
  const size_t common_length = length;
  AppendTestIntegrity(request, &length, false);
  memcpy(untouched, request, length);
  assert(Authenticate(auth, &peer, request, length, 101, reply, sizeof(reply), &result) == TURN_AUTH_VERIFIED);
  assert(result.credential.principal.bytes[0] == 0x23 && !result.sign_sha256);
  assert(memcmp(request, untouched, length) == 0);
  CheckResponseSignature(&result, false);
  TurnAuthResultClear(&result);

  assert(Authenticate(auth, &peer, request, length, 173, reply, sizeof(reply), &result) == TURN_AUTH_CHALLENGED);
  CheckError(request, reply, result.response_length, 438);
  assert(!result.credential.key_length);
  peer.s4.sin_port = htons(35001);
  assert(Authenticate(auth, &peer, request, length, 101, reply, sizeof(reply), &result) == TURN_AUTH_CHALLENGED);
  CheckError(request, reply, result.response_length, 438);
  peer.s4.sin_port = htons(35000);
  request[length - 1] ^= 1;
  assert(Authenticate(auth, &peer, request, length, 173, reply, sizeof(reply), &result) == TURN_AUTH_CHALLENGED);
  CheckError(request, reply, result.response_length, 401);

  length = common_length;
  turn_write_u16(request + 2, (uint16_t)(length - STUN_HEADER_LENGTH));
  assert(stun_attr_add_str(request, &length, STUN_ATTRIBUTE_PASSWORD_ALGORITHM, sha256_algorithm, 4));
  assert(stun_attr_add_str(request, &length, STUN_ATTRIBUTE_PASSWORD_ALGORITHMS, supported_algorithms, 8));
  AppendTestIntegrity(request, &length, true);
  assert(stun_attr_add_fingerprint_str(request, &length));
  assert(Authenticate(auth, &peer, request, length, 101, reply, sizeof(reply), &result) == TURN_AUTH_VERIFIED);
  assert(result.sign_sha256);
  CheckResponseSignature(&result, true);

  TurnCredentialProvider username_provider = TurnCredentialStoreProvider(store);
  username_provider.supports_userhash = false;
  TurnAuthContext *username_auth = TurnAuthCreate(realm, secret, sizeof(secret), 60, username_provider);
  assert(username_auth);
  assert(Authenticate(username_auth, &peer, request, length, 101, reply, sizeof(reply), &result) == TURN_AUTH_CHALLENGED);
  CheckError(request, reply, result.response_length, 438);
  nonce = stun_attr_get_first_by_type_str(reply, result.response_length, STUN_ATTRIBUTE_NONCE);
  assert(nonce && stun_attr_get_len(nonce) == nonce_len);
  assert(memcmp(stun_attr_get_value(nonce), "obMatJos2gAAA", 13) == 0);
  algorithms = stun_attr_get_first_by_type_str(reply, result.response_length, STUN_ATTRIBUTE_PASSWORD_ALGORITHMS);
  assert(algorithms && stun_attr_get_len(algorithms) == sizeof(supported_algorithms));
  TurnAuthDestroy(username_auth);

  TurnAuthContext *unavailable = TurnAuthCreate(realm, secret, sizeof(secret), 60,
                                                (TurnCredentialProvider){.resolve = Unavailable});
  assert(unavailable);
  assert(Authenticate(unavailable, &peer, request, length, 101, reply, sizeof(reply), &result) == TURN_AUTH_UNAVAILABLE);
  assert(!result.credential.key_length && !result.response_length);
  TurnAuthResultClear(&result);
  TurnAuthDestroy(unavailable);
  TurnAuthDestroy(auth);
  TurnCredentialStoreDestroy(store);
}
