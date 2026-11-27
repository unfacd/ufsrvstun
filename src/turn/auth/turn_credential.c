/**
 * @file turn_credential.c
 * @brief Provider result validation and immutable standalone lookup.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdlib.h>
#include <string.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include "turn/auth/turn_credential.h"
#include <ufsrvcorelib/ufsrv_core/turn/codec/ns_turn_msg_defs.h>

/*! @brief Owned standalone record and its precomputed USERHASH. */
typedef struct TurnStoredCredential
{
  char *          username;                           ///< Copied prepared username.
  char *          realm;                              ///< Copied prepared realm.
  size_t          username_length;                    ///< Prepared username length.
  uint8_t         userhash[TURN_USERHASH_LENGTH];     ///< Binary SHA-256 of username, colon, and realm.
  TurnPrincipalId principal;                          ///< Stable application identity.
  uint8_t         md5_key[TURN_MD5_KEY_LENGTH];       ///< Derived MD5 credential key if present.
  uint8_t         sha256_key[TURN_SHA256_KEY_LENGTH]; ///< Derived SHA-256 credential key if present.
  time_t          expires_at;                         ///< Unix expiry, or zero.
  bool            has_md5_key;                        ///< Whether MD5 lookup is enabled for this record.
  bool            has_sha256_key;                     ///< Whether SHA-256 lookup is enabled for this record.
  bool            disabled;                           ///< Whether all lookup is disabled.
} TurnStoredCredential;

/*! @brief Immutable array of provisioned credentials. */
struct TurnCredentialStore
{
  size_t               count;     ///< Number of copied records.
  TurnStoredCredential records[]; ///< Records owned and cleansed by this store.
};

static bool
PrincipalValid(const TurnPrincipalId *principal)
{
  return principal->length > 0 && principal->length <= sizeof(principal->bytes);
}

static size_t
PasswordKeyLength(TurnPasswordAlgorithm algorithm)
{
  if (algorithm == TURN_PASSWORD_MD5) return TURN_MD5_KEY_LENGTH;
  if (algorithm == TURN_PASSWORD_SHA256) return TURN_SHA256_KEY_LENGTH;
  return 0;
}

void
TurnCredentialClear(TurnCredential *credential)
{
  if (credential) OPENSSL_cleanse(credential, sizeof(*credential));
}

TurnCredentialStatus
TurnCredentialResolve(const TurnCredentialProvider *provider, const TurnCredentialQuery *query,
                      TurnCredential *              credential)
{
  if (!credential) return TURN_CREDENTIAL_UNAVAILABLE;
  TurnCredentialClear(credential);
  if (!provider || !provider->resolve || !query || !query->realm || !*query->realm || !query->identity || query->now <
    0) return TURN_CREDENTIAL_UNAVAILABLE;

  const size_t expected_length = PasswordKeyLength(query->password_algorithm);
  if (!expected_length || (query->kind != TURN_CREDENTIAL_USERNAME && query->kind != TURN_CREDENTIAL_USERHASH) || (query
    ->kind == TURN_CREDENTIAL_USERHASH && (!provider->supports_userhash || query->identity_length !=
      TURN_USERHASH_LENGTH)) || (query->kind == TURN_CREDENTIAL_USERNAME && (!query->identity_length || query->
    identity_length > STUN_MAX_USERNAME_SIZE))) return TURN_CREDENTIAL_NOT_FOUND;

  TurnCredentialStatus status = provider->resolve(provider->context, query, credential);
  if (status == TURN_CREDENTIAL_FOUND) {
    if (!PrincipalValid(&credential->principal) || credential->key_length != expected_length || credential->expires_at <
      0) {
      status = TURN_CREDENTIAL_UNAVAILABLE;
    }
    else if (credential->expires_at && credential->expires_at <= query->now) {
      status = TURN_CREDENTIAL_DISABLED;
    }
  }
  if (status != TURN_CREDENTIAL_FOUND) {
    TurnCredentialClear(credential);
    if (status != TURN_CREDENTIAL_NOT_FOUND && status != TURN_CREDENTIAL_DISABLED) status = TURN_CREDENTIAL_UNAVAILABLE;
  }
  return status;
}

static bool
ComputeUserhash(TurnStoredCredential *record)
{
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (!ctx) return false;

  unsigned int length = 0;
  bool         ok     = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
    EVP_DigestUpdate(ctx, record->username, record->username_length) == 1 && EVP_DigestUpdate(ctx, ":", 1) == 1 &&
    EVP_DigestUpdate(ctx, record->realm, strlen(record->realm)) == 1 &&
    EVP_DigestFinal_ex(ctx, record->userhash, &length) == 1 && length == TURN_USERHASH_LENGTH;
  EVP_MD_CTX_free(ctx);
  return ok;
}

static bool
CopyRecord(TurnStoredCredential *record, const TurnProvisionedCredential *source)
{
  if (!source->username || !source->realm || !*source->username || !*source->realm || (!source->md5_key && !source->
    sha256_key) || source->expires_at < 0 || !PrincipalValid(&source->principal)) return false;

  record->username_length = strlen(source->username);
  if (record->username_length > STUN_MAX_USERNAME_SIZE || strlen(source->realm) > STUN_MAX_REALM_SIZE) return false;

  record->username = strdup(source->username);
  record->realm    = strdup(source->realm);
  if (!record->username || !record->realm || !ComputeUserhash(record)) return false;

  record->principal      = source->principal;
  record->expires_at     = source->expires_at;
  record->disabled       = source->disabled;
  record->has_md5_key    = source->md5_key != NULL;
  record->has_sha256_key = source->sha256_key != NULL;
  if (source->md5_key) memcpy(record->md5_key, source->md5_key, sizeof(record->md5_key));
  if (source->sha256_key) memcpy(record->sha256_key, source->sha256_key, sizeof(record->sha256_key));
  return true;
}

TurnCredentialStore *
TurnCredentialStoreCreate(const TurnProvisionedCredential *records, size_t count)
{
  if ((!records && count) || count > (SIZE_MAX - sizeof(TurnCredentialStore)) / sizeof(TurnStoredCredential)) return
    NULL;

  TurnCredentialStore *store = OPENSSL_zalloc(sizeof(*store) + count * sizeof(store->records[0]));
  if (!store) return NULL;
  store->count = count;
  for (size_t i = 0; i < count; i++) {
    if (!CopyRecord(&store->records[i], &records[i])) goto fail;
    for (size_t j = 0; j < i; j++) {
      if (strcmp(store->records[j].realm, store->records[i].realm) == 0 && strcmp(
        store->records[j].username, store->records[i].username) == 0) goto fail;
    }
  }
  return store;

fail:
  TurnCredentialStoreDestroy(store);
  return NULL;
}

static bool
MatchesIdentity(const TurnStoredCredential *record, const TurnCredentialQuery *query)
{
  if (strcmp(record->realm, query->realm) != 0) return false;
  if (query->kind == TURN_CREDENTIAL_USERHASH) return CRYPTO_memcmp(record->userhash, query->identity,
                                                                    TURN_USERHASH_LENGTH) == 0;
  return record->username_length == query->identity_length && memcmp(record->username, query->identity,
                                                                     query->identity_length) == 0;
}

static TurnCredentialStatus
ResolveProvisioned(void *context, const TurnCredentialQuery *query, TurnCredential *credential)
{
  const TurnCredentialStore *store = context;
  if (!store) return TURN_CREDENTIAL_UNAVAILABLE;

  for (size_t i = 0; i < store->count; i++) {
    const TurnStoredCredential *record = &store->records[i];
    if (!MatchesIdentity(record, query)) continue;
    if (record->disabled) return TURN_CREDENTIAL_DISABLED;

    const bool md5 = query->password_algorithm == TURN_PASSWORD_MD5;
    if (md5 ? !record->has_md5_key : !record->has_sha256_key) return TURN_CREDENTIAL_NOT_FOUND;

    credential->principal  = record->principal;
    credential->expires_at = record->expires_at;
    credential->key_length = PasswordKeyLength(query->password_algorithm);
    memcpy(credential->key, md5 ? record->md5_key : record->sha256_key, credential->key_length);
    return TURN_CREDENTIAL_FOUND;
  }
  return TURN_CREDENTIAL_NOT_FOUND;
}

TurnCredentialProvider
TurnCredentialStoreProvider(TurnCredentialStore *store)
{
  return (TurnCredentialProvider)
  {
    .resolve = ResolveProvisioned, .context = store, .supports_userhash = true
  };
}

void
TurnCredentialStoreDestroy(TurnCredentialStore *store)
{
  if (!store) return;
  for (size_t i = 0; i < store->count; i++) {
    free(store->records[i].username);
    free(store->records[i].realm);
  }
  OPENSSL_clear_free(store, sizeof(*store) + store->count * sizeof(store->records[0]));
}
