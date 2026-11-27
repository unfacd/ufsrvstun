/**
 * @file protocol_stun.c
 * @brief Protocol descriptor for plain STUN over UDP.
 */

#include "protocol_stun.h"
#include "protocol_context.h"
#include "net_addr.h"
#include "net_addr.h"
#include "turn/policy/turn_dispatch.h"
#include "turn/auth/turn_auth.h"
#include "turn/model/turn_allocation.h"
#include "turn/model/turn_relay_policy.h"
#include "ufsrvstun_config.h"
#include <ufsrvcorelib/ufsrv_core/turn/codec/ns_turn_msg.h>

#include <netinet/in.h>
#include <pthread.h>
#include <syslog.h>
#include <string.h>
#include <time.h>
#include <ufsrv_core/protocol/protocol.h>

static const TurnAuthContext *turn_auth;
static StunProtocolContext *   stun_ctx;

static const UfsrvListenerDescriptor *
sListenerFor(const Protocol *protocol)
{
  CollectionDescriptor listeners = UfsrvConfigListeners();

  for (size_t i = 0; i < listeners.collection_sz; i++) {
    const UfsrvListenerDescriptor *listener = (const UfsrvListenerDescriptor *)listeners.collection[i];
    if (listener && listener->protocol == protocol) return listener;
  }

  return NULL;
}

static bool
sEndpoint(const UfsrvListenerDescriptor *listener, struct sockaddr_storage *address)
{
  return listener && listener->bind_address && listener->port &&
         NetAddrParseHost(listener->bind_address, listener->port, address);
}

static bool
sEndpointsOverlap(const struct sockaddr_storage *plain, const struct sockaddr_storage *secure)
{
  const struct sockaddr *a = (const struct sockaddr *)plain;
  const struct sockaddr *b = (const struct sockaddr *)secure;
  if (NetAddrPort(a) != NetAddrPort(b)) return false;

  struct sockaddr_storage mapped_a, mapped_b;
  if (a->sa_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&((const struct sockaddr_in6 *)a)->sin6_addr) &&
      NetAddrEmbeddedIPv4(a, &mapped_a)) a = (const struct sockaddr *)&mapped_a;
  if (b->sa_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&((const struct sockaddr_in6 *)b)->sin6_addr) &&
      NetAddrEmbeddedIPv4(b, &mapped_b)) b = (const struct sockaddr *)&mapped_b;

  return NetAddrEqualNoPort(a, b) ||
         (NetAddrIsUnspecified(a) && (a->sa_family == AF_INET6 || b->sa_family == AF_INET));
}

static const struct sockaddr *
StunPeerAddress(const struct sockaddr *peer, socklen_t peer_len, struct sockaddr_storage *mapped)
{
  if (!peer || peer_len < sizeof(peer->sa_family)) return NULL;
  const size_t length = NetAddrLength(peer);
  if (!length || peer_len < length) return NULL;

  if (peer->sa_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&((const struct sockaddr_in6 *)peer)->sin6_addr) &&
    NetAddrEmbeddedIPv4(peer, mapped)) return (const struct sockaddr*)mapped;
  return peer;
}

/* ChannelData and TURN Send indications carry application payloads, not STUN
 * requests, so the request-size cap must not bound them. */
static bool
sCarriesPayload(const unsigned char *input, size_t input_len, bool turn_enabled)
{
  if (!turn_enabled || !input || input_len < STUN_CHANNEL_HEADER_LENGTH) return false;
  if ((input[0] & 0xc0) == 0x40) return true;
  return stun_is_indication_str(input, input_len) && stun_get_method_str(input, input_len) == STUN_METHOD_SEND;
}

static bool
StunAdmitDatagram(UdpServer *server, const struct sockaddr *peer, socklen_t peer_len, const unsigned char *input,
                  size_t input_len)
{
  StunProtocolContext *ctx = UdpServerProtocolContext(server);
  if (!ctx) return false;

  const size_t min_length = UfsrvConfigStunTurnIsEnabled() ? STUN_CHANNEL_HEADER_LENGTH : STUN_HEADER_LENGTH;
  const bool   turn_enabled = UfsrvConfigStunTurnIsEnabled();
  const size_t max_length   = sCarriesPayload(input, input_len, turn_enabled) ? 65507u
                                                                               : UfsrvConfigStunMaxRequestBytes();
  if (input_len < min_length || input_len > max_length) return false;
  struct sockaddr_storage mapped;
  const struct sockaddr * address = StunPeerAddress(peer, peer_len, &mapped);
  if (!address) return false;

  struct timespec time;
  if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) return false;
  const double now   = (double)time.tv_sec + (double)time.tv_nsec / 1000000000.0;
  const double rate  = (double)UfsrvConfigStunBindingRateLimitPerSource();
  const double burst = (double)UfsrvConfigStunBindingRateLimitBurst();

  const uint32_t   hash   = NetAddrHashNoPort(address);
  StunSourceLimit *bucket = ctx->limits[hash % STUN_LIMIT_BUCKETS];

  pthread_mutex_lock(&ctx->limits_lock);
  StunSourceLimit *source    = NULL;
  StunSourceLimit *available = NULL;
  for (size_t i = 0; i < STUN_LIMITS_PER_BUCKET; i++) {
    StunSourceLimit *entry = &bucket[i];
    if (entry->occupied && NetAddrEqualNoPort((const struct sockaddr*)&entry->address, address)) {
      source = entry;
      break;
    }
    if (!available && (!entry->occupied || now - entry->updated_at >= burst / rate)) available = entry;
  }
  if (!source && available) {
    *available = (StunSourceLimit){.occupied = true, .updated_at = now, .tokens = burst};
    NetAddrCopy(&available->address, address);
    source = available;
  }

  bool admit = false;
  if (source) {
    // A shard can obtain its timestamp before another shard takes the lock.
    if (now > source->updated_at) {
      source->tokens += (now - source->updated_at) * rate;
      if (source->tokens > burst) source->tokens = burst;
      source->updated_at = now;
    }
    if (source->tokens >= 1.0) {
      source->tokens -= 1.0;
      admit          = true;
    }
  }
  pthread_mutex_unlock(&ctx->limits_lock);
#if UFSRVSTUN_DEBUG_BUILD_L3
  if (!admit) {
    char peer_text[NET_ADDR_STR_MAX] = {0};
    NetAddrFormat(address, peer_text, sizeof(peer_text));
    syslog(LOG_DEBUG, "%s: binding rate limit denied '%s'", __func__, peer_text);
  }
#endif
  return admit;
}

static size_t
StunBindingRequest(const struct sockaddr *peer, socklen_t peer_len, const unsigned char *input, size_t input_len,
                   bool                   fingerprint, unsigned char *reply, size_t reply_capacity)
{
  struct sockaddr_storage mapped;
  const struct sockaddr * source = StunPeerAddress(peer, peer_len, &mapped);
  if (!source) return 0;

  // Account for the full address attribute and optional fingerprint before encoding.
  const size_t address_attribute_size = source->sa_family == AF_INET ? 12 : 24;
  const size_t required               = STUN_HEADER_LENGTH + address_attribute_size + (fingerprint ? 8 : 0);
  if (reply_capacity < required) return 0;

  ioa_addr address = {0};
  memcpy(&address, source, NetAddrLength(source));
  stun_tid transaction;
  stun_tid_from_message_str(input, input_len, &transaction);

  size_t reply_len = 0;
  if (!stun_set_binding_response_str(reply, &reply_len, &transaction, &address, 0, NULL, STUN_MAGIC_COOKIE, false,
                                     false, false)) return 0;
  if (fingerprint && !stun_attr_add_fingerprint_str(reply, &reply_len)) return 0;
  return reply_len;
}

static size_t
DispatchTurnRequest(UdpServer *server, UdpFlowHandle    flow, const TurnDatagram *frame, const struct sockaddr *peer,
                    socklen_t  peer_len, const uint8_t *input, size_t input_len, uint8_t *reply, size_t capacity)
{
  TurnAuthResult auth;
  TurnAuthStatus status = TurnAuthenticateRequest(turn_auth, frame, peer, peer_len, input, input_len, time(NULL), reply,
                                                  capacity, &auth);
  size_t length = auth.response_length;
  if (status == TURN_AUTH_VERIFIED) {
    TurnModelSession *session = TurnSessionForFlow(server, flow);
    if (!session && frame->policy->method == STUN_METHOD_ALLOCATE) session = TurnSessionAttach(
      server, flow, &auth.credential.principal);

    uint16_t error = 500;
    if (session && session->alloc && (session->principal.length != auth.credential.principal.length || memcmp(
      session->principal.bytes, auth.credential.principal.bytes, session->principal.length) != 0)) error = 441;
    if (error != 441 && session && frame->policy->method == STUN_METHOD_ALLOCATE) {
      if (!session->alloc) session->principal = auth.credential.principal;
      struct sockaddr_storage mapped;
      const struct sockaddr * client = StunPeerAddress(peer, peer_len, &mapped);
      if (client)
        error = TurnAllocate(server, flow, session, frame, &auth, client, input, input_len, reply, capacity, &length);
    }
    if (error != 441 && frame->policy->method == STUN_METHOD_REFRESH) error = TurnRefresh(
      server, flow, session, frame, &auth, input, input_len, reply, capacity, &length);
    if (error != 441 && frame->policy->method == STUN_METHOD_CREATE_PERMISSION) error = TurnCreatePermission(
      session, frame, &auth, input, input_len, reply, capacity, &length);
    if (error != 441 && frame->policy->method == STUN_METHOD_CHANNEL_BIND) error = TurnChannelBind(
      session, frame, &auth, input, input_len, reply, capacity, &length);
    if (!error) {
      TurnAuthResultClear(&auth);
      return length;
    }
    length                = 0;
    const size_t required = STUN_HEADER_LENGTH + 8 + (auth.sign_sha256 ? 36 : 24) + (frame->fingerprint ? 8 : 0) + (
      error == 420 ? 8 : 0);
    if (capacity >= required) {
      stun_tid transaction;
      stun_tid_from_message_str(input, input_len, &transaction);
      stun_init_error_response_str(frame->policy->method, reply, &length, error, NULL, &transaction, false);
      const uint16_t unsupported = htons(STUN_ATTRIBUTE_DONT_FRAGMENT);
      if ((error == 420 && !stun_attr_add_str(reply, &length, STUN_ATTRIBUTE_UNKNOWN_ATTRIBUTES,
                                              (const uint8_t*)&unsupported, sizeof(unsupported))) ||
        !TurnAuthSignResponse(&auth, reply, &length, capacity) || (frame->fingerprint && !
          stun_attr_add_fingerprint_str(reply, &length))) length = 0;
    }
  }
  TurnAuthResultClear(&auth);
  return length;
}

/* A request that fails validation is answered, not dropped: 420 lists the
 * comprehension-required attributes the server does not understand, 400 covers
 * every other rule violation. */
static size_t
sErrorResponse(const unsigned char *input, size_t input_len, TurnDecodeStatus status, const TurnDatagram *frame,
               unsigned char *reply, size_t capacity)
{
  uint16_t code = status == TURN_DECODE_UNKNOWN_ATTRIBUTES ? 420 : 400;
  size_t   unknown_bytes = frame->unknown_required_count * sizeof(uint16_t);
  if (capacity < STUN_HEADER_LENGTH + 4 + unknown_bytes) return 0;

  stun_tid transaction;
  stun_tid_from_message_str(input, input_len, &transaction);

  size_t length = 0;
  stun_init_error_response_str(stun_get_method_str(input, input_len), reply, &length, code, NULL, &transaction, false);

  if (code == 420 && unknown_bytes) {
    uint16_t types[sizeof(frame->unknown_required) / sizeof(frame->unknown_required[0])];
    for (size_t i = 0; i < frame->unknown_required_count; i++) types[i] = htons(frame->unknown_required[i]);
    if (!stun_attr_add_str(reply, &length, STUN_ATTRIBUTE_UNKNOWN_ATTRIBUTES, (const uint8_t *)types, unknown_bytes))
      return 0;
  }

  return length;
}

static size_t
StunDispatchFlowDatagram(UdpServer *server, UdpFlowHandle flow, const struct sockaddr *peer, socklen_t peer_len,
                         const unsigned char *input, size_t input_len, unsigned char *reply, size_t reply_capacity)
{
  TurnAllocationExpire(TurnSessionForFlow(server, flow));
  TurnDatagram frame;
  const TurnDecodeStatus status = TurnDecodeDatagram(input, input_len, UfsrvConfigStunTurnIsEnabled(), &frame);
  if (status != TURN_DECODE_OK) {
    if (status != TURN_DECODE_IGNORE && stun_is_request_str(input, input_len)) {
      return sErrorResponse(input, input_len, status, &frame, reply, reply_capacity);
    }
    return 0;
  }

  if (frame.kind == TURN_DATAGRAM_BINDING) {
    if (!frame.fingerprint && strcmp(UfsrvConfigStunFingerprintPolicy(), "required") == 0) return 0;
    return StunBindingRequest(peer, peer_len, input, input_len, frame.fingerprint, reply, reply_capacity);
  }
  if (frame.kind == TURN_DATAGRAM_CHANNEL_DATA) {
    TurnRelayChannelData(TurnSessionForFlow(server, flow), &frame, input);
    return 0;
  }
  if (frame.kind == TURN_DATAGRAM_SEND) {
    TurnRelaySend(TurnSessionForFlow(server, flow), input, input_len);
    return 0;
  }
  if (frame.policy && frame.policy->auth_mode == TURN_AUTH_LONG_TERM) return DispatchTurnRequest(
    server, flow, &frame, peer, peer_len, input, input_len, reply, reply_capacity);
  return 0;
}

static size_t
StunDispatchDatagram(const struct sockaddr *peer, socklen_t peer_len, const unsigned char *input, size_t input_len,
                     unsigned char *        reply, size_t   reply_capacity)
{
  return StunDispatchFlowDatagram(NULL, (UdpFlowHandle){0}, peer, peer_len, input, input_len, reply, reply_capacity);
}

bool
StunProtocolInit(const TurnAuthContext *auth)
{
  const UfsrvListenerDescriptor *plain = sListenerFor(UfsrvStunProtocol());
  struct sockaddr_storage       plain_address;
  if (!sEndpoint(plain, &plain_address)) {
    syslog(LOG_ERR, "%s: protocol 'Stun' declares no usable listener", __func__);
    return false;
  }

  if (UfsrvConfigStunDtlsIsEnabled()) {
    const UfsrvListenerDescriptor *dtls = sListenerFor(UfsrvDtlsProtocol());
    struct sockaddr_storage       dtls_address;
    if (!sEndpoint(dtls, &dtls_address)) {
      syslog(LOG_ERR, "%s: protocol 'StunDtls' declares no usable listener", __func__);
      return false;
    }
    if (sEndpointsOverlap(&plain_address, &dtls_address)) {
      syslog(LOG_ERR, "%s: plain and DTLS listeners must use distinct endpoints", __func__);
      return false;
    }
  }

  if (auth && (!TurnRelayPolicyLoad(plain->bind_address) || !TurnSessionPoolInit(UfsrvConfigMemorySpecsAllocationGroups(),
                                                             UfsrvConfigMemorySpecsSessionsPerAllocationGroup())))
    return false;
  if (auth && !TurnRelayStart()) return false;

  StunProtocolContext *ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return false;
  if (pthread_mutex_init(&ctx->limits_lock, NULL) != 0) {
    free(ctx);
    return false;
  }

  turn_auth = auth;
  stun_ctx  = ctx;
  return true;
}

void
StunProtocolStop(void)
{
  TurnRelayStop();

  if (stun_ctx) {
    pthread_mutex_destroy(&stun_ctx->limits_lock);
    free(stun_ctx);
    stun_ctx = NULL;
  }
  turn_auth = NULL;
}

UdpServer *
StunProtocolStart(bool dtls, unsigned char *worker_slots, size_t worker_count)
{
  const UfsrvListenerDescriptor *listener = sListenerFor(dtls ? UfsrvDtlsProtocol() : UfsrvStunProtocol());
  if (!listener || !listener->bind_address || !listener->port) return NULL;

  UdpServerSpec spec = {
    .protocol_context_data = stun_ctx,
    .shard_count = UfsrvConfigStunShardCount(), .workers_per_shard = UfsrvConfigStunWorkersPerShard(),
    .bind_host = listener->bind_address,
    .port = listener->port,
    .protocol_id = dtls ? DTLS_PROTOCOL_ID : STUN_PROTOCOL_ID, .worker_slots = worker_slots,
    .slot_count = worker_count,
    .slot_size = sizeof(worker_slots[0]), .admit_datagram = StunAdmitDatagram,
    .shard_hash_table_startup_size = UfsrvConfigStunShardHashTableStartupSize(),
  };

  if (dtls) {
    spec.on_flow_datagram          = StunDispatchFlowDatagram;
    spec.dtls_chain_file           = UfsrvConfigStunDtlsCertificateChainFile();
    spec.dtls_key_file             = UfsrvConfigStunDtlsPrivateKeyFile();
    spec.flows_per_shard           = UfsrvConfigStunDtlsFlowsPerShard();
    spec.flow_idle_timeout_seconds = UfsrvConfigStunDtlsFlowIdleTimeoutSeconds();
    spec.sends_per_shard           = UfsrvConfigStunDtlsSendsPerShard();
  }
  else if (UfsrvConfigStunTurnIsEnabled()) {
    spec.on_flow_datagram          = StunDispatchFlowDatagram;
    spec.flows_per_shard           = UfsrvConfigStunTurnFlowsPerShard();
    spec.flow_idle_timeout_seconds = UfsrvConfigStunTurnFlowIdleTimeoutSeconds();
    spec.sends_per_shard           = UfsrvConfigStunTurnSendsPerShard();
  }
  else {
    spec.on_datagram = StunDispatchDatagram;
  }
  if (turn_auth) spec.on_flow_session_release = TurnSessionRelease;

  UdpServer *server = UdpServerStart(&spec);
  if (server) {
    syslog(LOG_INFO, "%s: listening on %s:%u%s", __func__, spec.bind_host, spec.port, dtls ? " (DTLS)" : "");
  }
  return server;
}
