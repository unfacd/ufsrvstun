/**
 * @file turn_auth_runtime.h
 * @brief Host startup wiring for encrypted TURN credentials.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef UFSRVSTUN_TURN_AUTH_RUNTIME_H
#define UFSRVSTUN_TURN_AUTH_RUNTIME_H

#include "turn_auth.h"
#include "turn_credential.h"
#include "turn_unfacd_credential.h"

typedef struct StunTurnAuthRuntime
{
  TurnAuthContext *auth;
  TurnCredentialStore *standalone;
  TurnUnfacdCredentialContext *unfacd;
} StunTurnAuthRuntime;

bool StunTurnAuthLoad(StunTurnAuthRuntime *runtime);
void StunTurnAuthDestroy(StunTurnAuthRuntime *runtime);

#endif
