/**
 * @file turn_dispatch.c
 * @brief Dispatch policy for STUN Binding and inbound TURN frames.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <syslog.h>

#include "turn/policy/turn_dispatch.h"
#include <ufsrvcorelib/ufsrv_core/turn/codec/ns_turn_msg.h>

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

enum { MAX_METHOD_RULES = 8 };

static const TurnAttributeRule allocate_rules[] = {
  {STUN_ATTRIBUTE_REQUESTED_TRANSPORT, 4, 4, 1, 1}, {STUN_ATTRIBUTE_LIFETIME, 4, 4, 0, 1},
  {STUN_ATTRIBUTE_REQUESTED_ADDRESS_FAMILY, 4, 4, 0, 1}, {STUN_ATTRIBUTE_ADDITIONAL_ADDRESS_FAMILY, 4, 4, 0, 1},
  {STUN_ATTRIBUTE_EVEN_PORT, 1, 1, 0, 1}, {STUN_ATTRIBUTE_RESERVATION_TOKEN, 8, 8, 0, 1},
  {STUN_ATTRIBUTE_DONT_FRAGMENT, 0, 0, 0, 1},
};
static const TurnAttributeRule refresh_rules[] = {
  {STUN_ATTRIBUTE_LIFETIME, 4, 4, 0, 1}, {STUN_ATTRIBUTE_REQUESTED_ADDRESS_FAMILY, 4, 4, 0, 1},
};
static const TurnAttributeRule permission_rules[]   = {{STUN_ATTRIBUTE_XOR_PEER_ADDRESS, 8, 20, 1, 0},};
static const TurnAttributeRule channel_bind_rules[] = {
  {STUN_ATTRIBUTE_XOR_PEER_ADDRESS, 8, 20, 1, 1}, {STUN_ATTRIBUTE_CHANNEL_NUMBER, 4, 4, 1, 1},
};
static const TurnAttributeRule send_rules[] = {
  {STUN_ATTRIBUTE_XOR_PEER_ADDRESS, 8, 20, 1, 1}, {STUN_ATTRIBUTE_DATA, 0, UINT16_MAX, 1, 1},
  {STUN_ATTRIBUTE_DONT_FRAGMENT, 0, 0, 0, 1},
};
static const TurnAttributeRule credential_rules[] = {
  {STUN_ATTRIBUTE_USERNAME, 1, STUN_MAX_USERNAME_SIZE, 0, 1}, {STUN_ATTRIBUTE_REALM, 1, STUN_MAX_REALM_SIZE, 0, 1},
  {STUN_ATTRIBUTE_NONCE, 1, STUN_MAX_NONCE_SIZE, 0, 1}, {STUN_ATTRIBUTE_USERHASH, 32, 32, 0, 1},
  {STUN_ATTRIBUTE_PASSWORD_ALGORITHM, 4, UINT16_MAX, 0, 1}, {STUN_ATTRIBUTE_PASSWORD_ALGORITHMS, 4, UINT16_MAX, 0, 1},
};

/*! @brief Wire policy and route for a supported method and class. */
typedef struct TurnRoute
{
  TurnDatagramKind  kind;   ///< Internal route identifier.
  TurnMessagePolicy policy; ///< Structural and authentication requirements.
} TurnRoute;

static const TurnRoute routes[] = {
  {TURN_DATAGRAM_BINDING, {STUN_METHOD_BINDING, TURN_MESSAGE_REQUEST, TURN_AUTH_PUBLIC, NULL, 0}},
  {
    TURN_DATAGRAM_ALLOCATE,
    {STUN_METHOD_ALLOCATE, TURN_MESSAGE_REQUEST, TURN_AUTH_LONG_TERM, allocate_rules, ARRAY_COUNT(allocate_rules)}
  },
  {
    TURN_DATAGRAM_REFRESH,
    {STUN_METHOD_REFRESH, TURN_MESSAGE_REQUEST, TURN_AUTH_LONG_TERM, refresh_rules, ARRAY_COUNT(refresh_rules)}
  },
  {
    TURN_DATAGRAM_CREATE_PERMISSION,
    {
      STUN_METHOD_CREATE_PERMISSION, TURN_MESSAGE_REQUEST, TURN_AUTH_LONG_TERM, permission_rules,
      ARRAY_COUNT(permission_rules)
    }
  },
  {
    TURN_DATAGRAM_CHANNEL_BIND,
    {
      STUN_METHOD_CHANNEL_BIND, TURN_MESSAGE_REQUEST, TURN_AUTH_LONG_TERM, channel_bind_rules,
      ARRAY_COUNT(channel_bind_rules)
    }
  },
  {
    TURN_DATAGRAM_SEND,
    {STUN_METHOD_SEND, TURN_MESSAGE_INDICATION, TURN_AUTH_ALLOCATION, send_rules, ARRAY_COUNT(send_rules)}
  },
};

static const TurnRoute *
FindRoute(uint16_t method, TurnMessageClass message_class, bool turn_enabled)
{
  for (size_t i = 0; i < ARRAY_COUNT(routes); i++) {
    const TurnRoute *route = &routes[i];
    if (route->policy.method == method && route->policy.message_class == message_class && (turn_enabled || route->kind
      == TURN_DATAGRAM_BINDING)) return route;
  }
  return NULL;
}

static size_t
FindRule(const TurnAttributeRule *rules, size_t count, uint16_t type)
{
  for (size_t i = 0; i < count; i++) {
    if (rules[i].type == type) return i;
  }
  return count;
}

static bool
CheckRule(const TurnAttributeRule *rule, const uint8_t *value, size_t length, size_t *occurrences)
{
  if (length < rule->min_length || length > rule->max_length || (rule->max_occurrences && *occurrences >= rule->
    max_occurrences)) return false;

  if (rule->type == STUN_ATTRIBUTE_XOR_PEER_ADDRESS && !((value[1] == 1 && length == 8) || (value[1] == 2 && length ==
    20))) return false;

  if (rule->type == STUN_ATTRIBUTE_CHANNEL_NUMBER && !STUN_VALID_CHANNEL_BIND(turn_read_u16(value))) return false;

  (*occurrences)++;
  return true;
}

static TurnDecodeStatus
ValidateAttributes(const uint8_t *packet, size_t length, TurnDatagram *frame)
{
  const TurnMessagePolicy *policy                                                = frame->policy;
  size_t                   method_occurrences[MAX_METHOD_RULES]                  = {0};
  size_t                   credential_occurrences[ARRAY_COUNT(credential_rules)] = {0};
  bool                     integrity_sha1                                        = false;
  bool                     integrity_sha256                                      = false;
  TurnDecodeStatus         status                                                = TURN_DECODE_OK;
  frame->attributes_end                                                          = length;
  if (policy->rule_count > ARRAY_COUNT(method_occurrences)) return TURN_DECODE_BAD_REQUEST;

  for (size_t offset = STUN_HEADER_LENGTH; offset < length;) {
    const size_t remaining = length - offset;
    if (remaining < 4) return TURN_DECODE_BAD_REQUEST;

    const uint16_t type          = turn_read_u16(packet + offset);
    const size_t   value_length  = turn_read_u16(packet + offset + 2);
    const size_t   padded_length = (value_length + 3) & ~(size_t)3;
    if (padded_length > remaining - 4) return TURN_DECODE_BAD_REQUEST;
    const uint8_t *value = packet + offset + 4;

    if (type == STUN_ATTRIBUTE_FINGERPRINT) {
      if (value_length != 4 || remaining != 8) return TURN_DECODE_BAD_REQUEST;
      if (frame->attributes_end == length) frame->attributes_end = offset;
    }
    else if (type == STUN_ATTRIBUTE_MESSAGE_INTEGRITY || type == STUN_ATTRIBUTE_MESSAGE_INTEGRITY_SHA256) {
      if (policy->auth_mode != TURN_AUTH_LONG_TERM) return TURN_DECODE_BAD_REQUEST;
      if (type == STUN_ATTRIBUTE_MESSAGE_INTEGRITY) {
        if (integrity_sha1 || value_length != 20) return TURN_DECODE_BAD_REQUEST;
        integrity_sha1 = true;
      }
      else {
        if (integrity_sha256 || value_length < 16 || value_length > 32 || value_length % 4)
          return TURN_DECODE_BAD_REQUEST;
        integrity_sha256 = true;
      }
      if (frame->attributes_end == length) frame->attributes_end = offset;
    }
    else if (!integrity_sha1 && !integrity_sha256) {
      const size_t method     = FindRule(policy->rules, policy->rule_count, type);
      const size_t credential = FindRule(credential_rules, ARRAY_COUNT(credential_rules), type);
      if (method < policy->rule_count) {
        if (!CheckRule(&policy->rules[method], value, value_length, &method_occurrences[method]))
          return TURN_DECODE_BAD_REQUEST;
      }
      else if (policy->auth_mode == TURN_AUTH_LONG_TERM && credential < ARRAY_COUNT(credential_rules)) {
        if (!CheckRule(&credential_rules[credential], value, value_length, &credential_occurrences[credential]))
          return TURN_DECODE_BAD_REQUEST;
      }
      else if (type < 0x8000) {
        /* Comprehension-required and not understood: RFC 8489 wants 420 with
         * the offending types listed, not silence. */
        if (frame->unknown_required_count < ARRAY_COUNT(frame->unknown_required)) {
          frame->unknown_required[frame->unknown_required_count++] = type;
        }
        status = TURN_DECODE_UNKNOWN_ATTRIBUTES;
      }
    }

    offset += 4 + padded_length;
  }

  if (status != TURN_DECODE_OK) return status;

  for (size_t i = 0; i < policy->rule_count; i++) {
    if (method_occurrences[i] < policy->rules[i].min_occurrences) return TURN_DECODE_BAD_REQUEST;
  }

  return stun_is_command_message_full_check_str(packet, length, false, &frame->fingerprint) ? TURN_DECODE_OK
                                                                                           : TURN_DECODE_BAD_REQUEST;
}

TurnDecodeStatus
TurnDecodeDatagram(const uint8_t *packet, size_t packet_len, bool turn_enabled, TurnDatagram *frame)
{
  if (!packet || !frame || packet_len < STUN_CHANNEL_HEADER_LENGTH) return TURN_DECODE_IGNORE;

  TurnDatagram decoded = {0};
  if ((packet[0] & 0xc0) == 0x40) {
    size_t consumed = packet_len;
    if (!turn_enabled || !stun_is_channel_message_str(packet, &consumed, &decoded.channel, false) || !
      STUN_VALID_CHANNEL_BIND(decoded.channel) || (packet_len != consumed && packet_len != ((consumed + 3) & ~(size_t)
        3))) return TURN_DECODE_IGNORE;
    decoded.kind                   = TURN_DATAGRAM_CHANNEL_DATA;
    decoded.channel_payload_length = consumed - STUN_CHANNEL_HEADER_LENGTH;
  }
  else {
    if (!stun_is_command_message_str(packet, packet_len)) return TURN_DECODE_IGNORE;

    TurnMessageClass message_class;
    if (stun_is_request_str(packet, packet_len)) message_class = TURN_MESSAGE_REQUEST;
    else if (stun_is_indication_str(packet, packet_len)) message_class = TURN_MESSAGE_INDICATION;
    else return TURN_DECODE_IGNORE;

    const uint16_t   method = stun_get_method_str(packet, packet_len);
    const TurnRoute *route  = FindRoute(method, message_class, turn_enabled);
    if (!route) {
      syslog(LOG_DEBUG, "%s: no route {method:'0x%03x', class:'0x%02x', turn_enabled:'%d'}", __func__, (unsigned)method,
             (unsigned)message_class, turn_enabled);
      return TURN_DECODE_IGNORE;
    }
    decoded.kind   = route->kind;
    decoded.policy = &route->policy;
    const TurnDecodeStatus status = ValidateAttributes(packet, packet_len, &decoded);
    if (status != TURN_DECODE_OK) {
      *frame = decoded;
      return status;
    }
  }

  *frame = decoded;
  return TURN_DECODE_OK;
}
