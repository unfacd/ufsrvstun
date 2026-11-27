/**
 * @file stun_diagnostics.h
 * @brief Startup/config diagnostics: one message, both destinations.
 *
 * A configuration or CLI error must reach an interactive operator on stderr and
 * a supervised deployment through syslog. Emitting both here keeps the two in
 * step and stamps every record with the failing function.
 */

#ifndef UFSRVSTUN_STUN_DIAGNOSTICS_H
#define UFSRVSTUN_STUN_DIAGNOSTICS_H

#include <stdio.h>
#include <syslog.h>

#define STUN_CONFIG_ERR(fmt, ...)                                    \
  do {                                                               \
    fprintf(stderr, fmt "\n", ##__VA_ARGS__);                        \
    syslog(LOG_ERR, "%s: " fmt, __func__, ##__VA_ARGS__);            \
  } while (0)

#endif /* UFSRVSTUN_STUN_DIAGNOSTICS_H */
