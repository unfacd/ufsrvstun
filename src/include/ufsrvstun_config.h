#ifndef UFSRVSTUN_CONFIG_H
#define UFSRVSTUN_CONFIG_H

#include <stdbool.h>

#include <ufsrvcorelib/ufsrv_core/cli_configurator/ufsrv_cli_configurator.h>

#include "ufsrv_core/ufsrv_config/generated/ufsrv_config_accessor_decls_generated.h"

/** Bootstrap the config stores and install compiled defaults. */
void StunConfigBootstrap(void);

/** Validate the selected Lua file and transfer its values to the store. */
bool StunConfigProcessFile(bool strict);

/** Write the effective configuration as JSON. */
bool StunConfigDumpJson(const char *path);


/** Release the stores after shutting down workers. */
void StunConfigDestroy(void);

#endif
