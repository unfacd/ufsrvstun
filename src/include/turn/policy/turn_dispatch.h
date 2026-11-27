/**
 * @file turn_dispatch.h
 * @brief Classify and validate datagrams before host protocol dispatch.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef UFSRVSTUN_TURN_DISPATCH_H
#define UFSRVSTUN_TURN_DISPATCH_H

#include <stdbool.h>
#include "turn_policy_type.h"

/*! @brief Inbound routes supported by the dispatcher. */
typedef enum TurnDatagramKind
{
  TURN_DATAGRAM_BINDING,           ///< STUN Binding request.
  TURN_DATAGRAM_ALLOCATE,          ///< TURN Allocate request.
  TURN_DATAGRAM_REFRESH,           ///< TURN Refresh request.
  TURN_DATAGRAM_CREATE_PERMISSION, ///< TURN CreatePermission request.
  TURN_DATAGRAM_CHANNEL_BIND,      ///< TURN ChannelBind request.
  TURN_DATAGRAM_SEND,              ///< TURN Send indication.
  TURN_DATAGRAM_CHANNEL_DATA       ///< TURN ChannelData frame.
} TurnDatagramKind;

/*! @brief Outcome of validating one datagram. */
typedef enum TurnDecodeStatus
{
  TURN_DECODE_OK = 0,              ///< Structurally valid; frame describes it.
  TURN_DECODE_IGNORE,              ///< Not ours or unroutable; stay silent.
  TURN_DECODE_BAD_REQUEST,         ///< A request violates its attribute rules: answer 400.
  TURN_DECODE_UNKNOWN_ATTRIBUTES   ///< A request carries unknown comprehension-required attributes: answer 420.
} TurnDecodeStatus;

/*! @brief Metadata from a structurally valid datagram. */
typedef struct TurnDatagram
{
  TurnDatagramKind kind;           ///< Route selected by the wire method and class.
  const TurnMessagePolicy *policy; ///< Static policy, or NULL for ChannelData.
  size_t attributes_end;           ///< Offset of first integrity attribute, or end of ordinary attributes.
  bool fingerprint;                ///< Whether a validated FINGERPRINT was present.
  uint16_t channel;                ///< Channel number for ChannelData.
  size_t channel_payload_length;   ///< ChannelData payload length excluding header and padding.
  uint16_t unknown_required[8];    ///< Unknown comprehension-required attribute types.
  size_t unknown_required_count;   ///< Entries filled in unknown_required.
} TurnDatagram;

/**
 * Validate one full UDP or DTLS plaintext datagram and classify it.
 * The result describes wire structure. Authentication occurs later.
 *
 * @code{.c}
 * TurnDatagram frame;
 * TurnDecodeStatus status = TurnDecodeDatagram(packet, packet_len, turn_enabled, &frame);
 * if (status == TURN_DECODE_OK) {
 *   // Select the handler with frame.kind.
 * }
 * @endcode
 */
TurnDecodeStatus TurnDecodeDatagram(const uint8_t *packet, size_t packet_len, bool turn_enabled,
                                    TurnDatagram *frame);

#endif /* UFSRVSTUN_TURN_DISPATCH_H */
