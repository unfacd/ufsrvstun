/**
 * @file protocol_stun.h
 * @brief STUN protocol registry entry.
 */

#ifndef UFSRVSTUN_PROTOCOL_STUN_H
#define UFSRVSTUN_PROTOCOL_STUN_H

#include <ufsrv_core/udp_server/udp_server.h>
#include <ufsrv_core/protocol/protocol_type.h>
#include "main_protocols.h"

typedef struct TurnAuthContext TurnAuthContext;

bool StunProtocolInit(const TurnAuthContext *auth);
void StunProtocolStop(void);
UdpServer *StunProtocolStart(bool dtls, unsigned char *worker_slots, size_t worker_count);

#endif
