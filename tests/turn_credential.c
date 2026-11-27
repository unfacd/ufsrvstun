#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

#include "turn/auth/turn_credential.h"
#include "turn/auth/turn_unfacd_credential.h"

static TurnCredentialStatus
BadProvider(void *context, const TurnCredentialQuery *query, TurnCredential *credential)
{
  (void)context;
  (void)query;
  memset(credential->key, 0xa5, sizeof(credential->key));
  credential->key_length = 3;
  return TURN_CREDENTIAL_FOUND;
}

static void
TestUnfacdCredentials(void)
{
  const uint8_t md5[] = {
    0x0e, 0x61, 0xf9, 0x06, 0xc6, 0x59, 0xaf, 0xad, 0xc5, 0x08, 0x34, 0xd9, 0x4d, 0x2e, 0x5c, 0xcb
  };
  const uint8_t sha256[] = {
    0x3a, 0x99, 0xa7, 0x33, 0x1f, 0xb3, 0x6e, 0xdc, 0x93, 0xa3, 0xb4, 0x82, 0x44, 0xa2, 0xa7, 0xba,
    0xd2, 0xe4, 0xdd, 0x1e, 0xed, 0xde, 0x2d, 0xf9, 0x4b, 0x59, 0x2a, 0xdf, 0x60, 0xc6, 0xb6, 0xdd
  };
  uint8_t secret[TURN_UNFACD_SECRET_LENGTH];
  for (size_t i = 0; i < sizeof(secret); i++) secret[i] = (uint8_t)i;

  TurnUnfacdCredentialContext *issuer = TurnUnfacdCredentialCreate("turn.example", secret, 60);
  TurnUnfacdCredentialContext *resolver = TurnUnfacdCredentialCreate("turn.example", secret, 60);
  assert(issuer && resolver);
  TurnUnfacdIssuedCredential issued;
  assert(!TurnUnfacdCredentialIssue(issuer, 0, 1000, &issued));
  assert(TurnUnfacdCredentialIssue(issuer, 42, 1000, &issued));
  assert(issued.expires_at == 1060);
  assert(strcmp(issued.username, "0000000000000424:000000000000002a") == 0);
  assert(strcmp(issued.password, "hMTx/phMpxhNwb+3Klf1pbn270Y8W7Kj++QjEvvdNyw=") == 0);
  TurnUnfacdCredentialDestroy(issuer);

  TurnCredentialProvider provider = TurnUnfacdCredentialProvider(resolver);
  TurnCredentialQuery query = {
    .realm = "turn.example", .identity = (const uint8_t *)issued.username,
    .identity_length = strlen(issued.username), .kind = TURN_CREDENTIAL_USERNAME,
    .password_algorithm = TURN_PASSWORD_MD5, .now = 1059
  };
  TurnCredential result;
  assert(TurnCredentialResolve(&provider, &query, &result) == TURN_CREDENTIAL_FOUND);
  assert(result.key_length == sizeof(md5) && memcmp(result.key, md5, sizeof(md5)) == 0);
  const uint8_t principal[] = {0, 0, 0, 0, 0, 0, 0, 42};
  assert(result.principal.length == sizeof(principal));
  assert(memcmp(result.principal.bytes, principal, sizeof(principal)) == 0);
  query.password_algorithm = TURN_PASSWORD_SHA256;
  assert(TurnCredentialResolve(&provider, &query, &result) == TURN_CREDENTIAL_FOUND);
  assert(result.key_length == sizeof(sha256) && memcmp(result.key, sha256, sizeof(sha256)) == 0);

  issued.username[32] = 'b';
  assert(TurnCredentialResolve(&provider, &query, &result) == TURN_CREDENTIAL_FOUND);
  assert(memcmp(result.key, sha256, sizeof(sha256)) != 0);
  issued.username[32] = 'A';
  assert(TurnCredentialResolve(&provider, &query, &result) == TURN_CREDENTIAL_NOT_FOUND);
  issued.username[32] = 'a';
  query.realm = "other.example";
  assert(TurnCredentialResolve(&provider, &query, &result) == TURN_CREDENTIAL_NOT_FOUND);
  query.realm = "turn.example";
  query.now = 1060;
  assert(TurnCredentialResolve(&provider, &query, &result) == TURN_CREDENTIAL_DISABLED);
  assert(result.key_length == 0);

  TurnUnfacdIssuedCredentialClear(&issued);
  TurnUnfacdCredentialDestroy(resolver);
}

int
main(void)
{
  TestUnfacdCredentials();
  static const uint8_t userhash[TURN_USERHASH_LENGTH] = {
    0xc3, 0xd6, 0x23, 0x4b, 0xcd, 0x00, 0x89, 0x19, 0x2f, 0xd0, 0xd5, 0x2d, 0xea, 0x80, 0x2a, 0xc7,
    0xcb, 0x65, 0x8d, 0xeb, 0x02, 0x3e, 0x3c, 0x3a, 0xc9, 0x12, 0xa1, 0x6b, 0x46, 0x69, 0x2e, 0xfa,
  };
  uint8_t md5_key[TURN_MD5_KEY_LENGTH] = {0x17};
  uint8_t sha256_key[TURN_SHA256_KEY_LENGTH] = {0x42};
  TurnProvisionedCredential records[] = {
    {.username = "alice", .realm = "relay.example", .principal = {.bytes = {0x23}, .length = 1},
     .md5_key = md5_key, .sha256_key = sha256_key, .expires_at = 200},
    {.username = "bob", .realm = "relay.example", .principal = {.bytes = {0x24}, .length = 1},
     .sha256_key = sha256_key, .disabled = true},
    {.username = "alice", .realm = "other.example", .principal = {.bytes = {0x25}, .length = 1},
     .sha256_key = sha256_key},
  };

  TurnCredentialStore *store = TurnCredentialStoreCreate(records, 3);
  assert(store);
  // The store copied its inputs.
  sha256_key[0] = 0;

  TurnCredentialProvider provider = TurnCredentialStoreProvider(store);
  TurnCredentialQuery query = {
    .realm = "relay.example", .identity = (const uint8_t *)"alice", .identity_length = 5,
    .kind = TURN_CREDENTIAL_USERNAME, .password_algorithm = TURN_PASSWORD_MD5, .now = 199,
  };
  TurnCredential result;
  assert(TurnCredentialResolve(&provider, &query, &result) == TURN_CREDENTIAL_FOUND);
  assert(result.key_length == TURN_MD5_KEY_LENGTH && result.key[0] == 0x17 && result.principal.bytes[0] == 0x23);

  query.password_algorithm = TURN_PASSWORD_SHA256;
  query.kind = TURN_CREDENTIAL_USERHASH;
  query.identity = userhash;
  query.identity_length = sizeof(userhash);
  assert(TurnCredentialResolve(&provider, &query, &result) == TURN_CREDENTIAL_FOUND);
  assert(result.key_length == TURN_SHA256_KEY_LENGTH && result.key[0] == 0x42);
  TurnCredentialClear(&result);
  assert(!result.key_length && !result.key[0]);

  query.now = 200;
  assert(TurnCredentialResolve(&provider, &query, &result) == TURN_CREDENTIAL_DISABLED);
  assert(!result.key_length && !result.key[0]);
  query.realm = "other.example";
  assert(TurnCredentialResolve(&provider, &query, &result) == TURN_CREDENTIAL_NOT_FOUND);

  query.kind = TURN_CREDENTIAL_USERNAME;
  query.identity = (const uint8_t *)"alice";
  query.identity_length = 5;
  assert(TurnCredentialResolve(&provider, &query, &result) == TURN_CREDENTIAL_FOUND);
  assert(result.principal.bytes[0] == 0x25);
  query.password_algorithm = TURN_PASSWORD_MD5;
  assert(TurnCredentialResolve(&provider, &query, &result) == TURN_CREDENTIAL_NOT_FOUND);

  query.realm = "relay.example";
  query.identity = (const uint8_t *)"bob";
  query.identity_length = 3;
  query.password_algorithm = TURN_PASSWORD_SHA256;
  assert(TurnCredentialResolve(&provider, &query, &result) == TURN_CREDENTIAL_DISABLED);

  TurnCredentialProvider bad = {.resolve = BadProvider};
  assert(TurnCredentialResolve(&bad, &query, &result) == TURN_CREDENTIAL_UNAVAILABLE);
  for (size_t i = 0; i < sizeof(result.key); i++) assert(result.key[i] == 0);

  TurnCredentialStoreDestroy(store);
  records[1] = records[0];
  assert(!TurnCredentialStoreCreate(records, 2));
  return 0;
}
