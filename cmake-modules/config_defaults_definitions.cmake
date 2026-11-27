# Provide defaults for configuration items the user did not pass via -D.
#
# ufsrvstun is a leaf host: the only host-owned configure-time value is the
# adjunct Lua schema it hands to the ufsrv_config validator.

if(NOT DEFINED CONFIG_UFSRVSTUN_CONFIGFILE_ADJUNCT_SCHEMA)
    set(CONFIG_UFSRVSTUN_CONFIGFILE_ADJUNCT_SCHEMA "adjunct_schema_ufsrvstun.lua" CACHE STRING
        "Lua adjunct schema definition for lua-base configuration files")
endif()

if(NOT DEFINED CONFIG_CONFIGFILE_DEFAULT_PID_DIR)
    set(CONFIG_CONFIGFILE_DEFAULT_PID_DIR "/var/run" CACHE STRING "Directory location of PID file")
endif()
