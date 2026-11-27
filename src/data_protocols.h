/**
 * @file data_protocols.h
 * @brief Protocol registry for ufsrvstun.
 *
 * Included inside main(), following the ufsrvapi convention: the static
 * registry lives for the lifetime of the process and is handed to corelib by
 * InitProtocolsForUfsrv(). Entries are initialised by field name, never by
 * position, so adding a callback or control bit cannot silently realign them.
 *
 * The registry is passed with UFSRVSTUN_PROTOCOL_COUNT; the trailing sentinel
 * is therefore only a terminator and is not counted.
 */

#ifndef UFSRVSTUN_DATA_PROTOCOLS_H
#define UFSRVSTUN_DATA_PROTOCOLS_H

#include <ufsrv_core/protocol/protocol.h>

#include "protocol_stun.h"

static Protocol ProtocolsRegistry[] = {
  [STUN_PROTOCOL_ID] = {
    .protocol_name = "Stun",
    .protocol_id = STUN_PROTOCOL_ID,
    .protocol_key = "stun",
    /* The instance runs its own UDP engine; corelib starts no thread for it. */
    .protocol_threadx = NULL,
    .protocol_callbacks = {0},
    .post_handshake = 0,
    .protocol_data = NULL,
    .protocol_controls = {
      .is_enabled = 0,
      .client_of_recycler = 0,
      .msgqueue_subscriber = 0,
      .msgqueue_publisher = 0,
    },
  },
  [DTLS_PROTOCOL_ID] = {
    .protocol_name = "StunDtls",
    .protocol_id = DTLS_PROTOCOL_ID,
    .protocol_key = "stun-dtls",
    .protocol_threadx = NULL,
    .protocol_callbacks = {0},
    .post_handshake = 0,
    .protocol_data = NULL,
    .protocol_controls = {
      .is_enabled = 0,
      .client_of_recycler = 0,
      .msgqueue_subscriber = 0,
      .msgqueue_publisher = 0,
    },
  },
  {
    .protocol_name = "",
    .protocol_id = (protocol_id_t)-1,
  }
};

#endif /* UFSRVSTUN_DATA_PROTOCOLS_H */
