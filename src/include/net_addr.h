/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * net_addr.h - IPv4/IPv6 address utilities, header-only.
 *
 * Single-file module for servers that need correct dual-stack address
 * handling without the STUN/TURN codec. Derived from coturn 4.19.2
 * (src/client/ns_turn_ioaddr.c), Copyright (C) 2011, 2012, 2013 Citrix
 * Systems, BSD-3-Clause; see the coturn LICENSE.
 *
 * Design notes:
 *  - No new address type. Everything takes plain `struct sockaddr *`, so it
 *    interoperates with `sockaddr_storage`, `sockaddr_in`, `ioa_addr`, etc.
 *  - The classification helpers canonicalise IPv4-in-IPv6 encodings first
 *    (::ffff:0:0/96, ::/96, 2002::/16, 64:ff9b::/96), so an ACL cannot be
 *    dodged by re-encoding the same IPv4 target.
 *  - All functions are `static inline`, so this is literally one file and
 *    needs no build-system change. Include it where needed.
 */

#ifndef NET_ADDR_H
#define NET_ADDR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest formatted output: "[ffff:...:ffff]:65535" plus NUL. */
#define NET_ADDR_STR_MAX 64

/* ---------------------------------------------------------------- basics */

static inline size_t NetAddrLength(const struct sockaddr *sa) {
  if (!sa) {
    return 0;
  }
  switch (sa->sa_family) {
  case AF_INET:
    return sizeof(struct sockaddr_in);
  case AF_INET6:
    return sizeof(struct sockaddr_in6);
  default:
    return 0;
  }
}

static inline bool NetAddrCopy(struct sockaddr_storage *dst, const struct sockaddr *src) {
  if (!dst || !src) {
    return false;
  }
  const size_t n = NetAddrLength(src);
  if (n == 0) {
    return false;
  }
  memset(dst, 0, sizeof(*dst));
  memcpy(dst, src, n);
  return true;
}

static inline uint16_t NetAddrPort(const struct sockaddr *sa) {
  if (!sa) {
    return 0;
  }
  if (sa->sa_family == AF_INET) {
    return ntohs(((const struct sockaddr_in *)(const void *)sa)->sin_port);
  }
  if (sa->sa_family == AF_INET6) {
    return ntohs(((const struct sockaddr_in6 *)(const void *)sa)->sin6_port);
  }
  return 0;
}

static inline bool NetAddrSetPort(struct sockaddr *sa, uint16_t port) {
  if (!sa) {
    return false;
  }
  if (sa->sa_family == AF_INET) {
    ((struct sockaddr_in *)(void *)sa)->sin_port = htons(port);
    return true;
  }
  if (sa->sa_family == AF_INET6) {
    ((struct sockaddr_in6 *)(void *)sa)->sin6_port = htons(port);
    return true;
  }
  return false;
}

static inline bool NetAddrEqualNoPort(const struct sockaddr *a, const struct sockaddr *b) {
  if (!a || !b || a->sa_family != b->sa_family) {
    return false;
  }
  if (a->sa_family == AF_INET) {
    return ((const struct sockaddr_in *)(const void *)a)->sin_addr.s_addr ==
           ((const struct sockaddr_in *)(const void *)b)->sin_addr.s_addr;
  }
  if (a->sa_family == AF_INET6) {
    return memcmp(&((const struct sockaddr_in6 *)(const void *)a)->sin6_addr,
                  &((const struct sockaddr_in6 *)(const void *)b)->sin6_addr, 16) == 0;
  }
  return false;
}

static inline bool NetAddrEqual(const struct sockaddr *a, const struct sockaddr *b) {
  return NetAddrEqualNoPort(a, b) && NetAddrPort(a) == NetAddrPort(b);
}

/* FNV-1a over family, port and address bytes. Network byte order keeps the
 * result stable across architectures. Not cryptographic: use it to bucket,
 * never to authenticate. Pair with NetAddrEqual(). */
static inline uint32_t NetAddrHash(const struct sockaddr *sa) {
  uint32_t h = 2166136261u;
  if (!sa) {
    return 0;
  }
  const uint8_t family = (uint8_t)sa->sa_family;
  h = (h ^ family) * 16777619u;
  if (sa->sa_family == AF_INET) {
    const struct sockaddr_in *s4 = (const struct sockaddr_in *)(const void *)sa;
    const uint8_t *p = (const uint8_t *)&s4->sin_port;
    for (size_t i = 0; i < 2; i++) {
      h = (h ^ p[i]) * 16777619u;
    }
    p = (const uint8_t *)&s4->sin_addr;
    for (size_t i = 0; i < 4; i++) {
      h = (h ^ p[i]) * 16777619u;
    }
  } else if (sa->sa_family == AF_INET6) {
    const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)(const void *)sa;
    const uint8_t *p = (const uint8_t *)&s6->sin6_port;
    for (size_t i = 0; i < 2; i++) {
      h = (h ^ p[i]) * 16777619u;
    }
    p = (const uint8_t *)&s6->sin6_addr;
    for (size_t i = 0; i < 16; i++) {
      h = (h ^ p[i]) * 16777619u;
    }
  } else {
    return 0;
  }
  return h;
}

static inline uint32_t NetAddrHashNoPort(const struct sockaddr *sa) {
  if (sa && sa->sa_family == AF_INET) {
    uint32_t h = 2166136261u;
    const uint8_t *p = (const uint8_t *)&((const struct sockaddr_in *)(const void *)sa)->sin_addr;
    h = (h ^ AF_INET) * 16777619u;
    for (size_t i = 0; i < 4; i++) {
      h = (h ^ p[i]) * 16777619u;
    }
    return h;
  }
  if (sa && sa->sa_family == AF_INET6) {
    uint32_t h = 2166136261u;
    const uint8_t *p = (const uint8_t *)&((const struct sockaddr_in6 *)(const void *)sa)->sin6_addr;
    h = (h ^ AF_INET6) * 16777619u;
    for (size_t i = 0; i < 16; i++) {
      h = (h ^ p[i]) * 16777619u;
    }
    return h;
  }
  return 0;
}

/* --------------------------------------------- IPv4-in-IPv6 canonicalisation */

/* If `sa` is an IPv6 address that embeds an IPv4 address, write the IPv4 form
 * (same port) into `out` and return true. Encodings handled:
 *   ::ffff:a.b.c.d   (::ffff:0:0/96, IPv4-mapped)
 *   2002:AABB:CCDD:: (6to4, IPv4 in bytes 2..5)
 *   64:ff9b::a.b.c.d (NAT64 well-known prefix)
 *   ::a.b.c.d        (IPv4-compatible; :: and ::1 are excluded)
 */
static inline bool NetAddrEmbeddedIPv4(const struct sockaddr *sa, struct sockaddr_storage *out) {
  if (!sa || !out || sa->sa_family != AF_INET6) {
    return false;
  }
  const uint8_t *u = (const uint8_t *)&((const struct sockaddr_in6 *)(const void *)sa)->sin6_addr;
  const uint8_t *v4 = NULL;

  if (u[0] == 0 && u[1] == 0 && u[2] == 0 && u[3] == 0 && u[4] == 0 && u[5] == 0 && u[6] == 0 && u[7] == 0 &&
      u[8] == 0 && u[9] == 0 && u[10] == 0xff && u[11] == 0xff) {
    v4 = u + 12; /* ::ffff:0:0/96 */
  } else if (u[0] == 0x20 && u[1] == 0x02) {
    v4 = u + 2; /* 2002::/16 */
  } else if (u[0] == 0x00 && u[1] == 0x64 && u[2] == 0xff && u[3] == 0x9b && u[4] == 0 && u[5] == 0 && u[6] == 0 &&
             u[7] == 0 && u[8] == 0 && u[9] == 0 && u[10] == 0 && u[11] == 0) {
    v4 = u + 12; /* 64:ff9b::/96 */
  } else if (u[0] == 0 && u[1] == 0 && u[2] == 0 && u[3] == 0 && u[4] == 0 && u[5] == 0 && u[6] == 0 && u[7] == 0 &&
             u[8] == 0 && u[9] == 0 && u[10] == 0 && u[11] == 0) {
    /* ::/96, but :: (unspecified) and ::1 (loopback) keep their IPv6 meaning. */
    if (!(u[12] == 0 && u[13] == 0 && u[14] == 0 && (u[15] == 0 || u[15] == 1))) {
      v4 = u + 12;
    }
  }

  if (!v4) {
    return false;
  }
  memset(out, 0, sizeof(*out));
  struct sockaddr_in *s4 = (struct sockaddr_in *)(void *)out;
  s4->sin_family = AF_INET;
  memcpy(&s4->sin_addr, v4, 4);
  s4->sin_port = ((const struct sockaddr_in6 *)(const void *)sa)->sin6_port;
  return true;
}

/* Internal helper: point at the IPv4 form when one is embedded. */
static inline const struct sockaddr *NetAddrCanonicalize(const struct sockaddr *sa, struct sockaddr_storage *tmp) {
  if (sa && tmp && NetAddrEmbeddedIPv4(sa, tmp)) {
    return (const struct sockaddr *)tmp;
  }
  return sa;
}

/* ------------------------------------------------------- classification */

static inline bool NetAddrIsUnspecified(const struct sockaddr *sa) {
  if (!sa) {
    return false;
  }
  if (sa->sa_family == AF_INET) {
    return ((const struct sockaddr_in *)(const void *)sa)->sin_addr.s_addr == 0;
  }
  if (sa->sa_family == AF_INET6) {
    const uint8_t *u = (const uint8_t *)&((const struct sockaddr_in6 *)(const void *)sa)->sin6_addr;
    for (size_t i = 0; i < 16; i++) {
      if (u[i]) {
        return false;
      }
    }
    return true;
  }
  return false;
}

static inline bool NetAddrIsLoopback(const struct sockaddr *sa) {
  struct sockaddr_storage tmp;
  const struct sockaddr *a = NetAddrCanonicalize(sa, &tmp);
  if (!a) {
    return false;
  }
  if (a->sa_family == AF_INET) {
    return ((const uint8_t *)&((const struct sockaddr_in *)(const void *)a)->sin_addr)[0] == 127;
  }
  if (a->sa_family == AF_INET6) {
    const uint8_t *u = (const uint8_t *)&((const struct sockaddr_in6 *)(const void *)a)->sin6_addr;
    for (size_t i = 0; i < 15; i++) {
      if (u[i]) {
        return false;
      }
    }
    return u[15] == 1;
  }
  return false;
}

static inline bool NetAddrIsMulticast(const struct sockaddr *sa) {
  struct sockaddr_storage tmp;
  const struct sockaddr *a = NetAddrCanonicalize(sa, &tmp);
  if (!a) {
    return false;
  }
  if (a->sa_family == AF_INET) {
    return ((const uint8_t *)&((const struct sockaddr_in *)(const void *)a)->sin_addr)[0] >= 224;
  }
  if (a->sa_family == AF_INET6) {
    return ((const uint8_t *)&((const struct sockaddr_in6 *)(const void *)a)->sin6_addr)[0] == 0xff;
  }
  return false;
}

/* Scopes that should never be used as a relay peer: IPv4 link-local
 * (169.254.0.0/16, incl. the cloud metadata service), IPv6 link-local
 * (fe80::/10), ULA (fc00::/7) and site-local (fec0::/10). Loopback is
 * deliberately excluded - keep that a separate policy decision. */
static inline bool NetAddrIsInternalDeny(const struct sockaddr *sa) {
  struct sockaddr_storage tmp;
  const struct sockaddr *a = NetAddrCanonicalize(sa, &tmp);
  if (!a) {
    return false;
  }
  if (a->sa_family == AF_INET) {
    const uint8_t *u = (const uint8_t *)&((const struct sockaddr_in *)(const void *)a)->sin_addr;
    return u[0] == 169 && u[1] == 254;
  }
  if (a->sa_family == AF_INET6) {
    const uint8_t *u = (const uint8_t *)&((const struct sockaddr_in6 *)(const void *)a)->sin6_addr;
    if (u[0] == 0xfe && (u[1] & 0xc0) == 0x80) {
      return true; /* fe80::/10 */
    }
    if ((u[0] & 0xfe) == 0xfc) {
      return true; /* fc00::/7 */
    }
    if (u[0] == 0xfe && (u[1] & 0xc0) == 0xc0) {
      return true; /* fec0::/10 */
    }
  }
  return false;
}

/* ------------------------------------------------------------- ranges */

/* A contiguous [min, max] span. `min`/`max` may be unspecified (family 0 or
 * all-zero address) to mean "unbounded on that side". Build min/max from your
 * prefix yourself: this is a lexicographic span, not a CIDR parser. */
struct NetAddrRange {
  struct sockaddr_storage min;
  struct sockaddr_storage max;
};

static inline bool NetAddrLessEqual(const struct sockaddr *a, const struct sockaddr *b) {
  if (!a) {
    return true;
  }
  if (!b) {
    return false;
  }
  if (a->sa_family != b->sa_family) {
    return a->sa_family < b->sa_family;
  }
  if (a->sa_family == AF_INET) {
    return ntohl(((const struct sockaddr_in *)(const void *)a)->sin_addr.s_addr) <=
           ntohl(((const struct sockaddr_in *)(const void *)b)->sin_addr.s_addr);
  }
  if (a->sa_family == AF_INET6) {
    /* in6_addr is MSB-first, so memcmp is the lexicographic total order. */
    return memcmp(&((const struct sockaddr_in6 *)(const void *)a)->sin6_addr,
                  &((const struct sockaddr_in6 *)(const void *)b)->sin6_addr, 16) <= 0;
  }
  return true;
}

static inline void NetAddrRangeSet(struct NetAddrRange *range, const struct sockaddr *min, const struct sockaddr *max) {
  if (!range) {
    return;
  }
  memset(range, 0, sizeof(*range));
  if (min) {
    (void)NetAddrCopy(&range->min, min);
  }
  if (max) {
    (void)NetAddrCopy(&range->max, max);
  }
}

static inline bool NetAddrInRange(const struct NetAddrRange *range, const struct sockaddr *sa) {
  if (!range || !sa) {
    return false;
  }
  const struct sockaddr *a = sa;
  struct sockaddr_storage tmp;
  const sa_family_t family = range->min.ss_family ? range->min.ss_family : range->max.ss_family;
  if (family == AF_INET && a->sa_family == AF_INET6) {
    a = NetAddrCanonicalize(a, &tmp);
  }
  const bool min_open = range->min.ss_family == 0 || NetAddrIsUnspecified((const struct sockaddr *)&range->min);
  if (!min_open && !NetAddrLessEqual((const struct sockaddr *)&range->min, a)) {
    return false;
  }
  const bool max_open = range->max.ss_family == 0 || NetAddrIsUnspecified((const struct sockaddr *)&range->max);
  if (max_open) {
    return true;
  }
  return NetAddrLessEqual(a, (const struct sockaddr *)&range->max);
}

/* ------------------------------------------------------------- text I/O */

/* Format as "1.2.3.4:port" / "[2001:db8::1]:port", or bare address when the
 * port is 0. Returns false on unknown family or truncation. */
static inline bool NetAddrFormat(const struct sockaddr *sa, char *buf, size_t buflen) {
  if (!sa || !buf || buflen == 0) {
    return false;
  }
  char text[INET6_ADDRSTRLEN];
  const uint16_t port = NetAddrPort(sa);
  int n = -1;

  if (sa->sa_family == AF_INET) {
    if (!inet_ntop(AF_INET, &((const struct sockaddr_in *)(const void *)sa)->sin_addr, text, sizeof(text))) {
      return false;
    }
    n = port ? snprintf(buf, buflen, "%s:%u", text, (unsigned)port) : snprintf(buf, buflen, "%s", text);
  } else if (sa->sa_family == AF_INET6) {
    if (!inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)(const void *)sa)->sin6_addr, text, sizeof(text))) {
      return false;
    }
    n = port ? snprintf(buf, buflen, "[%s]:%u", text, (unsigned)port) : snprintf(buf, buflen, "%s", text);
  } else {
    return false;
  }
  return n > 0 && (size_t)n < buflen;
}

static inline bool NetAddrParsePort(const char *s, uint16_t *out) {
  if (!s || !*s) {
    return false;
  }
  uint32_t value = 0;
  for (const char *p = s; *p; p++) {
    if (*p < '0' || *p > '9') {
      return false;
    }
    value = value * 10u + (uint32_t)(*p - '0');
    if (value > 65535u) {
      return false;
    }
  }
  *out = (uint16_t)value;
  return true;
}

/* Parse a bare numeric IPv4 or IPv6 address and attach a host-order port. */
static inline bool NetAddrParseHost(const char *host, uint16_t port, struct sockaddr_storage *out) {
  if (!host || !out) {
    return false;
  }
  struct sockaddr_storage addr;
  memset(&addr, 0, sizeof(addr));
  if (strchr(host, ':')) {
    struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)(void *)&addr;
    if (inet_pton(AF_INET6, host, &s6->sin6_addr) != 1) {
      return false;
    }
    s6->sin6_family = AF_INET6;
    s6->sin6_port = htons(port);
  } else {
    struct sockaddr_in *s4 = (struct sockaddr_in *)(void *)&addr;
    if (inet_pton(AF_INET, host, &s4->sin_addr) != 1) {
      return false;
    }
    s4->sin_family = AF_INET;
    s4->sin_port = htons(port);
  }
  *out = addr;
  return true;
}

/* Parse a numeric address with an optional port:
 *   "1.2.3.4"        "1.2.3.4:3478"
 *   "2001:db8::1"    "[2001:db8::1]:3478"
 * Strictly numeric (inet_pton): no DNS, no scope ids ("%eth0" is rejected).
 * `default_port` applies when the text carries no port. */
static inline bool NetAddrParse(const char *text, uint16_t default_port, struct sockaddr_storage *out) {
  if (!text || !out) {
    return false;
  }
  char host[INET6_ADDRSTRLEN + 1];
  uint16_t port = default_port;
  const char *p = text;

  if (*p == '[') {
    const char *close = strchr(p + 1, ']');
    if (!close) {
      return false;
    }
    const size_t hlen = (size_t)(close - (p + 1));
    if (hlen == 0 || hlen > INET6_ADDRSTRLEN) {
      return false;
    }
    memcpy(host, p + 1, hlen);
    host[hlen] = '\0';
    if (close[1] == ':') {
      if (!NetAddrParsePort(close + 2, &port)) {
        return false;
      }
    } else if (close[1] != '\0') {
      return false;
    }
  } else {
    const char *colon = strchr(p, ':');
    if (colon && strchr(colon + 1, ':') == NULL) {
      /* Exactly one colon: address:port. */
      const size_t hlen = (size_t)(colon - p);
      if (hlen == 0 || hlen > INET6_ADDRSTRLEN) {
        return false;
      }
      memcpy(host, p, hlen);
      host[hlen] = '\0';
      if (!NetAddrParsePort(colon + 1, &port)) {
        return false;
      }
    } else {
      const size_t hlen = strlen(p);
      if (hlen == 0 || hlen > INET6_ADDRSTRLEN) {
        return false;
      }
      memcpy(host, p, hlen + 1);
    }
  }

  return NetAddrParseHost(host, port, out);
}

#ifdef __cplusplus
}
#endif

#endif /* NET_ADDR_H */
