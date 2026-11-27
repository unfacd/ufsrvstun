/**
 * @file turn_credential_type.h
 * @brief Credential provider types for TURN long-term authentication.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef UFSRVSTUN_TURN_CREDENTIAL_TYPE_H
#define UFSRVSTUN_TURN_CREDENTIAL_TYPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

enum {
  TURN_PRINCIPAL_MAX_LENGTH = 32,
  TURN_USERHASH_LENGTH = 32,
  TURN_MD5_KEY_LENGTH = 16,
  TURN_SHA256_KEY_LENGTH = 32
};

/*! @brief Identity attribute used for credential lookup. */
typedef enum TurnCredentialIdentityKind
{
  TURN_CREDENTIAL_USERNAME, ///< Prepared username bytes.
  TURN_CREDENTIAL_USERHASH  ///< Binary SHA-256 username and realm hash.
} TurnCredentialIdentityKind;

/*! @brief Wire algorithm for deriving the long-term integrity key. */
typedef enum TurnPasswordAlgorithm
{
  TURN_PASSWORD_MD5 = 1,   ///< Derive a 16-byte key.
  TURN_PASSWORD_SHA256 = 2 ///< Derive a 32-byte key.
} TurnPasswordAlgorithm;

/*! @brief Stable application identity, independent of wire usernames. */
typedef struct TurnPrincipalId
{
  uint8_t bytes[TURN_PRINCIPAL_MAX_LENGTH]; ///< Opaque principal bytes.
  size_t length;                            ///< Number of valid bytes.
} TurnPrincipalId;

/*! @brief Borrowed lookup inputs with one wall-clock time snapshot. */
typedef struct TurnCredentialQuery
{
  const char *realm;                      ///< Prepared configured realm, NUL-terminated.
  const uint8_t *identity;                ///< Prepared username or binary USERHASH.
  size_t identity_length;                 ///< Identity byte count.
  TurnCredentialIdentityKind kind;       ///< Format of the identity bytes.
  TurnPasswordAlgorithm password_algorithm; ///< Requested key derivation algorithm.
  time_t now;                             ///< Unix time for credential expiry checks.
} TurnCredentialQuery;

/*! @brief Caller-owned resolved credential. Clear it after the request. */
typedef struct TurnCredential
{
  TurnPrincipalId principal;           ///< Resolved identity. Authenticate the message before using it.
  uint8_t key[TURN_SHA256_KEY_LENGTH]; ///< Derived long-term key.
  size_t key_length;                    ///< Number of valid key bytes.
  time_t expires_at;                    ///< Unix expiry, or zero for a provisioned non-expiring key.
} TurnCredential;

typedef enum TurnCredentialStatus
{
  TURN_CREDENTIAL_FOUND,       ///< A usable credential was copied to the result.
  TURN_CREDENTIAL_NOT_FOUND,   ///< No matching credential or algorithm.
  TURN_CREDENTIAL_DISABLED,    ///< The credential is disabled or expired.
  TURN_CREDENTIAL_UNAVAILABLE  ///< Provider failed or returned invalid data.
} TurnCredentialStatus;

/*! @brief Host-supplied key resolver. Resolve may run on multiple shard workers. */
typedef struct TurnCredentialProvider
{
  TurnCredentialStatus (*resolve)(void *context, const TurnCredentialQuery *query,
                                   TurnCredential *credential); ///< Resolve one credential.
  void *context;                                                ///< Opaque host context.
  bool supports_userhash;
} TurnCredentialProvider;

#endif /* UFSRVSTUN_TURN_CREDENTIAL_TYPE_H */
