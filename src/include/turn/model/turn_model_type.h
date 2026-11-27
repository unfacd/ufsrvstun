/*
 * turn_model_type.h - stateful data model for a TURN server (RFC 8656).
 *
 * Types only: no functions, no allocation policy. The split is deliberate:
 * everything here is state that must outlive a single packet. Per-packet data
 * (message view, attribute cursor, transaction id, credentials) lives on the
 * stack and is not modelled here.
 *
 * Four objects carry TURN semantics:
 *
 *   TurnModelSession     keyed by the client 5-tuple (or connection for TCP/TLS)
 *     +-- TurnModelAllocation   created by Allocate, 600 s default / 3600 max
 *           +-- TurnModelPermission   keyed by peer IP only, 300 s
 *           +-- TurnModelChannel      chnum <-> peer IP:port, 600 s
 *
 * Optional stateful extensions, present only if you implement them:
 *
 *   TurnModelTcpConnection     RFC 6062 CONNECTION-ID
 *   TurnModelReservation       RFC 8656 EVEN-PORT / RESERVATION-TOKEN
 *   TurnModelMobilityTicket    RFC 8016 MOBILITY-TICKET
 *
 * Indexing requirements (see the *_next members):
 *   - sessions by client 5-tuple / connection id
 *   - relay socket -> allocation (peer packets arrive with no client address)
 *   - permissions by peer IP (port is wildcarded)
 *   - channels by chnum AND by peer address: client->peer keys on the channel
 *     number, peer->client keys on the peer address. Both are needed.
 */

#ifndef TURN_MODEL_TYPE_H
#define TURN_MODEL_TYPE_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <time.h>

#include <ufsrv_core/udp_server/udp_server.h>
#include "turn/auth/turn_credential_type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------- limits */

#define TURN_MODEL_CHANNEL_MIN 0x4000u
#define TURN_MODEL_CHANNEL_MAX 0x4FFFu /* RFC 8656 par. 12 */

/* Lifetimes in seconds (RFC 8656 par. 4, 8, 9, 11). */
#define TURN_MODEL_ALLOC_LIFETIME 600u
#define TURN_MODEL_ALLOC_LIFETIME_MAX 3600u
#define TURN_MODEL_PERMISSION_LIFETIME 300u
#define TURN_MODEL_CHANNEL_LIFETIME 600u

#define TURN_MODEL_MAX_TCP_CONNECTIONS 128u
#define TURN_MODEL_TICKET_MAX 32u /* opaque, adjust to your ticket format */

/* ---------------------------------------------------------------- types */

/* Addresses are plain sockaddr_storage so they interoperate with
 * recvfrom/sendto and with net_addr.h. */
typedef struct sockaddr_storage TurnModelAddr;

/* Monotonic seconds. Compare with subtraction, not <, so wraparound is safe:
 * (int32_t)(now - expires) >= 0  ==  expired. */
typedef uint32_t TurnModelSeconds;

typedef uint64_t TurnModelId;

typedef enum {
  TURN_MODEL_TRANSPORT_UDP = 0,
  TURN_MODEL_TRANSPORT_TCP,
  TURN_MODEL_TRANSPORT_TLS,
  TURN_MODEL_TRANSPORT_DTLS
} TurnModelTransport;

typedef enum { TURN_MODEL_TCP_CONNECTING = 0, TURN_MODEL_TCP_CONNECTED, TURN_MODEL_TCP_CLOSING } TurnModelTcpState;

struct TurnModelSession;
struct TurnModelAllocation;
struct TurnModelPermission;
struct TurnModelChannel;
struct TurnModelTcpConnection;

/* One channel binding. A channel is always subordinate to a permission: the
 * ChannelBind that creates it also installs/refreshes the permission for the
 * peer IP. */
typedef struct TurnModelChannel {
  uint16_t chnum; /* 0x4000..0x4FFF */
  TurnModelAddr peer;
  struct timespec deadline;
  struct TurnModelChannel *next_by_chnum;
  struct TurnModelChannel *next_by_peer;
} TurnModelChannel;

/* One permission. Keyed by peer IP only: RFC 8656 makes the port wildcard, so
 * never include sin_port / sin6_port in the key or in equality checks. */
typedef struct TurnModelPermission {
  TurnModelAddr peer_ip;    /* port fields must be zero */
  struct timespec deadline;
  struct TurnModelPermission *next;
} TurnModelPermission;

/* One allocation, created by a successful Allocate and destroyed on expiry or
 * on a Refresh with lifetime 0. */
typedef struct TurnModelAllocation {
  TurnModelId id;
  /* [0] = IPv4, [1] = IPv6; ss_family == 0 means that family is absent.
   * Dual allocation (RFC 8656 par. 7.2) fills both. */
  TurnModelAddr relayed[2];
  int relay_fd[2]; /* -1 when the family is absent */
  struct timespec deadline;
  uint8_t allocate_transaction[12];
  uint32_t allocate_lifetime;
  uint8_t refresh_transaction[12];
  uint32_t refresh_lifetime;
  struct TurnModelAllocation *quota_next;
  UdpServer *server;
  UdpFlowHandle flow;
  TurnModelPermission *perms;
  struct TurnModelChannelIndex *channels;
  uint32_t permission_count;
  struct TurnModelSession *owner;
} TurnModelAllocation;

typedef struct TurnModelSession {
  TurnPrincipalId principal;
  TurnModelAllocation *alloc;
  TurnPrincipalId deallocation_principal;
  uint8_t deallocation_transaction[12];
} TurnModelSession;

/* RFC 6062 TCP allocation entry. Full peer address: the port counts here. */
typedef struct TurnModelTcpConnection {
  uint64_t connection_id; /* CONNECTION-ID */
  TurnModelAddr peer;
  int client_fd; /* data connection towards the client */
  int peer_fd;   /* connection towards the peer */
  TurnModelTcpState state;
  TurnModelSeconds expires;
  TurnModelAllocation *alloc;
  struct TurnModelTcpConnection *next;
} TurnModelTcpConnection;

/* EVEN-PORT reservation: a relay port held for a second Allocate, addressed by
 * the 8-byte RESERVATION-TOKEN. */
typedef struct TurnModelReservation {
  uint64_t token; /* 8 bytes, opaque to the client */
  TurnModelAddr relayed;
  int relay_fd;
  TurnModelSeconds expires;
  struct TurnModelReservation *next;
} TurnModelReservation;

/* RFC 8016 mobility: a ticket that lets the client resume an allocation from a
 * new 5-tuple. Opaque bytes; the ticket is whatever you hand out. */
typedef struct TurnModelMobilityTicket {
  uint8_t ticket[TURN_MODEL_TICKET_MAX];
  size_t ticket_len;
  struct TurnModelAllocation *alloc;
  TurnModelSeconds expires;
  struct TurnModelMobilityTicket *next;
} TurnModelMobilityTicket;

#ifdef __cplusplus
}
#endif

#endif /* TURN_MODEL_TYPE_H */
