/**
 * @file turn_auth.h
 * @brief TURN long-term wire authentication and response signing.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef UFSRVSTUN_TURN_AUTH_H
#define UFSRVSTUN_TURN_AUTH_H

#include <stdbool.h>
#include <sys/socket.h>

#include "turn_credential_type.h"
#include "turn/policy/turn_dispatch.h"

typedef struct TurnAuthContext TurnAuthContext;

/*! @brief Result of authenticating a structurally valid TURN request. */
typedef enum TurnAuthStatus
{
  TURN_AUTH_DROP,        ///< Invalid input or insufficient response capacity.
  TURN_AUTH_CHALLENGED,  ///< A 400, 401, or 438 reply was encoded.
  TURN_AUTH_VERIFIED,    ///< The request integrity and nonce were verified.
  TURN_AUTH_UNAVAILABLE  ///< Credential lookup or authentication infrastructure failed.
} TurnAuthStatus;

/*! @brief Request-local authentication output. */
typedef struct TurnAuthResult
{
  TurnCredential credential; ///< Authenticated principal and signing key on success.
  bool sign_sha256;           ///< Sign the response with MESSAGE-INTEGRITY-SHA256.
  size_t response_length;     ///< Encoded challenge/error length, or zero.
} TurnAuthResult;

/**
 * Copy the realm and derive an isolated nonce key from a provisioned secret.
 * The provider context must outlive the returned authentication context.
 *
 * @code{.c}
 * TurnAuthContext *auth = TurnAuthCreate(realm, secret, secret_length, 600, provider);
 * @endcode
 */
TurnAuthContext *TurnAuthCreate(const char *realm, const uint8_t *secret, size_t secret_length,
                                uint32_t nonce_lifetime, TurnCredentialProvider provider);

/** Release the context and cleanse its nonce key. */
void TurnAuthDestroy(TurnAuthContext *context);

/** Clear the result after the authenticated handler finishes signing its response. */
void TurnAuthResultClear(TurnAuthResult *result);

/**
 * Authenticate a request using metadata from TurnDecodeDatagram for the same
 * packet. Writes challenge replies within reply_capacity and leaves the
 * original request bytes intact. Call TurnAuthResultClear after use.
 *
 * @code{.c}
 * TurnAuthResult result;
 * TurnAuthStatus status = TurnAuthenticateRequest(auth, &frame, peer, peer_len,
 *     request, request_length, time(NULL), reply, reply_capacity, &result);
 * @endcode
 */
TurnAuthStatus TurnAuthenticateRequest(const TurnAuthContext *context, const TurnDatagram *frame,
                                       const struct sockaddr *peer, socklen_t peer_len,
                                       const uint8_t *packet, size_t packet_length, time_t now,
                                       uint8_t *reply, size_t reply_capacity, TurnAuthResult *result);

/**
 * Append integrity to a success or error response before any FINGERPRINT.
 * The result must come from a successful TurnAuthenticateRequest.
 *
 * @code{.c}
 * if (status == TURN_AUTH_VERIFIED)
 *   TurnAuthSignResponse(&result, reply, &reply_length, reply_capacity);
 * @endcode
 */
bool TurnAuthSignResponse(const TurnAuthResult *result, uint8_t *reply, size_t *reply_length,
                          size_t reply_capacity);

#endif /* UFSRVSTUN_TURN_AUTH_H */
