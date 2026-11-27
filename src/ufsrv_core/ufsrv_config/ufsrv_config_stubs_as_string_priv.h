/*
 * ufsrv_config_stubs_as_string_priv_generated.h — MERGED by ufsrv_config_compose_host.
 * This file was seeded from generated ufsrv_config_stubs_as_string_priv.h and is now HAND MAINTAINED.
 *
 * Provenance: Merge of the following library headers:
 *   - /home/devops/repos/1.ufsrv/ufsrvcorelib/src/ufsrv_core/ufsrv_config/ufsrv_config_stubs_as_string_priv.h
 *   - /home/devops/repos/1.ufsrv/ufsrvstun/src/ufsrv_core/ufsrv_config/ufsrv_config_stubs_as_string_priv.h
 */
#ifndef UFSRV_CONFIG_STUBS_AS_STRING_PRIV_H
#define UFSRV_CONFIG_STUBS_AS_STRING_PRIV_H

#include <ufsrvcorelib/ufsrv_core/ufsrv_config/ufsrv_config_value_type.h>
#include <uflib/main_types.h>

/* === from: /home/devops/repos/1.ufsrv/ufsrvcorelib/src/ufsrv_core/ufsrv_config/ufsrv_config_stubs_as_string_priv.h ===
 */
BufferDescriptor *UfsrvConfigListenersAsString_CollectionDescriptor(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigConfFileStoreOperatingStateAsString_enum_UfsrvConfigStoreOperatingState(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigRuntimeStoreOperatingStateAsString_enum_UfsrvConfigStoreOperatingState(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigServerProcessModeAsString_enum_ServerProcessMode(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigServerRunModeAsString_enum_ServerRunMode(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigServerCpuAffinityAsString_enum_CpuAffinityPolicy(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigLuaHandleAsString_lua_State_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigInstrumentationBackendAsString_InstrumentationBackend_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigInstrumentationBackendServerAsString_struct_sockaddr_in(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigSessionsDelegatorAsString_SessionsDelegator_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigWorkDelegatorPipeAsString_InstanceHolderForSession_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigPipefdsAsString_int_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigSslCommandConsoleAsString_SSL_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigSslCtxAsString_SSL_CTX_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigSslUserCtxAsString_SSL_CTX_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigThreadHandleTimerDelegatorAsString_pthread_t(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigFileLoaderServiceAsString_FileLoaderServiceDescriptor(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigUfLoggerAsString_UfLogger_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigOauthSettingsAsString_UfsrvConfigOauth(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigUfsrvResultAsString_UFSRVResult(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigThreadsSubsystemUfsrvOnceAsString_pthread_once_t(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigSslMutexesAsString_pthread_mutex_t_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigSslCommandConsoleAllowListGodAsString_DistinctArray(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigSslCommandConsoleAllowListBrowserAsString_DistinctArray(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigSslCommandConsoleDenyListAsString_DistinctArray(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigFileloaderBlackListedFilesRegistryAsString_CollectionDescriptor(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
BufferDescriptor * UfsrvConfigFileloaderFilesRegistryAsString_CollectionDescriptor(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);

/* === from: /home/devops/repos/1.ufsrv/ufsrvstun/src/ufsrv_core/ufsrv_config/ufsrv_config_stubs_as_string_priv.h ===
 */
BufferDescriptor *UfsrvConfigStunTurnPeerAllowCidrsAsString_CollectionDescriptor(int field_index, UfsrvConfigValue *value, BufferDescriptor *buffer);
BufferDescriptor *UfsrvConfigStunTurnPeerDenyCidrsAsString_CollectionDescriptor(int field_index, UfsrvConfigValue *value, BufferDescriptor *buffer);

#endif /* UFSRV_CONFIG_STUBS_AS_STRING_PRIV_H */
