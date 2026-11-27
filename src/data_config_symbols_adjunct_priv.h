/**
 * @file data_config_symbols_adjunct_priv.h
 * @brief Leaf (ufsrvstun scope) symbol table for this host.
 */

#ifndef UFSRVSTUN_DATA_CONFIG_SYMBOLS_ADJUNCT_PRIV_H
#define UFSRVSTUN_DATA_CONFIG_SYMBOLS_ADJUNCT_PRIV_H

#include <ufsrv_core/configfile_processor/configfile_processor_symbol_table_type.h>
#include <ufsrv_core/configfile_processor/ufsrv_config_literals_defs.h>
#include <ufsrv_core/ufsrv_config/generated/ufsrv_config_field_enums_generated.h>

static ConfFileSymbolTableEntry g_app_config_symbols[] = {
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_IS_ENABLED, .type = UFSRV_CONFIG_VALUE_BOOL },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_SHARD_COUNT, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_SHARD_HASH_TABLE_STARTUP_SIZE, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_WORKERS_PER_SHARD, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_FINGERPRINT_POLICY, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_MAX_REQUEST_BYTES, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_BINDING_RATE_LIMIT_PER_SOURCE, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_BINDING_RATE_LIMIT_BURST, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_DTLS_IS_ENABLED, .type = UFSRV_CONFIG_VALUE_BOOL },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_DTLS_BIND_ADDRESS, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_DTLS_PORT, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_DTLS_CERTIFICATE_CHAIN_FILE, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_DTLS_PRIVATE_KEY_FILE, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_DTLS_FLOWS_PER_SHARD, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_DTLS_FLOW_IDLE_TIMEOUT_SECONDS, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_DTLS_SENDS_PER_SHARD, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_DTLS_RELAY_MIN_PORT, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_DTLS_RELAY_MAX_PORT, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_TURN_IS_ENABLED, .type = UFSRV_CONFIG_VALUE_BOOL },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_TURN_ALLOCATION_LIFETIME_SECONDS, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_TURN_MAX_ALLOCATION_LIFETIME_SECONDS, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_TURN_MAX_ALLOCATIONS_PER_PRINCIPAL, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_TURN_FLOWS_PER_SHARD, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_TURN_FLOW_IDLE_TIMEOUT_SECONDS, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_TURN_SENDS_PER_SHARD, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_TURN_REALM, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_TURN_NONCE_LIFETIME_SECONDS, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_TURN_CREDENTIAL_PROVIDER, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_TURN_REALM_CREDENTIAL_SECRET, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_TURN_MAX_CREDENTIAL_LIFETIME_SECONDS, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_TURN_RELAY_BIND_ADDRESS, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_TURN_RELAY_ADVERTISED_ADDRESS, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_TURN_RELAY_MIN_PORT, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_TURN_RELAY_MAX_PORT, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_TURN_PEER_ALLOW_CIDRS, .type = UFSRV_CONFIG_VALUE_ARRAY },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_TURN_PEER_DENY_CIDRS, .type = UFSRV_CONFIG_VALUE_ARRAY },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_TURN_MAX_PERMISSIONS_PER_ALLOCATION, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_TURN_MAX_CHANNELS_PER_ALLOCATION, .type = UFSRV_CONFIG_VALUE_INT },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_TURN_CREDENTIAL_FILE, .type = UFSRV_CONFIG_VALUE_STR },
  { .scope = "ufsrvstun", .key = CONF_FILE_LITERAL_UFSRVSTUN_TURN_NONCE_SECRET, .type = UFSRV_CONFIG_VALUE_STR },
  {0},
};

#endif /* UFSRVSTUN_DATA_CONFIG_SYMBOLS_ADJUNCT_PRIV_H */
