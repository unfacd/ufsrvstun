/**
 * @file ufsrvstun_defs.h
 * @brief Preprocessor definitions shared across the server's sources.
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of ufsrvstun source code.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 */

#ifndef UFSRVSTUN_UFSRVSTUN_DEFS_H
#define UFSRVSTUN_UFSRVSTUN_DEFS_H

#include <pthread.h>
#include <syslog.h>

/**
 * Trace macros. Each names the emitting function and its thread, matching the
 * rest of the server's syslog output, and compiles out unless the build
 * selects its switch. STUN_LOG() carries no level and follows the base debug
 * switch, which either level implies.
 */
#if UFSRVSTUN_DEBUG_BUILD
# define STUN_LOG(fmt, ...) \
  syslog(LOG_DEBUG, "%s {pid:'%lu'}: " fmt, __func__, pthread_self(), ##__VA_ARGS__)
#else
# define STUN_LOG(fmt, ...) ((void)0)
#endif

#if UFSRVSTUN_DEBUG_BUILD_L2
# define STUN_LOG_L2(fmt, ...) \
  syslog(LOG_DEBUG, "%s {pid:'%lu'}: " fmt, __func__, pthread_self(), ##__VA_ARGS__)
#else
# define STUN_LOG_L2(fmt, ...) ((void)0)
#endif

#if UFSRVSTUN_DEBUG_BUILD_L3
# define STUN_LOG_L3(fmt, ...) \
  syslog(LOG_DEBUG, "%s {pid:'%lu'}: " fmt, __func__, pthread_self(), ##__VA_ARGS__)
#else
# define STUN_LOG_L3(fmt, ...) ((void)0)
#endif

#endif /* UFSRVSTUN_UFSRVSTUN_DEFS_H */
