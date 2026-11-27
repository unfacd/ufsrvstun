/**
 * @file turn_policy_type.h
 * @brief Types for TURN dispatch and attribute validation policies.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef UFSRVSTUN_TURN_POLICY_TYPE_H
#define UFSRVSTUN_TURN_POLICY_TYPE_H

#include <stddef.h>
#include <stdint.h>

/*! @brief Decoded message classes accepted by the incoming policy table. */
typedef enum TurnMessageClass
{
  TURN_MESSAGE_REQUEST,   ///< Request that may produce a response.
  TURN_MESSAGE_INDICATION ///< Indication processed without a response.
} TurnMessageClass;

/*! @brief Authentication or allocation check required before dispatch. */
typedef enum TurnAuthMode
{
  TURN_AUTH_PUBLIC,        ///< Method can run without a TURN credential.
  TURN_AUTH_LONG_TERM,     ///< Verify the request using long-term credentials.
  TURN_AUTH_ALLOCATION     ///< Require an existing allocation for the client flow.
} TurnAuthMode;

/*! @brief Length and occurrence constraints for one understood wire attribute. */
typedef struct TurnAttributeRule
{
  uint16_t type;            ///< Wire attribute ID from the RFC codec.
  uint16_t min_length;      ///< Minimum attribute value length in bytes, excluding padding.
  uint16_t max_length;      ///< Maximum attribute value length in bytes, excluding padding.
  uint16_t min_occurrences; ///< Minimum instances per message, zero for optional attributes.
  uint16_t max_occurrences; ///< Maximum instances per message, zero for no explicit limit.
} TurnAttributeRule;

/*! @brief Attribute and authentication policy for one incoming method and class. */
typedef struct TurnMessagePolicy
{
  uint16_t method;                 ///< Decoded STUN or TURN method ID.
  TurnMessageClass message_class;  ///< Decoded message class for this policy.
  TurnAuthMode auth_mode;          ///< Authentication or allocation requirement.
  const TurnAttributeRule *rules; ///< Static array of method-specific attribute rules.
  size_t rule_count;               ///< Number of entries in rules.
} TurnMessagePolicy;

#endif /* UFSRVSTUN_TURN_POLICY_TYPE_H */
