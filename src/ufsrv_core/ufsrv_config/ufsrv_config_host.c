/**
 * @file ufsrv_config_host.c
 * @brief STUN host configuration bootstrap and Lua-to-store transfer.
 */

#ifdef HAVE_CONFIG_UFSRVSTUN_H
#include <config_ufsrvstun.h>
#endif

#include <arpa/inet.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uflib/buffer_descriptor/buffer_descriptor.h>
#include <ufsrv_core/configfile_processor/configfile_processor.h>
#include <ufsrv_core/configfile_processor/configfile_processor_symbol_table_type.h>
#include <ufsrv_core/configfile_processor/configfile_validator_descriptor_type.h>
#include <ufsrv_core/ufsrv_config/ufsrv_config.h>
#include <ufsrv_core/ufsrv_config/ufsrv_config_secret.h>
#include <ufsrv_core/ufsrv_config/ufsrv_config_pretty_printers.h>
#include <ufsrv_core/ufsrv_config/ufsrv_config_store_operating_state_enum.h>
#include <ufsrvcorelib/ufsrv_core/main_utils.h>

#include "protocol_stun.h"
#include "net_addr.h"
#include "stun_diagnostics.h"
#include "ufsrvstun_config.h"

#include <string.h>

#include <uflib/collection_descriptor_type.h>

#include <ufsrvcorelib/ufsrv_core/delegator_sessions/ufsrv_listener_descriptor_type.h>
#include "turn/model/turn_relay_policy.h"
#include "generated/data_ufsrv_config_field_tables_generated.c"
#include "generated/data_ufsrv_config_literal_to_field_map_priv_generated.h"

#include "data_config_symbols_core_priv.h"
#include "data_config_symbols_adjunct_priv.h"


static void
ReportValidation(const char *scope, const char *key, const char *severity, const char *message)
{
  fprintf(stderr, "%s %s %s: %s\n", severity, scope, key, message);
}

static void
ReportSymbolError(const ConfFileSymbolTableEntry *entry, const char *message)
{
  ReportValidation(entry->scope, entry->key, "error", message);
}

static bool
ConfigPath(char *out, size_t capacity, const char *directory, const char *name)
{
  int length = snprintf(out, capacity, "%s%s", directory, name);
  if (length >= 0 && (size_t)length < capacity) return true;
  STUN_CONFIG_ERR("%s", "Config path is too long");
  return false;
}

static bool
ValidateDtlsSettings(void)
{
  if (UfsrvConfigStunDtlsIsEnabled() &&
      (!*UfsrvConfigStunDtlsCertificateChainFile() || !*UfsrvConfigStunDtlsPrivateKeyFile())) {
    STUN_CONFIG_ERR("%s", "dtls.certificate_chain_file and dtls.private_key_file are required when DTLS is enabled");
    return false;
  }
  return true;
}

static bool
sValidateTransferredSymbols(const ConfFileSymbolTableEntry *table)
{
  bool ok = true;

  for (const ConfFileSymbolTableEntry *symbol = table; symbol->scope; symbol++) {
    if (symbol->value.source != UFSRV_CONFIG_SOURCE_CONFIG_FILE) continue;

    UfsrvConfigFieldEntry *field = UfsrvConfigConfFileStoreInstance(symbol->field_index);
    if (symbol->type == UFSRV_CONFIG_VALUE_STR &&
        symbol->value.type_allocation_sz > field->config.type_allocation_sz) {
      ReportSymbolError(symbol, "string exceeds backing store capacity");
      ok = false;
    }
  }

  return ok;
}

static void
sReleaseTransferredSymbols(ConfFileSymbolTableEntry *table)
{
  for (ConfFileSymbolTableEntry *symbol = table; symbol->scope; symbol++) {
    if (symbol->type == UFSRV_CONFIG_VALUE_STR) free(symbol->value.stored_value.str_val);
    if (symbol->type == UFSRV_CONFIG_VALUE_ARRAY) free(symbol->value.stored_value.blob_val);
    symbol->value = (UfsrvConfigValue){0};
  }
}

bool
StunConfigProcessFile(bool strict)
{
  ConfigFileValidatorDescriptor validator;
  ConfigProvideValidatorDescriptor(&validator, strict, CONFIG_UFSRVSTUN_CONFIGFILE_ADJUNCT_SCHEMA,
                                   ReportValidation);
  validator.core_symbols              = g_ufsrv_config_symbols;
  validator.adjunct_symbols           = g_app_config_symbols;
  validator.associative_lookup_map    = ufsrv_config_key_map;
  validator.associative_lookup_map_sz = UFSRV_CONFIG_KEY_MAP_COUNT;

  /* An absolute --config-file path is used verbatim; the shared helper always
   * joins it onto <config-dir>/etc. */
  const char *config_file = UfsrvConfigConfigFile();
  if (config_file[0] == '/') {
    snprintf(validator.config_file, sizeof(validator.config_file), "%s", config_file);
  }
  ConfigProcessFile(&validator);

  bool ok = sValidateTransferredSymbols(g_ufsrv_config_symbols) &&
            sValidateTransferredSymbols(g_app_config_symbols);
  if (ok) {
    ConfigPopulateConfFileStoreFromLua(g_ufsrv_config_symbols);
    ConfigPopulateConfFileStoreFromLua(g_app_config_symbols);
  }
  sReleaseTransferredSymbols(g_ufsrv_config_symbols);
  sReleaseTransferredSymbols(g_app_config_symbols);
  if (!ok) return false;

  /* ── STUN protocol-domain validation ─────────────────────────────────── */
  const char *fingerprint_policy = UfsrvConfigStunFingerprintPolicy();
  if (strcmp(fingerprint_policy, "optional") != 0 && strcmp(fingerprint_policy, "required") != 0) {
    STUN_CONFIG_ERR("%s", "fingerprint_policy must be optional or required");
    return false;
  }

  if (UfsrvConfigSslCommandConsoleIsEnabled()) {
    struct in_addr console_address;
    if (inet_pton(AF_INET, UfsrvConfigCommandConsoleAddress(), &console_address) != 1) {
      STUN_CONFIG_ERR("%s", "command_console_address must be a numeric IPv4 address");
      return false;
    }
  }
  const char *nonce_secret = UfsrvConfigStunTurnNonceSecret();
  if (*nonce_secret && strncmp(nonce_secret, SECRET_PREFIX, SECRET_PREFIX_LEN) != 0) {
    STUN_CONFIG_ERR("%s", "turn.nonce_secret must be an enc:v1 envelope");
    return false;
  }
  const char *provider = UfsrvConfigStunTurnCredentialProvider();
  const bool standalone = strcmp(provider, "standalone") == 0;
  if (!standalone && strcmp(provider, "unfacd") != 0) {
    STUN_CONFIG_ERR("%s", "turn.credential_provider must be standalone or unfacd");
    return false;
  }
  const char *realm_secret = UfsrvConfigStunTurnRealmCredentialSecret();
  if (*realm_secret && strncmp(realm_secret, SECRET_PREFIX, SECRET_PREFIX_LEN) != 0) {
    STUN_CONFIG_ERR("%s", "turn.realm_credential_secret must be an enc:v1 envelope");
    return false;
  }
  if (UfsrvConfigStunTurnIsEnabled()) {
    if (!*UfsrvConfigStunTurnRealm() || !*nonce_secret) {
      STUN_CONFIG_ERR("%s", "turn.realm and turn.nonce_secret are required when TURN is enabled");
      return false;
    }
    if (standalone ? !*UfsrvConfigStunTurnCredentialFile() : !*realm_secret) {
      STUN_CONFIG_ERR("%s", standalone ? "The standalone provider requires turn.credential_file" :
            "The unfacd provider requires turn.realm_credential_secret");
      return false;
    }
  }
  if (UfsrvConfigStunTurnAllocationLifetimeSeconds() > UfsrvConfigStunTurnMaxAllocationLifetimeSeconds()) {
    STUN_CONFIG_ERR("%s", "turn.allocation_lifetime_seconds must not exceed turn.max_allocation_lifetime_seconds");
    return false;
  }
  if (!ValidateDtlsSettings()) return false;

  size_t shards = UfsrvConfigStunShardCount();
  size_t workers_per_shard = UfsrvConfigStunWorkersPerShard();
  if (shards == 0 || workers_per_shard == 0 || shards > SIZE_MAX / workers_per_shard) {
    STUN_CONFIG_ERR("%s", "shard_count and workers_per_shard must fit the total worker count");
    return false;
  }
  return true;
}

static BufferDescriptor *
RedactTurnSecret(int field, UfsrvConfigValue *value, BufferDescriptor *buffer)
{
  (void)field;
  (void)value;
  return BufferDescriptorAppendFormatted(buffer, "%s", "(redacted)") == 0 ? buffer : NULL;
}

void
StunConfigBootstrap(void)
{
  ufsrv_config_field_table[UFSRV_CONFIG_FIELD_STUN_TURN_NONCE_SECRET].lifecycle.on_as_string =
      RedactTurnSecret;
  ufsrv_config_field_table[UFSRV_CONFIG_FIELD_STUN_TURN_REALM_CREDENTIAL_SECRET].lifecycle.on_as_string =
      RedactTurnSecret;
  UfsrvConfigRuntimeStoreInit(ufsrv_runtime_field_table, ufsrv_runtime_field_defaults_table);
  UfsrvConfigConfFileStoreInit(ufsrv_config_field_table, ufsrv_config_field_defaults_table);
  UfsrvConfigInflateRuntimeStore();
  UfsrvConfigInflateConfigStore();
  UfsrvConfigPopulateDefaults(ufsrv_config_field_defaults_table, ufsrv_runtime_field_defaults_table, false);

  UfsrvConfigSetConfigDir(&(UfsrvConfigItem){.value = {
      .stored_value.str_val = UFSRVSTUN_CONFIG_DIR, .type_allocation_sz = sizeof(UFSRVSTUN_CONFIG_DIR)}},
      UFSRV_CONFIG_SOURCE_DEFAULTS);
  UfsrvConfigSetConfigFile(&(UfsrvConfigItem){.value = {
      .stored_value.str_val = "ufsrvstun-1.lua", .type_allocation_sz = sizeof("ufsrvstun-1.lua")}},
      UFSRV_CONFIG_SOURCE_DEFAULTS);
  UfsrvConfigSetServerClass(&(UfsrvConfigItem){.value = {
      .stored_value.str_val = "ufsrvstun", .type_allocation_sz = sizeof("ufsrvstun")}},
      UFSRV_CONFIG_SOURCE_RUNTIME);
}

bool
StunConfigDumpJson(const char *path)
{
  BufferDescriptor runtime = {0};
  BufferDescriptor config = {0};
  bool ok = JsonFormatConfigStore(ufsrv_runtime_field_table, UFSRV_RUNTIME_FIELD_COUNT,
                                 PRETTY_PRINT_SCOPE_DIGEST, &runtime) == 0 &&
            JsonFormatConfigStore(ufsrv_config_field_table, UFSRV_CONFIG_FIELD_COUNT,
                                 PRETTY_PRINT_SCOPE_DIGEST, &config) == 0;
  if (ok) {
    FILE *out = path ? fopen(path, "w") : stdout;
    ok = out && fprintf(out, "{\"runtime\":%s,\"config\":%s}\n", runtime.data, config.data) >= 0;
    if (out && path && fclose(out) != 0) ok = false;
  }
  if (!ok) STUN_CONFIG_ERR("%s", "Could not dump configuration JSON");
  BufferDescriptorRelease(&config);
  BufferDescriptorRelease(&runtime);
  return ok;
}

void
StunConfigDestroy(void)
{
  TurnRelayPolicyDestroy();
  UfsrvConfigRuntimeTeardown();
  for (int i = UFSRV_CONFIG_FIELD_COUNT; i-- > 0;) {
    UfsrvConfigFieldEntry *field = UfsrvConfigConfFileStoreInstance(i);
    if (field->lifecycle.on_teardown) field->lifecycle.on_teardown(i, &field->config);
  }
}
