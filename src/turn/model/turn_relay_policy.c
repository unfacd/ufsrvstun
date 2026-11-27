#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <uflib/collection_descriptor_type.h>

#include "net_addr.h"
#include "stun_diagnostics.h"
#include "turn/model/turn_relay_policy.h"
#include "ufsrvstun_config.h"
#include "main_protocols.h"

typedef struct TurnPeerPrefix
{
  struct sockaddr_storage network;
  unsigned                bits;
} TurnPeerPrefix;

typedef struct TurnPeerPrefixes
{
  TurnPeerPrefix *items;
  size_t          count;
} TurnPeerPrefixes;

static struct sockaddr_storage bind_address;
static struct sockaddr_storage advertised_address;
static TurnPeerPrefixes        allowed_peers;
static TurnPeerPrefixes        denied_peers;
static unsigned                min_port;
static unsigned                max_port;
static atomic_uint             port_cursor;

static bool
ParsePrefix(const char *cidr, TurnPeerPrefix *prefix)
{
  const char *slash = strchr(cidr, '/');
  char        address[INET6_ADDRSTRLEN];
  if (!slash || slash == cidr || (size_t)(slash - cidr) >= sizeof(address)) {
    syslog(LOG_ERR, "%s: malformed CIDR '%s' (expected address/bits)", __func__, cidr);
    return false;
  }
  size_t length = (size_t)(slash - cidr);
  memcpy(address, cidr, length);
  address[length]  = '\0';
  const char *bits = slash + 1;
  if (!*bits || bits[strspn(bits, "0123456789")] || !NetAddrParseHost(address, 0, &prefix->network)) {
    syslog(LOG_ERR, "%s: unparsable CIDR '%s'", __func__, cidr);
    return false;
  }
  struct sockaddr_storage mapped;
  if (NetAddrEmbeddedIPv4((const struct sockaddr*)&prefix->network, &mapped)) return false;
  unsigned long parsed   = strtoul(bits, NULL, 10);
  unsigned      max_bits = prefix->network.ss_family == AF_INET ? 32 : 128;
  if (parsed > max_bits) {
    syslog(LOG_ERR, "%s: CIDR '%s' has %lu bits, more than the %u the family allows", __func__, cidr, parsed, max_bits);
    return false;
  }
  prefix->bits = (unsigned)parsed;
  return true;
}

static bool
LoadPrefixes(const CollectionDescriptor *source, TurnPeerPrefixes *target, const char *key)
{
  if (!source) return false;
  if (!source->collection_sz) return true;

  target->items = calloc(source->collection_sz, sizeof(*target->items));
  if (!target->items) return false;
  for (size_t i = 0; i < source->collection_sz; i++) {
    if (!ParsePrefix(source->collection[i], &target->items[i])) {
      fprintf(stderr, "turn.%s contains an invalid CIDR at entry %zu\n", key, i + 1);
      return false;
    }
  }
  target->count = source->collection_sz;
  return true;
}

void
TurnRelayPolicyDestroy(void)
{
  free(allowed_peers.items);
  free(denied_peers.items);
  allowed_peers = (TurnPeerPrefixes){0};
  denied_peers  = (TurnPeerPrefixes){0};
}

bool
TurnRelayPolicyLoad(const char *fallback_host)
{
  const char *host          = UfsrvConfigStunTurnRelayBindAddress();
  const char *advertised    = UfsrvConfigStunTurnRelayAdvertisedAddress();
  const bool  explicit_bind = *host != '\0';
  if (!explicit_bind) host = fallback_host;
  if (!host || !NetAddrParseHost(host, 0, &bind_address) || ((explicit_bind || UfsrvConfigStunTurnIsEnabled()) && (
    NetAddrIsUnspecified((const struct sockaddr*)&bind_address) || NetAddrIsMulticast(
      (const struct sockaddr*)&bind_address)))) {
    STUN_CONFIG_ERR("%s", "turn.relay_bind_address or its listener fallback must be a concrete numeric IP address");
    return false;
  }
  advertised_address = bind_address;
  if (*advertised && (!NetAddrParseHost(advertised, 0, &advertised_address) || advertised_address.ss_family !=
    bind_address.ss_family || NetAddrIsUnspecified((const struct sockaddr*)&advertised_address) || NetAddrIsMulticast(
      (const struct sockaddr*)&advertised_address))) {
    STUN_CONFIG_ERR("%s", "turn.relay_advertised_address must be a concrete numeric IP in the relay bind family");
    return false;
  }

  min_port = UfsrvConfigStunTurnRelayMinPort();
  max_port = UfsrvConfigStunTurnRelayMaxPort();
  if ((!min_port && max_port) || min_port > max_port || max_port > UINT16_MAX) {
    STUN_CONFIG_ERR("%s", "TURN relay ports must both be zero or form an ascending nonzero UDP port range");
    return false;
  }

  if (!LoadPrefixes(UfsrvConfigStunTurnPeerAllowCidrsAsPtr(), &allowed_peers, "peer_allow_cidrs") || !LoadPrefixes(
    UfsrvConfigStunTurnPeerDenyCidrsAsPtr(), &denied_peers, "peer_deny_cidrs")) {
    TurnRelayPolicyDestroy();
    return false;
  }
  return true;
}

int
TurnRelayAddressFamily(void)
{
  return bind_address.ss_family;
}

int
TurnRelayBindSocket(struct sockaddr_storage *advertised)
{
  struct sockaddr_storage local = bind_address;
  const int               fd    = socket(local.ss_family, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_UDP);
  if (fd < 0) return -1;

  socklen_t      length = (socklen_t)NetAddrLength((const struct sockaddr*)&local);
  const unsigned count  = min_port ? max_port - min_port + 1 : 1;
  const unsigned start  = min_port ? atomic_fetch_add_explicit(&port_cursor, 1, memory_order_relaxed) % count : 0;
  for (unsigned i = 0; i < count; i++) {
    NetAddrSetPort((struct sockaddr*)&local, (uint16_t)(min_port + (start + i) % count));
    if (bind(fd, (const struct sockaddr*)&local, length) == 0) {
      if (getsockname(fd, (struct sockaddr*)&local, &length) == 0) {
        *advertised = advertised_address;
        NetAddrSetPort((struct sockaddr*)advertised, NetAddrPort((const struct sockaddr*)&local));
        return fd;
      }
      break;
    }
    if (errno != EADDRINUSE) break;
  }
  close(fd);
  return -1;
}

static const uint8_t *
AddressBytes(const struct sockaddr *address)
{
  if (address->sa_family == AF_INET) return (const uint8_t*)&((const struct sockaddr_in*)address)->sin_addr;
  return (const uint8_t*)&((const struct sockaddr_in6*)address)->sin6_addr;
}

static bool
MatchesPrefix(const TurnPeerPrefixes *prefixes, const struct sockaddr *peer)
{
  const uint8_t *address = AddressBytes(peer);
  for (size_t i = 0; i < prefixes->count; i++) {
    const TurnPeerPrefix *prefix = &prefixes->items[i];
    if (prefix->network.ss_family != peer->sa_family) continue;
    const uint8_t *network = AddressBytes((const struct sockaddr*)&prefix->network);
    const unsigned bytes   = prefix->bits / 8;
    if (memcmp(address, network, bytes) != 0) continue;
    const unsigned tail = prefix->bits % 8;
    if (!tail || ((address[bytes] ^ network[bytes]) >> (8 - tail)) == 0) return true;
  }
  return false;
}

bool
TurnRelayPeerAllowed(const struct sockaddr *peer)
{
  struct sockaddr_storage mapped;
  peer = NetAddrCanonicalize(peer, &mapped);
  if (!NetAddrLength(peer) || NetAddrIsUnspecified(peer) || NetAddrIsMulticast(peer) || NetAddrIsInternalDeny(peer))
    return false;
  return !MatchesPrefix(&denied_peers, peer) && (!allowed_peers.count || MatchesPrefix(&allowed_peers, peer));
}
