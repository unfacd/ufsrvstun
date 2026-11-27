#ifndef UFSRVSTUN_TURN_SESSION_H
#define UFSRVSTUN_TURN_SESSION_H

#include <ufsrv_core/udp_server/udp_server.h>

#include "turn_model_type.h"

bool TurnSessionPoolInit(size_t groups, size_t sessions_per_group);
TurnModelSession *TurnSessionForFlow(UdpServer *server, UdpFlowHandle flow);
TurnModelSession *TurnSessionAttach(UdpServer *server, UdpFlowHandle flow,
                                    const TurnPrincipalId *principal);
void TurnSessionRelease(UdpFlowHandle flow, InstanceHolder *holder);

#endif
