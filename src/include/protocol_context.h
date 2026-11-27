/**
 * @file protocol_context.h
 * @brief Per-instance protocol state for ufsrvstun.
 *
 * @copyright Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of ufsrvstun source code.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFSRVSTUN_PROTOCOL_CONTEXT_H
#define UFSRVSTUN_PROTOCOL_CONTEXT_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/socket.h>

#include <ufsrvcorelib/ufsrv_core/delegator_sessions/ufsrv_listener_descriptor_type.h>
#include <ufsrvcorelib/ufsrv_core/protocol/protocol_type.h>
#include <ufsrvcorelib/ufsrv_core/udp_server/udp_server.h>

typedef struct TurnAuthContext   TurnAuthContext;
typedef struct TurnRelayRuntime  TurnRelayRuntime;
typedef struct RecyclerPoolHandle RecyclerPoolHandle;

enum { STUN_LIMIT_BUCKETS = 1024, STUN_LIMITS_PER_BUCKET = 4 };

typedef struct ServerProtocolBase  ServerProtocolBase;
typedef struct StunProtocolContext  StunProtocolContext;
typedef struct TurnProtocolContext  TurnProtocolContext;

/** @brief One source's admission budget: refilled by the configured rate. */
typedef struct StunSourceLimit
{
  struct sockaddr_storage address;
  bool                    occupied;
  double                  updated_at;
  double                  tokens;
} StunSourceLimit;

/** @brief What every protocol instance owns, whichever domain it serves. */
struct ServerProtocolBase
{
  const Protocol *               protocol;    ///< Registry slot this instance serves
  const UfsrvListenerDescriptor *listener;    ///< Declared endpoint, owned by the config store
  UdpServer *                    server;      ///< The one I/O engine this instance runs
  UfsrvUserData *                worker_slots; ///< Worker slot storage for that engine
  size_t                         worker_count;
  bool                           dtls;        ///< Transport this instance was declared with
};

/** @brief Plain STUN: binding responses and the admission budget behind them. */
struct StunProtocolContext
{
  struct ServerProtocolBase base;
  StunSourceLimit           limits[STUN_LIMIT_BUCKETS][STUN_LIMITS_PER_BUCKET];
  pthread_mutex_t           limits_lock;
};

/** @brief TURN: authentication, session capacity and the relay plane. */
struct TurnProtocolContext
{
  struct ServerProtocolBase base;
  TurnAuthContext *         auth;         ///< Owned; loaded by the instance
  RecyclerPoolHandle *      session_pool; ///< This instance's session capacity
  pthread_mutex_t           session_lock;
  TurnRelayRuntime *        relay;        ///< Allocations, relay loop, port range and policy
};

#endif /* UFSRVSTUN_PROTOCOL_CONTEXT_H */
