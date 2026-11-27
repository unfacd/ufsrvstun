/**
 * @file main.c
 * @brief Process entry point for the STUN server.
 */

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include <openssl/err.h>
#include <uflib/buffer_descriptor/buffer_descriptor.h>
#include <uflib/version.h>
#include <ufsrv_core/command_console/command_console_thread.h>
#include <ufsrvcorelib/ufsrv_console_script/ufsrv_console_script.h>
#include <ufsrvcorelib/ufsrv_core/main_utils.h>
#include <ufsrvcorelib/ufsrv_core/system/ufsrv_signals.h>
#include <ufsrvcorelib/version.h>

#include <ufsrvcorelib/ufsrv_core/ufsrv_utils.h>

#include "version.h"
#include "main_protocols.h"

#include "protocol_stun.h"
#include "ufsrv_console_handlers.h"
#include "net_addr.h"
#include "stun_diagnostics.h"
#include "ufsrvstun_config.h"
#include "turn/auth/turn_auth_runtime.h"

static bool
ParseStunOptions(const UfsrvUnknownArgs *args, unsigned *port)
{
  for (size_t i = 0; i < args->count; i++) {
    const char *flag = args->args[i].flag;
    const char *value = NULL;
    if (strncmp(flag, "--port=", 7) == 0) value = flag + 7;
    else if (strcmp(flag, "--port") == 0 && i + 1 < args->count) value = args->args[++i].flag;
    if (!value || *port || value[strspn(value, "0123456789")] != '\0') {
      fprintf(stderr, "Invalid STUN option: %s\n", flag);
      return false;
    }
    unsigned long parsed = strtoul(value, NULL, 10);
    if (parsed == 0 || parsed > 65535) {
      STUN_CONFIG_ERR("%s", "Port must be between 1 and 65535");
      return false;
    }
    *port = (unsigned)parsed;
  }
  return true;
}

static int
RunStunServer(void)
{
  /* Standard signal model: SIGPIPE ignored, HUP/TERM/INT blocked before any
   * worker thread is created and consumed by sigwait() below. */
  InitSignals();
  int status = EXIT_SUCCESS;

  size_t workers = UfsrvConfigStunShardCount() * UfsrvConfigStunWorkersPerShard();
  const bool dtls_enabled = UfsrvConfigStunDtlsIsEnabled();

  StunTurnAuthRuntime turn_auth;
  if (!StunTurnAuthLoad(&turn_auth)) return EXIT_FAILURE;

  if (!StunProtocolInit(turn_auth.auth)) {
    syslog(LOG_ERR, "Could not initialize TURN session and relay state");
    StunTurnAuthDestroy(&turn_auth);
    return EXIT_FAILURE;
  }

  unsigned char *worker_slots = calloc(workers, sizeof(*worker_slots));
  unsigned char *dtls_worker_slots = NULL;
  UdpServer *server = worker_slots ? StunProtocolStart(false, worker_slots, workers) : NULL;
  UdpServer *dtls_server = NULL;
  UfsrvCommandConsole *console = NULL;
  if (!server) {
    syslog(LOG_ERR, "Could not start the declared UDP listener");
    status = EXIT_FAILURE;
    goto cleanup;
  }
  if (dtls_enabled) {
    dtls_worker_slots = calloc(workers, sizeof(*dtls_worker_slots));
    ERR_clear_error();
    if (dtls_worker_slots) dtls_server = StunProtocolStart(true, dtls_worker_slots, workers);
    if (!dtls_server) {
      syslog(LOG_ERR, "Could not start the declared DTLS listener");
      ERR_print_errors_fp(stderr);
      status = EXIT_FAILURE;
      goto cleanup;
    }
  }
  if (UfsrvConfigSslCommandConsoleIsEnabled()) {
    StunConsoleRegisterHandlers();
    console = UfsrvCommandConsoleStart(UfsrvConsoleScriptResolve);
    if (!console) {
      syslog(LOG_ERR, "Could not start command console");
      status = EXIT_FAILURE;
      goto cleanup;
    }
  }

  int received_signal;
  int wait_status = sigwait(UfsrvSignalControlSet(), &received_signal);
  if (wait_status != 0) {
    syslog(LOG_ERR, "Could not wait for shutdown signals: %s", strerror(wait_status));
    status = EXIT_FAILURE;
  }
cleanup:
  UfsrvCommandConsoleStop(console);
  UfsrvConsoleScriptDestroy();
  StunProtocolStop();
  UdpServerStop(dtls_server);
  UdpServerStop(server);
  StunTurnAuthDestroy(&turn_auth);
  free(dtls_worker_slots);
  free(worker_slots);
  return status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

#include "data_protocols.h"

const Protocol *
UfsrvStunProtocol(void)
{
  return &ProtocolsRegistry[STUN_PROTOCOL_ID];
}

const Protocol *
UfsrvDtlsProtocol(void)
{
  return &ProtocolsRegistry[DTLS_PROTOCOL_ID];
}

size_t
UfsrvProtocolRegistrySize(void)
{
  return HOST_PROTOCOL_SLOTS;
}

int
main(int argc, char *argv[])
{
  openlog("ufsrvstun", LOG_NDELAY, LOG_DAEMON);

  StunConfigBootstrap();
  UfsrvConfigSetWhen(&(UfsrvConfigItem){.value.stored_value.int_val = time(NULL)},
                     UFSRV_CONFIG_SOURCE_RUNTIME);

  CommandLineArgsConfig cli = {0};
  cli.instance_id = -1; // Preserve the file setting when the CLI omits the ID.
  UfsrvConfigParseArgs(argc, argv, &cli);

  int result = EXIT_FAILURE;
  unsigned port = 0;
  if (cli.help_requested) {
    UfsrvConfigPrintUsage(stdout, argv[0], "  --port PORT  Override the UDP listener port\n");
    result = EXIT_SUCCESS;
  } else if (cli.version_requested) {
    printf("ufsrvstun %s.%s (ufsrvcorelib %s)\nbuilt %s by %s\n",
           UfsrvVersion(), UfsrvVersionInternal(), UfsrvCoreLibVersion(),
           UfsrvCompiledTimestamp(), UfsrvCompiledBy());
    result = EXIT_SUCCESS;
  } else if (cli.result.state == UFSRV_ARGS_ERROR) {
    fprintf(stderr, "%s\n", cli.result.message);
  } else if (cli.sandbox_protocol || cli.cpu_affinity_verify_requested ||
             cli.server_process_mode == SERVER_PROCESS_MODE_DAEMON ||
             (cli.config_op_requested && cli.config_file_op == CLI_CONFIGFILE_OP_GENERATE)) {
    STUN_CONFIG_ERR("%s", "Requested option is unavailable for ufsrvstun");
  } else if (ParseStunOptions(&cli.unknown_args, &port)) {
    ConfigBootstrapCliDefaults(&cli);

    if (!StunConfigProcessFile(!cli.config_op_requested ||
                               cli.config_file_op == CLI_CONFIGFILE_OP_VERIFY_STRICT)) {
      /* the pipeline reported its own diagnostics */
    } else if (cli.config_op_requested) {
      puts("Configuration valid");
      result = EXIT_SUCCESS;
    } else {
      ConfigInjectOverridingCliArguments(&cli);
      if (cli.dump_config_json_requested) {
        result = StunConfigDumpJson(cli.dump_config_json_file) ? EXIT_SUCCESS : EXIT_FAILURE;
      } else if (!UfsrvConfigStunIsEnabled()) {
        syslog(LOG_NOTICE, "STUN is disabled by configuration");
        result = EXIT_SUCCESS;
      } else {
        syslog(LOG_INFO, "Starting ufsrvstun %s.%s (ufsrvcorelib %s), built %s by %s",
               UfsrvVersion(), UfsrvVersionInternal(), UfsrvCoreLibVersion(),
               UfsrvCompiledTimestamp(), UfsrvCompiledBy());
        CollectionDescriptor listeners = UfsrvConfigListeners();
        if (!IsUfsrvListenersDescriptorValid(&listeners, ProtocolsRegistry)) {
          syslog(LOG_ERR, "Invalid listener descriptor");
          result = EXIT_FAILURE;
        } else {
          if (port) {
            for (size_t i = 0; i < listeners.collection_sz; i++) {
              UfsrvListenerDescriptor *listener = (UfsrvListenerDescriptor *)listeners.collection[i];
              if (listener->protocol == UfsrvStunProtocol()) listener->port = (uint16_t)port;
            }
          }
          
          ProtocolsRegistry[STUN_PROTOCOL_ID].protocol_controls.is_enabled  = UfsrvConfigStunIsEnabled() ? 1 : 0;
          ProtocolsRegistry[DTLS_PROTOCOL_ID].protocol_controls.is_enabled = UfsrvConfigStunDtlsIsEnabled() ? 1 : 0;

          InitProtocolsForUfsrv(ProtocolsRegistry, HOST_PROTOCOL_SLOTS);
          result = RunStunServer();
        }
      }
    }
  }

  UfsrvConfigArgsDestruct(&cli);
  StunConfigDestroy();
  closelog();
  return result;
}
