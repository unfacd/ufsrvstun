#ifndef UFSRVSTUN_TURN_RELAY_POLICY_H
#define UFSRVSTUN_TURN_RELAY_POLICY_H

#include <stdbool.h>
#include <sys/socket.h>

bool TurnRelayPolicyLoad(const char *fallback_host);
void TurnRelayPolicyDestroy(void);
int TurnRelayAddressFamily(void);
int TurnRelayBindSocket(struct sockaddr_storage *advertised);
bool TurnRelayPeerAllowed(const struct sockaddr *peer);

#endif
