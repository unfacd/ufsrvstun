#include <errno.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <uflib/recycler/recycler.h>

#include "net_addr.h"
#include <ufsrvcorelib/ufsrv_core/turn/codec/ns_turn_msg.h>
#include "turn/model/turn_allocation.h"
#include "turn/model/turn_relay_policy.h"
#include "ufsrvstun_config.h"

static pthread_mutex_t      allocations_lock = PTHREAD_MUTEX_INITIALIZER;
static TurnModelAllocation *allocations;
static uint64_t             next_relay_id;
static int                  relay_epoll = -1;
static int                  relay_wake  = -1;
static pthread_t            relay_thread;

static bool
Before(const struct timespec *now, const struct timespec *deadline)
{
  return now->tv_sec < deadline->tv_sec || (now->tv_sec == deadline->tv_sec && now->tv_nsec < deadline->tv_nsec);
}

enum { CHANNEL_BUCKETS = 64 };

typedef struct TurnModelChannelIndex
{
  TurnModelChannel *by_number[CHANNEL_BUCKETS];
  TurnModelChannel *by_peer[CHANNEL_BUCKETS];
  size_t            count;
} TurnModelChannelIndex;

static size_t
PeerBucket(const struct sockaddr *peer)
{
  return NetAddrHash(peer) % CHANNEL_BUCKETS;
}

static TurnModelChannel *
FindChannelNumber(TurnModelChannelIndex *index, uint16_t number)
{
  for (TurnModelChannel *entry = index->by_number[number % CHANNEL_BUCKETS]; entry; entry = entry->next_by_chnum) if (
    entry->chnum == number) return entry;
  return NULL;
}

static TurnModelChannel *
FindChannelPeer(const TurnModelChannelIndex *index, const struct sockaddr *peer)
{
  for (TurnModelChannel *entry = index->by_peer[PeerBucket(peer)]; entry; entry = entry->next_by_peer) if (NetAddrEqual(
    (const struct sockaddr*)&entry->peer, peer)) return entry;
  return NULL;
}

static void
InsertChannel(TurnModelChannelIndex *index, TurnModelChannel *entry)
{
  TurnModelChannel **by_number = &index->by_number[entry->chnum % CHANNEL_BUCKETS];
  TurnModelChannel **by_peer   = &index->by_peer[PeerBucket((const struct sockaddr*)&entry->peer)];
  entry->next_by_chnum         = *by_number;
  entry->next_by_peer          = *by_peer;
  *by_number                   = *by_peer = entry;
  index->count++;
}

static void
PruneChannels(TurnModelChannelIndex *index, const struct timespec *now)
{
  if (!index) return;
  for (size_t i = 0; i < CHANNEL_BUCKETS; i++) {
    TurnModelChannel **number = &index->by_number[i];
    while (*number) {
      TurnModelChannel *entry = *number;
      if (now && Before(now, &entry->deadline)) {
        number = &entry->next_by_chnum;
        continue;
      }

      TurnModelChannel **peer = &index->by_peer[PeerBucket((const struct sockaddr*)&entry->peer)];
      while (*peer != entry) peer = &(*peer)->next_by_peer;
      *peer   = entry->next_by_peer;
      *number = entry->next_by_chnum;
      index->count--;
      free(entry);
    }
  }
}

static TurnModelPermission *
FindPermission(TurnModelPermission *head, const struct sockaddr *peer)
{
  for (; head; head = head->next) if (NetAddrEqualNoPort((const struct sockaddr*)&head->peer_ip, peer)) return head;
  return NULL;
}

static void
FreePermissions(TurnModelPermission *head)
{
  while (head) {
    TurnModelPermission *next = head->next;
    free(head);
    head = next;
  }
}

static int
RelaySlot(const TurnModelAllocation *allocation)
{
  return allocation->relay_fd[0] >= 0 ? 0 : 1;
}

static bool
PeerAllowed(const TurnModelAllocation *allocation, const struct sockaddr *peer)
{
  return peer->sa_family == allocation->relayed[RelaySlot(allocation)].ss_family && TurnRelayPeerAllowed(peer);
}

static bool
HasPermission(TurnModelAllocation *allocation, const struct sockaddr *peer, const struct timespec *now)
{
  if (!PeerAllowed(allocation, peer) || !Before(now, &allocation->deadline)) return false;
  TurnModelPermission *permission = FindPermission(allocation->perms, peer);
  return permission && Before(now, &permission->deadline);
}

static void
RelayPeerDatagram(TurnModelAllocation *allocation)
{
  uint8_t   payload[STUN_BUFFER_SIZE - STUN_CHANNEL_HEADER_LENGTH];
  ioa_addr  peer     = {0};
  socklen_t peer_len = sizeof(peer);
  ssize_t   size     = recvfrom(allocation->relay_fd[RelaySlot(allocation)], payload, sizeof(payload), MSG_TRUNC,
                                (struct sockaddr*)&peer, &peer_len);
  struct timespec        now;
  const struct sockaddr *source = (const struct sockaddr*)&peer;
  if (size < 0 || (size_t)size > sizeof(payload)) {
#if UFSRVSTUN_DEBUG_BUILD_L3
    syslog(LOG_DEBUG, "%s {id:'%llu'}: peer read failed: %s", __func__, (unsigned long long)allocation->id,
           strerror(errno));
#endif
    return;
  }
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    syslog(LOG_WARNING, "%s {id:'%llu'}: could not read the monotonic clock", __func__,
           (unsigned long long)allocation->id);
    return;
  }
  if (!HasPermission(allocation, source, &now)) {
    char peer_text[NET_ADDR_STR_MAX] = {0};
    NetAddrFormat(source, peer_text, sizeof(peer_text));
#if UFSRVSTUN_DEBUG_BUILD_L3
    syslog(LOG_DEBUG, "%s {id:'%llu'}: no permission or expired for peer '%s'", __func__,
           (unsigned long long)allocation->id, peer_text);
#endif
    return;
  }

  TurnModelChannel *channel = allocation->channels ? FindChannelPeer(allocation->channels, source) : NULL;
  if (channel && Before(&now, &channel->deadline)) {
    uint8_t  reply[STUN_BUFFER_SIZE];
    uint16_t header[] = {htons(channel->chnum), htons((uint16_t)size)};
    memcpy(reply, header, sizeof(header));
    memcpy(reply + sizeof(header), payload, (size_t)size);
    UdpServerQueueSend(allocation->server, allocation->flow, reply, sizeof(header) + (size_t)size);
    return;
  }

  const size_t address_size   = source->sa_family == AF_INET ? 12 : 24;
  const size_t padded_payload = ((size_t)size + 3) & ~(size_t)3;
  if (STUN_HEADER_LENGTH + address_size + 4 + padded_payload > STUN_BUFFER_SIZE) return;

  uint8_t reply[STUN_BUFFER_SIZE];
  size_t  length;
  stun_init_indication_str(STUN_METHOD_DATA, reply, &length);
  if (stun_attr_add_addr_str(reply, &length, STUN_ATTRIBUTE_XOR_PEER_ADDRESS, &peer) &&
    stun_attr_add_str(reply, &length, STUN_ATTRIBUTE_DATA, payload, (int)size)) UdpServerQueueSend(
    allocation->server, allocation->flow, reply, length);
}

static void *
RelayThread(void *unused)
{
  (void)unused;
  for (;;) {
    struct epoll_event event;
    int                count = epoll_wait(relay_epoll, &event, 1, -1);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0 || !event.data.u64) return NULL;

    pthread_mutex_lock(&allocations_lock);
    for (TurnModelAllocation *entry = allocations; entry; entry = entry->quota_next) {
      if (entry->id == event.data.u64) {
        RelayPeerDatagram(entry);
        break;
      }
    }
    pthread_mutex_unlock(&allocations_lock);
  }
}

bool
TurnRelayStart(void)
{
  relay_epoll = epoll_create1(EPOLL_CLOEXEC);
  if (relay_epoll < 0) return false;
  relay_wake               = eventfd(0, EFD_CLOEXEC);
  struct epoll_event event = {.events = EPOLLIN};
  if (relay_wake >= 0 && epoll_ctl(relay_epoll, EPOLL_CTL_ADD, relay_wake, &event) == 0 && pthread_create(
    &relay_thread, NULL, RelayThread, NULL) == 0) return true;
  syslog(LOG_ERR, "%s: could not start the relay thread: %s", __func__, strerror(errno));
  if (relay_wake >= 0) close(relay_wake);
  close(relay_epoll);
  relay_wake = relay_epoll = -1;
  return false;
}

void
TurnRelayStop(void)
{
  if (relay_epoll < 0) return;
  while (eventfd_write(relay_wake, 1) < 0 && errno == EINTR) {
  }
  pthread_join(relay_thread, NULL);
  pthread_mutex_lock(&allocations_lock);
  close(relay_wake);
  close(relay_epoll);
  relay_wake = relay_epoll = -1;
  pthread_mutex_unlock(&allocations_lock);
}

static bool
RegisterRelay(TurnModelAllocation *allocation, UdpServer *server, UdpFlowHandle flow)
{
  pthread_mutex_lock(&allocations_lock);
  allocation->server = server;
  allocation->flow   = flow;
  bool registered    = false;
  if (relay_epoll >= 0 && next_relay_id < UINT64_MAX) {
    struct epoll_event event = {.events = EPOLLIN, .data.u64 = ++next_relay_id};
    registered = epoll_ctl(relay_epoll, EPOLL_CTL_ADD, allocation->relay_fd[RelaySlot(allocation)], &event) == 0;
    if (registered) allocation->id = event.data.u64;
  }
  pthread_mutex_unlock(&allocations_lock);
  if (!registered)
    syslog(LOG_WARNING, "%s: relay registration failed {relay_epoll:'%d', fd:'%d', next_id:'%llu'}", __func__,
           relay_epoll, allocation->relay_fd[RelaySlot(allocation)], (unsigned long long)next_relay_id);
  return registered;
}

static bool
ReserveQuota(TurnModelAllocation *allocation, unsigned limit)
{
  const TurnPrincipalId *principal = &allocation->owner->principal;
  unsigned               count     = 0;
  pthread_mutex_lock(&allocations_lock);
  for (TurnModelAllocation *entry = allocations; entry; entry = entry->quota_next) {
    const TurnPrincipalId *other = &entry->owner->principal;
    if (other->length == principal->length && memcmp(other->bytes, principal->bytes, principal->length) == 0 && ++count
      >= limit) break;
  }
  if (count < limit) {
    allocation->quota_next = allocations;
    allocations            = allocation;
  }
  pthread_mutex_unlock(&allocations_lock);
  return count < limit;
}

static void
ReleaseAllocation(TurnModelAllocation *allocation)
{
  char relay_text[NET_ADDR_STR_MAX] = {0};
  NetAddrFormat((const struct sockaddr*)&allocation->relayed[RelaySlot(allocation)], relay_text, sizeof(relay_text));
#if UFSRVSTUN_DEBUG_BUILD_L2
  syslog(LOG_DEBUG, "%s {id:'%llu', relay:'%s'}: allocation closed", __func__, (unsigned long long)allocation->id,
         relay_text);
#endif
  pthread_mutex_lock(&allocations_lock);
  TurnModelAllocation **entry = &allocations;
  while (*entry && *entry != allocation) entry = &(*entry)->quota_next;
  if (*entry) *entry = allocation->quota_next;
  pthread_mutex_unlock(&allocations_lock);

  for (size_t i = 0; i < 2; i++) if (allocation->relay_fd[i] >= 0) close(allocation->relay_fd[i]);
  PruneChannels(allocation->channels, NULL);
  free(allocation->channels);
  FreePermissions(allocation->perms);
  free(allocation);
}

void
TurnAllocationClose(TurnModelSession *session)
{
  if (!session->alloc) return;
  ReleaseAllocation(session->alloc);
  session->alloc = NULL;
}

static void
OnAllocationLeaseExpired(UdpFlowHandle flow, InstanceHolder *holder)
{
  (void)flow;
  TurnAllocationClose((TurnModelSession*)RecyclerGetClientContextData(holder));
}

void
TurnAllocationExpire(TurnModelSession *session)
{
  if (!session || !session->alloc) return;
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return;
  const struct timespec *deadline = &session->alloc->deadline;
  if (now.tv_sec > deadline->tv_sec || (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)) {
#if UFSRVSTUN_DEBUG_BUILD_L2
    syslog(LOG_DEBUG, "%s {id:'%llu'}: allocation lease expired", __func__, (unsigned long long)session->alloc->id);
#endif
    TurnAllocationClose(session);
  }
}

static bool
EncodeAllocateResponse(const TurnModelAllocation *allocation, const TurnDatagram *frame, const TurnAuthResult *auth,
                       const struct sockaddr *    peer, const uint8_t *request, size_t request_length, uint8_t *reply,
                       size_t                     capacity, size_t *length)
{
  const size_t relay_slot = allocation->relay_fd[0] >= 0 ? 0 : 1;
  const size_t required = STUN_HEADER_LENGTH + 8 + (relay_slot == 0 ? 12 : 24) + (peer->sa_family == AF_INET ? 12 : 24)
    + (auth->sign_sha256 ? 36 : 24) + (frame->fingerprint ? 8 : 0);
  if (capacity < required) return false;

  ioa_addr               relayed = {0}, mapped = {0};
  const struct sockaddr *relay   = (const struct sockaddr*)&allocation->relayed[relay_slot];
  memcpy(&relayed, relay, NetAddrLength(relay));
  memcpy(&mapped, peer, NetAddrLength(peer));
  stun_tid transaction;
  stun_tid_from_message_str(request, request_length, &transaction);
  const uint32_t lifetime = allocation->allocate_lifetime;
  return stun_set_allocate_response_str(reply, length, &transaction, &relayed, NULL, &mapped, lifetime, lifetime, 0,
                                        NULL, 0, NULL,
                                        false) && TurnAuthSignResponse(auth, reply, length, capacity) && (!frame->
    fingerprint || stun_attr_add_fingerprint_str(reply, length));
}

static stun_attr_ref
FindCoveredAttribute(const uint8_t *request, size_t length, size_t covered_end, uint16_t type)
{
  stun_attr_ref attr = stun_attr_get_first_by_type_str(request, length, type);
  return attr && (size_t)((const uint8_t*)attr - request) < covered_end ? attr : NULL;
}

static uint32_t
GrantedLifetime(const TurnDatagram *frame, const uint8_t *request, size_t length, bool allow_delete)
{
  uint32_t      lifetime = UfsrvConfigStunTurnAllocationLifetimeSeconds();
  stun_attr_ref attr     = FindCoveredAttribute(request, length, frame->attributes_end, STUN_ATTRIBUTE_LIFETIME);
  if (attr) {
    uint32_t requested = turn_read_u32(stun_attr_get_value(attr));
    if (allow_delete && !requested) return 0;
    if (requested > lifetime) lifetime = requested;
  }
  const uint32_t maximum = UfsrvConfigStunTurnMaxAllocationLifetimeSeconds();
  return lifetime < maximum ? lifetime : maximum;
}

uint16_t
TurnAllocate(UdpServer *           server, UdpFlowHandle flow, TurnModelSession *session, const TurnDatagram *frame,
             const TurnAuthResult *auth, const struct sockaddr *peer, const uint8_t *request, size_t request_length,
             uint8_t *             reply, size_t capacity, size_t *length)
{
  const uint8_t *transaction = request + 8;
  if (session->alloc) {
    if (memcmp(session->alloc->allocate_transaction, transaction, sizeof(session->alloc->allocate_transaction))) return
      437;
    return EncodeAllocateResponse(session->alloc, frame, auth, peer, request, request_length, reply, capacity, length) ?
             0 :
             500;
  }

  const size_t  covered   = frame->attributes_end;
  stun_attr_ref transport = FindCoveredAttribute(request, request_length, covered, STUN_ATTRIBUTE_REQUESTED_TRANSPORT);
  if (!transport) return 400;
  if (stun_attr_get_value(transport)[0] != IPPROTO_UDP) return 442;
  if (FindCoveredAttribute(request, request_length, covered, STUN_ATTRIBUTE_DONT_FRAGMENT)) return 420;

  stun_attr_ref family =
    FindCoveredAttribute(request, request_length, covered, STUN_ATTRIBUTE_REQUESTED_ADDRESS_FAMILY);
  stun_attr_ref additional = FindCoveredAttribute(request, request_length, covered,
                                                  STUN_ATTRIBUTE_ADDITIONAL_ADDRESS_FAMILY);
  stun_attr_ref even  = FindCoveredAttribute(request, request_length, covered, STUN_ATTRIBUTE_EVEN_PORT);
  stun_attr_ref token = FindCoveredAttribute(request, request_length, covered, STUN_ATTRIBUTE_RESERVATION_TOKEN);
  if ((family && additional) || (token && (family || additional || even)) || (additional && (
    stun_attr_get_value(additional)[0] != 2 || (even && (stun_attr_get_value(even)[0] & 0x80))))) return 400;
  if (additional || even || token) return 508;

  const uint8_t requested_family = family ? stun_attr_get_value(family)[0] : 1;
  if ((requested_family != 1 && requested_family != 2) || TurnRelayAddressFamily() != (
    requested_family == 1 ? AF_INET : AF_INET6)) return 440;

  const uint32_t lifetime = GrantedLifetime(frame, request, request_length, false);

  TurnModelAllocation *allocation = calloc(1, sizeof(*allocation));
  if (!allocation) return 508;
  allocation->owner             = session;
  allocation->relay_fd[0]       = allocation->relay_fd[1] = -1;
  allocation->allocate_lifetime = lifetime;
  memcpy(allocation->allocate_transaction, transaction, sizeof(allocation->allocate_transaction));
  if (!ReserveQuota(allocation, UfsrvConfigStunTurnMaxAllocationsPerPrincipal())) {
#if UFSRVSTUN_DEBUG_BUILD_L2
    syslog(LOG_NOTICE, "%s: allocation quota reached (limit:'%u')", __func__,
           (unsigned)UfsrvConfigStunTurnMaxAllocationsPerPrincipal());
#endif
    free(allocation);
    return 486;
  }

  uint16_t     error               = 508;
  const size_t relay_slot          = requested_family - 1;
  allocation->relay_fd[relay_slot] = TurnRelayBindSocket(&allocation->relayed[relay_slot]);
  if (allocation->relay_fd[relay_slot] >= 0 && clock_gettime(CLOCK_MONOTONIC, &allocation->deadline) == 0) {
    allocation->deadline.tv_sec += lifetime;
    error                       = 500;
    if (EncodeAllocateResponse(allocation, frame, auth, peer, request, request_length, reply, capacity, length) &&
      RegisterRelay(allocation, server, flow) && UdpServerSetFlowLeaseWithExpiry(
        server, flow, &allocation->deadline, OnAllocationLeaseExpired)) {
      session->alloc                    = allocation;
      char relay_text[NET_ADDR_STR_MAX] = {0};
      NetAddrFormat((const struct sockaddr*)&allocation->relayed[relay_slot], relay_text, sizeof(relay_text));
#if UFSRVSTUN_DEBUG_BUILD_L2
      syslog(LOG_DEBUG, "%s {id:'%llu', relay:'%s', lifetime:'%u'}: allocation created", __func__,
             (unsigned long long)allocation->id, relay_text, (unsigned)lifetime);
#endif
      return 0;
    }
  }

  ReleaseAllocation(allocation);
  return error;
}


static bool
EncodeRefreshResponse(const TurnDatagram *frame, const TurnAuthResult *auth, const uint8_t *request,
                      size_t request_length, uint32_t lifetime, uint8_t *reply, size_t capacity, size_t *length)
{
  const size_t required = STUN_HEADER_LENGTH + 8 + (auth->sign_sha256 ? 36 : 24) + (frame->fingerprint ? 8 : 0);
  if (capacity < required) return false;

  stun_tid transaction;
  stun_tid_from_message_str(request, request_length, &transaction);
  stun_init_success_response_str(STUN_METHOD_REFRESH, reply, length, &transaction);
  const uint32_t wire_lifetime = htonl(lifetime);
  return stun_attr_add_str(reply, length, STUN_ATTRIBUTE_LIFETIME, (const uint8_t*)&wire_lifetime,
                           sizeof(wire_lifetime)) && TurnAuthSignResponse(auth, reply, length, capacity) && (!frame->
    fingerprint || stun_attr_add_fingerprint_str(reply, length));
}

uint16_t
TurnRefresh(UdpServer *           server, UdpFlowHandle flow, TurnModelSession *session, const TurnDatagram *frame,
            const TurnAuthResult *auth, const uint8_t * request, size_t request_length, uint8_t *reply, size_t capacity,
            size_t *              length)
{
  if (!session) return 437;
  const uint8_t *        transaction = request + 8;
  const TurnPrincipalId *deleted_by  = &session->deallocation_principal;
  if (deleted_by->length == auth->credential.principal.length &&
    memcmp(deleted_by->bytes, auth->credential.principal.bytes, deleted_by->length) == 0 && memcmp(
      session->deallocation_transaction, transaction,
      sizeof(session->deallocation_transaction)) == 0) return EncodeRefreshResponse(frame, auth, request,
                                                                request_length, 0, reply, capacity, length) ?
                                                                0 :
                                                                500;

  TurnModelAllocation *allocation = session->alloc;
  if (!allocation) return 437;
  stun_attr_ref family = FindCoveredAttribute(request, request_length, frame->attributes_end,
                                              STUN_ATTRIBUTE_REQUESTED_ADDRESS_FAMILY);
  if (family) {
    const uint8_t requested = stun_attr_get_value(family)[0];
    if ((requested != 1 && requested != 2) || allocation->relay_fd[requested - 1] < 0) return 443;
  }

  if (allocation->refresh_lifetime && memcmp(allocation->refresh_transaction, transaction,
                                             sizeof(allocation->refresh_transaction)) == 0)
    return EncodeRefreshResponse(frame, auth, request, request_length, allocation->refresh_lifetime, reply, capacity,
                                 length) ?
             0 :
             500;

  const uint32_t lifetime = GrantedLifetime(frame, request, request_length, true);
  if (!EncodeRefreshResponse(frame, auth, request, request_length, lifetime, reply, capacity, length)) return 500;

  if (!lifetime) {
    if (!UdpServerSetFlowLease(server, flow, NULL)) return 500;
    memcpy(session->deallocation_transaction, transaction, sizeof(session->deallocation_transaction));
    session->deallocation_principal = auth->credential.principal;
    TurnAllocationClose(session);
    return 0;
  }

  struct timespec deadline;
  if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) return 500;
  deadline.tv_sec += lifetime;
  if (!UdpServerSetFlowLeaseWithExpiry(server, flow, &deadline, OnAllocationLeaseExpired)) return 500;

  pthread_mutex_lock(&allocations_lock);
  allocation->deadline = deadline;
  pthread_mutex_unlock(&allocations_lock);
  allocation->refresh_lifetime = lifetime;
  memcpy(allocation->refresh_transaction, transaction, sizeof(allocation->refresh_transaction));
  return 0;
}

static uint16_t
StagePermission(TurnModelAllocation *  allocation, TurnModelPermission **pending, size_t *added, const ioa_addr *peer,
                const struct timespec *deadline)
{
  const struct sockaddr *address = (const struct sockaddr*)peer;
  if (address->sa_family != allocation->relayed[RelaySlot(allocation)].ss_family) return 443;
  if (!PeerAllowed(allocation, address)) return 403;
  if (FindPermission(*pending, address)) return 0;

  if (!FindPermission(allocation->perms, address)) {
    (*added)++;
    if (allocation->permission_count + *added > UfsrvConfigStunTurnMaxPermissionsPerAllocation()) return 508;
  }

  TurnModelPermission *permission = calloc(1, sizeof(*permission));
  if (!permission) return 508;
  NetAddrCopy(&permission->peer_ip, address);
  NetAddrSetPort((struct sockaddr*)&permission->peer_ip, 0);
  permission->deadline = *deadline;
  permission->next     = *pending;
  *pending             = permission;
  return 0;
}

static void
PrunePermissions(TurnModelAllocation *allocation, const struct timespec *now)
{
  TurnModelPermission **slot = &allocation->perms;
  while (*slot) {
    TurnModelPermission *permission = *slot;
    if (Before(now, &permission->deadline)) {
      slot = &permission->next;
    }
    else {
      *slot = permission->next;
      free(permission);
      allocation->permission_count--;
    }
  }
}

static void
ApplyPermissions(TurnModelAllocation *allocation, TurnModelPermission *pending)
{
  while (pending) {
    TurnModelPermission *permission = pending;
    pending                         = pending->next;
    TurnModelPermission *existing   = FindPermission(allocation->perms, (const struct sockaddr*)&permission->peer_ip);
    if (existing) {
      existing->deadline = permission->deadline;
      free(permission);
    }
    else {
      permission->next  = allocation->perms;
      allocation->perms = permission;
      allocation->permission_count++;
    }
  }
}

uint16_t
TurnCreatePermission(TurnModelSession *session, const TurnDatagram *frame, const TurnAuthResult *auth,
                     const uint8_t *   request, size_t request_length, uint8_t *reply, size_t capacity, size_t *length)
{
  if (!session || !session->alloc) return 437;
  TurnModelAllocation *allocation = session->alloc;
  struct timespec      now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 500;
  if (!Before(&now, &allocation->deadline)) return 437;
  struct timespec deadline = now;
  deadline.tv_sec          += TURN_MODEL_PERMISSION_LIFETIME;

  const size_t required = STUN_HEADER_LENGTH + (auth->sign_sha256 ? 36 : 24) + (frame->fingerprint ? 8 : 0);
  if (capacity < required) return 500;

  TurnModelPermission *pending = NULL;
  size_t               added   = 0;
  uint16_t             error   = 0;
  pthread_mutex_lock(&allocations_lock);
  PrunePermissions(allocation, &now);
  for (stun_attr_ref attr = stun_attr_get_first_str(request, request_length); attr && (
         size_t)((const uint8_t*)attr - request) < frame->attributes_end; attr = stun_attr_get_next_str(
         request, request_length, attr)) {
    if (stun_attr_get_type(attr) != STUN_ATTRIBUTE_XOR_PEER_ADDRESS) continue;
    ioa_addr peer = {0};
    if (!stun_attr_get_addr_str(request, request_length, attr, &peer, NULL)) {
      error = 400;
      break;
    }
    error = StagePermission(allocation, &pending, &added, &peer, &deadline);
    if (error) break;
  }
  if (!error && !pending) error = 400;
  if (!error) {
    stun_tid transaction;
    stun_tid_from_message_str(request, request_length, &transaction);
    stun_init_success_response_str(STUN_METHOD_CREATE_PERMISSION, reply, length, &transaction);
    if (!TurnAuthSignResponse(auth, reply, length, capacity) || (frame->fingerprint && !
      stun_attr_add_fingerprint_str(reply, length))) error = 500;
  }
  if (error) FreePermissions(pending);
  else ApplyPermissions(allocation, pending);
  pthread_mutex_unlock(&allocations_lock);
  return error;
}

void
TurnRelaySend(TurnModelSession *session, const uint8_t *request, size_t length)
{
  if (!session || !session->alloc || stun_attr_get_first_by_type_str(request, length, STUN_ATTRIBUTE_DONT_FRAGMENT))
    return;
  stun_attr_ref address = stun_attr_get_first_by_type_str(request, length, STUN_ATTRIBUTE_XOR_PEER_ADDRESS);
  stun_attr_ref data    = stun_attr_get_first_by_type_str(request, length, STUN_ATTRIBUTE_DATA);
  ioa_addr      peer    = {0};
  if (!address || !data || !stun_attr_get_addr_str(request, length, address, &peer, NULL)) return;
  const struct sockaddr *target = (const struct sockaddr*)&peer;
  const int              size   = stun_attr_get_len(data);
  if (size < 0 || !NetAddrPort(target)) return;

  TurnModelAllocation *allocation = session->alloc;
  pthread_mutex_lock(&allocations_lock);
  struct timespec now;
  char            peer_text[NET_ADDR_STR_MAX] = {0};
  NetAddrFormat(target, peer_text, sizeof(peer_text));
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    syslog(LOG_WARNING, "%s {id:'%llu'}: could not read the monotonic clock", __func__,
           (unsigned long long)allocation->id);
  }
  else if (!HasPermission(allocation, target, &now)) {
#if UFSRVSTUN_DEBUG_BUILD_L3
    syslog(LOG_DEBUG, "%s {id:'%llu'}: peer '%s' has no permission or it expired", __func__,
           (unsigned long long)allocation->id, peer_text);
#endif
  }
  else if (sendto(allocation->relay_fd[RelaySlot(allocation)], stun_attr_get_value(data), (size_t)size, 0, target,
                  (socklen_t)NetAddrLength(target)) < 0) {
    syslog(LOG_WARNING, "%s {id:'%llu'}: relay send to '%s' failed: %s", __func__, (unsigned long long)allocation->id,
           peer_text, strerror(errno));
  }
  else {
#if UFSRVSTUN_DEBUG_BUILD_L3
    syslog(LOG_DEBUG, "%s {id:'%llu'}: relayed '%zu' bytes to '%s'", __func__, (unsigned long long)allocation->id,
           (size_t)size, peer_text);
#endif
  }
  pthread_mutex_unlock(&allocations_lock);
}

uint16_t
TurnChannelBind(TurnModelSession *session, const TurnDatagram *frame, const TurnAuthResult *auth,
                const uint8_t *   request, size_t request_length, uint8_t *reply, size_t capacity, size_t *length)
{
  if (!session || !session->alloc) return 437;

  const size_t covered = frame->attributes_end;
  stun_attr_ref number_attr = FindCoveredAttribute(request, request_length, covered, STUN_ATTRIBUTE_CHANNEL_NUMBER);
  stun_attr_ref peer_attr = FindCoveredAttribute(request, request_length, covered, STUN_ATTRIBUTE_XOR_PEER_ADDRESS);
  ioa_addr peer = {0};
  const struct sockaddr *address = (const struct sockaddr*)&peer;
  if (!number_attr || !peer_attr || !stun_attr_get_addr_str(request, request_length, peer_attr, &peer, NULL) || !
    NetAddrPort(address)) return 400;
  const uint16_t number = turn_read_u16(stun_attr_get_value(number_attr));

  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 500;
  TurnModelAllocation *allocation = session->alloc;
  if (!Before(&now, &allocation->deadline)) return 437;
  if (address->sa_family != allocation->relayed[RelaySlot(allocation)].ss_family) return 443;
  if (!PeerAllowed(allocation, address)) return 403;

  const size_t required = STUN_HEADER_LENGTH + (auth->sign_sha256 ? 36 : 24) + (frame->fingerprint ? 8 : 0);
  if (capacity < required) return 500;

  uint16_t             error   = 0;
  TurnModelChannel *   created = NULL;
  TurnModelPermission *pending = NULL;
  pthread_mutex_lock(&allocations_lock);
  if (!allocation->channels) allocation->channels = calloc(1, sizeof(*allocation->channels));
  TurnModelChannelIndex *index = allocation->channels;
  if (!index) {
    error = 508;
    goto done;
  }

  PruneChannels(index, &now);
  TurnModelChannel *channel = FindChannelNumber(index, number);
  if (channel != FindChannelPeer(index, address)) {
    error = 400;
    goto done;
  }
  if (!channel) {
    if (index->count >= UfsrvConfigStunTurnMaxChannelsPerAllocation()) {
      error = 508;
      goto done;
    }
    created = calloc(1, sizeof(*created));
    if (!created) {
      error = 508;
      goto done;
    }
    created->chnum = number;
    NetAddrCopy(&created->peer, address);
    channel = created;
  }

  PrunePermissions(allocation, &now);
  struct timespec permission_deadline = now;
  permission_deadline.tv_sec          += TURN_MODEL_PERMISSION_LIFETIME;
  size_t added                        = 0;
  error                               = StagePermission(allocation, &pending, &added, &peer, &permission_deadline);
  if (error) goto done;

  stun_tid transaction;
  stun_tid_from_message_str(request, request_length, &transaction);
  stun_init_success_response_str(STUN_METHOD_CHANNEL_BIND, reply, length, &transaction);
  if (!TurnAuthSignResponse(auth, reply, length, capacity) || (frame->fingerprint && !
    stun_attr_add_fingerprint_str(reply, length))) {
    error = 500;
    goto done;
  }

  channel->deadline        = now;
  channel->deadline.tv_sec += TURN_MODEL_CHANNEL_LIFETIME;
  if (created) {
    InsertChannel(index, created);
    created = NULL;
  }
  ApplyPermissions(allocation, pending);
  pending = NULL;

done:
  free(created);
  FreePermissions(pending);
  pthread_mutex_unlock(&allocations_lock);
  return error;
}

void
TurnRelayChannelData(TurnModelSession *session, const TurnDatagram *frame, const uint8_t *packet)
{
  if (!session || !session->alloc) return;
  TurnModelAllocation *allocation = session->alloc;
  struct timespec      now;
  pthread_mutex_lock(&allocations_lock);
  TurnModelChannel *channel = allocation->channels ? FindChannelNumber(allocation->channels, frame->channel) : NULL;
  if (channel && clock_gettime(CLOCK_MONOTONIC, &now) == 0 && Before(&now, &channel->deadline)) {
    const struct sockaddr *peer                        = (const struct sockaddr*)&channel->peer;
    char                   peer_text[NET_ADDR_STR_MAX] = {0};
    NetAddrFormat(peer, peer_text, sizeof(peer_text));
    if (!HasPermission(allocation, peer, &now)) {
#if UFSRVSTUN_DEBUG_BUILD_L3
      syslog(LOG_DEBUG, "%s {id:'%llu', ch:'%u'}: peer '%s' has no permission or it expired", __func__,
             (unsigned long long)allocation->id, (unsigned)channel->chnum, peer_text);
#endif
    }
    else if (sendto(allocation->relay_fd[RelaySlot(allocation)], packet + STUN_CHANNEL_HEADER_LENGTH,
                    frame->channel_payload_length, 0, peer, (socklen_t)NetAddrLength(peer)) < 0) {
      syslog(LOG_WARNING, "%s {id:'%llu', ch:'%u'}: channel send to '%s' failed: %s", __func__,
             (unsigned long long)allocation->id, (unsigned)channel->chnum, peer_text, strerror(errno));
    }
    else {
#if UFSRVSTUN_DEBUG_BUILD_L3
      syslog(LOG_DEBUG, "%s {id:'%llu', ch:'%u'}: relayed '%zu' bytes to '%s'", __func__,
             (unsigned long long)allocation->id, (unsigned)channel->chnum, frame->channel_payload_length, peer_text);
#endif
    }
  }
  pthread_mutex_unlock(&allocations_lock);
}
