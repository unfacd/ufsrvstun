/**
 * @file turn_credential.h
 * @brief Credential lookup and standalone provisioned credential store.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef UFSRVSTUN_TURN_CREDENTIAL_H
#define UFSRVSTUN_TURN_CREDENTIAL_H

#include <stdbool.h>
#include "turn_credential_type.h"

typedef struct TurnCredentialStore TurnCredentialStore;

/*! @brief Input record copied into an immutable standalone store. */
typedef struct TurnProvisionedCredential
{
  const char *username;                 ///< Prepared username, NUL-terminated.
  const char *realm;                    ///< Prepared realm, NUL-terminated.
  TurnPrincipalId principal;            ///< Stable identity used by TURN allocation policy.
  const uint8_t *md5_key;                ///< Optional 16-byte derived key.
  const uint8_t *sha256_key;             ///< Optional 32-byte derived key.
  time_t expires_at;                     ///< Unix expiry, or zero for no expiry.
  bool disabled;                        ///< Disable lookup of this record.
} TurnProvisionedCredential;

/**
 * Resolve a credential and validate the provider result. Output is cleared
 * on failure. The caller clears successful results after their final use.
 *
 * @code{.c}
 * TurnCredential result = {0};
 * TurnCredentialStatus status = TurnCredentialResolve(&provider, &query, &result);
 * // Consume result when status is TURN_CREDENTIAL_FOUND.
 * TurnCredentialClear(&result);
 * @endcode
 */
TurnCredentialStatus TurnCredentialResolve(const TurnCredentialProvider *provider,
                                            const TurnCredentialQuery *query,
                                            TurnCredential *credential);

/**
 * Cleanse a credential, including its key buffer.
 *
 * @code{.c}
 * TurnCredentialClear(&result);
 * @endcode
 */
void TurnCredentialClear(TurnCredential *credential);

/**
 * Copy the supplied records and precompute their USERHASH values. The returned
 * store rejects duplicate username and realm pairs. The provider may be
 * called concurrently while the store is alive.
 *
 * @code{.c}
 * TurnCredentialStore *store = TurnCredentialStoreCreate(records, count);
 * TurnCredentialProvider provider = TurnCredentialStoreProvider(store);
 * @endcode
 */
TurnCredentialStore *TurnCredentialStoreCreate(const TurnProvisionedCredential *records,
                                                size_t count);

/**
 * Return a provider that borrows the store. Stop its consumers before
 * destroying the store.
 *
 * @code{.c}
 * TurnCredentialProvider provider = TurnCredentialStoreProvider(store);
 * @endcode
 */
TurnCredentialProvider TurnCredentialStoreProvider(TurnCredentialStore *store);

/**
 * Release the store and cleanse its derived keys.
 *
 * @code{.c}
 * TurnCredentialStoreDestroy(store);
 * @endcode
 */
void TurnCredentialStoreDestroy(TurnCredentialStore *store);

#endif /* UFSRVSTUN_TURN_CREDENTIAL_H */
