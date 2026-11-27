#ifndef UFSRVSTUN_TURN_ALLOCATION_H
#define UFSRVSTUN_TURN_ALLOCATION_H

#include "turn_session.h"
#include "turn/auth/turn_auth.h"

uint16_t TurnAllocate(UdpServer *server, UdpFlowHandle flow, TurnModelSession *session,
                      const TurnDatagram *frame, const TurnAuthResult *auth,
                      const struct sockaddr *peer, const uint8_t *request, size_t request_length,
                      uint8_t *reply, size_t capacity, size_t *length);
uint16_t TurnRefresh(UdpServer *server, UdpFlowHandle flow, TurnModelSession *session,
                      const TurnDatagram *frame, const TurnAuthResult *auth,
                      const uint8_t *request, size_t request_length,
                      uint8_t *reply, size_t capacity, size_t *length);
bool TurnRelayStart(void);
void TurnRelayStop(void);
void TurnRelaySend(TurnModelSession *session, const uint8_t *request, size_t length);
uint16_t TurnCreatePermission(TurnModelSession *session, const TurnDatagram *frame,
                               const TurnAuthResult *auth, const uint8_t *request, size_t request_length,
                               uint8_t *reply, size_t capacity, size_t *length);
uint16_t TurnChannelBind(TurnModelSession *session, const TurnDatagram *frame,
                           const TurnAuthResult *auth, const uint8_t *request, size_t request_length,
                           uint8_t *reply, size_t capacity, size_t *length);
void TurnRelayChannelData(TurnModelSession *session, const TurnDatagram *frame, const uint8_t *packet);
void TurnAllocationExpire(TurnModelSession *session);
void TurnAllocationClose(TurnModelSession *session);

#endif
