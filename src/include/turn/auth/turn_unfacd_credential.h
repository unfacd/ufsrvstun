#ifndef UFSRVSTUN_TURN_UNFACD_CREDENTIAL_H
#define UFSRVSTUN_TURN_UNFACD_CREDENTIAL_H

#include <stdbool.h>

#include "turn_credential_type.h"

#include <ufsrvcorelib/ufsrv_core/turn/turn_ephemeral_credential.h>

enum {
  TURN_UNFACD_SECRET_LENGTH = TURN_EPHEMERAL_SECRET_LENGTH,
  TURN_UNFACD_USERNAME_SIZE = TURN_EPHEMERAL_USERNAME_SIZE,
  TURN_UNFACD_PASSWORD_SIZE = TURN_EPHEMERAL_PASSWORD_SIZE
};

typedef struct TurnUnfacdCredentialContext TurnUnfacdCredentialContext;
typedef TurnEphemeralCredential TurnUnfacdIssuedCredential;

TurnUnfacdCredentialContext *TurnUnfacdCredentialCreate(
    const char *realm, const uint8_t secret[TURN_UNFACD_SECRET_LENGTH], uint32_t lifetime_seconds);
void TurnUnfacdCredentialDestroy(TurnUnfacdCredentialContext *context);
bool TurnUnfacdCredentialIssue(const TurnUnfacdCredentialContext *context,
                               uint64_t authenticated_user_id, time_t now,
                               TurnUnfacdIssuedCredential *issued);
void TurnUnfacdIssuedCredentialClear(TurnUnfacdIssuedCredential *issued);
TurnCredentialProvider TurnUnfacdCredentialProvider(TurnUnfacdCredentialContext *context);

#endif
