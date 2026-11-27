/**
 * @file data_config_symbols_core_priv.h
 * @brief Core (ufsrv scope) symbol table for this host.
 *
 * Every ufsrvcorelib key this host links; the composed key map resolves each
 * literal to its field index.
 */

#ifndef UFSRVSTUN_DATA_CONFIG_SYMBOLS_CORE_PRIV_H
#define UFSRVSTUN_DATA_CONFIG_SYMBOLS_CORE_PRIV_H

#include <ufsrv_core/configfile_processor/configfile_processor_symbol_table_type.h>
#include <ufsrv_core/configfile_processor/ufsrv_config_literals_defs.h>
#include <ufsrv_core/ufsrv_config/generated/ufsrv_config_field_enums_generated.h>

static ConfFileSymbolTableEntry g_ufsrv_config_symbols[] = {
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_SERVER_INSTANCE_ID, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_SERVER_RUN_MODE, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_SERVER_CPU_AFFINITY, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_UFSRV_GEOGROUP, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_LISTENERS, .type = UFSRV_CONFIG_VALUE_COLLECTION },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_COMMAND_CONSOLE_PORT, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_COMMAND_CONSOLE_BIND_ADDRESS, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_SESSION_WORKERS_THREAD_POOL, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_UFSRV_WORKERS_THREAD_POOL, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_LISTENER_WORKERS_THREAD_POOL, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_SSL_COMMAND_CONSOLE_IS_ENABLED, .type = UFSRV_CONFIG_VALUE_BOOL },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_SSL_COMMAND_CONSOLE_CERTIFICATE_FILE, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_SSL_COMMAND_CONSOLE_KEY_FILE, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_SSL_COMMAND_CONSOLE_ALLOW_LIST_GOD, .type = UFSRV_CONFIG_VALUE_ARRAY },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_SSL_COMMAND_CONSOLE_ALLOW_LIST_BROWSER, .type = UFSRV_CONFIG_VALUE_ARRAY },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_SSL_COMMAND_CONSOLE_DENY_LIST, .type = UFSRV_CONFIG_VALUE_ARRAY },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_SSL_COMMAND_CONSOLE_CLIENT_IS_ENABLED, .type = UFSRV_CONFIG_VALUE_BOOL },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_SSL_COMMAND_CONSOLE_IS_CLIENT_CERTIFICATE_REQUIRED, .type = UFSRV_CONFIG_VALUE_BOOL },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_SSL_COMMAND_CONSOLE_CLIENT_CERTIFICATE_FILE, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_SSL_COMMAND_CONSOLE_CLIENT_KEY_FILE, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_STATS_BACKEND_ADDRESS, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_STATS_BACKEND_PORT, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_USER_TIMEOUTS_UNAUTHENTICATED_TIMEOUT, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_USER_TIMEOUTS_CONNECTED_TIMEOUT, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_USER_TIMEOUTS_SUSPENDED_TIMEOUT, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_BUFFER_SIZES_INCOMING_BUFFER_SIZE, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_BUFFER_SIZES_INCOMING_BUFFER_MAXSIZE, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_BUFFER_SIZES_OUTGOING_BUFFER_SIZE, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_BUFFER_SIZES_HOLDING_BUFFER_SIZE, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_MEMORY_SPECS_SESSIONS_PER_ALLOC_GROUP, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_MEMORY_SPECS_ALLOCATION_GROUPS, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_MEMORY_SPECS_ALLOCATION_THRESHOLD, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_SYS_RUNTIME_RUN_AS_USER, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_SYS_RUNTIME_CHROOT, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_FILELOADER_ENABLED, .type = UFSRV_CONFIG_VALUE_BOOL },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_FILELOADER_REGISTRY_SIZE, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_FILELOADER_REGISTRY_HOT_LOADED_SIZE, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_FILELOADER_BLACK_LISTED_FILES_REGISTRY, .type = UFSRV_CONFIG_VALUE_ARRAY },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_FILELOADER_FILES_REGISTRY, .type = UFSRV_CONFIG_VALUE_ARRAY },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_LOGGER_DRIVER, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_LOGGER_FORMAT, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_LOGGER_PREFIX, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_OAUTH_ACCESS_TOKEN_FORMAT, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_OAUTH_JWT_ISSUER, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_OAUTH_JWT_SIGNING_KEY, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_OAUTH_JWT_ACTIVE_KID, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_RATELIMIT_IS_ENABLED, .type = UFSRV_CONFIG_VALUE_BOOL },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_RATELIMIT_IS_BYPASS_PRIVATE_IP, .type = UFSRV_CONFIG_VALUE_BOOL },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_EMAIL_API_KEY, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrv", .key = CONF_FILE_LITERAL_EMAIL_API_URL, .type = UFSRV_CONFIG_VALUE_STR },
  {0},
};

#endif /* UFSRVSTUN_DATA_CONFIG_SYMBOLS_CORE_PRIV_H */
