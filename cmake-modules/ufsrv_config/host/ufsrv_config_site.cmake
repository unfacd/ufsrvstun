#
# cmake-modules/ufsrv_config/host/ufsrv_config_site.cmake
#
# Site declaration for the ufsrvstun config composition.
# Consumed by ufsrv_config_compose_host.cmake through:
#
#   -DUFSRV_CONFIG_HOST_SITE_FILE=<this file>
#
# Declares the composition order (toolchain library first), each library's
# source mode, the canonical source for the invariant files/shim, and the
# leaf-specific catalogue/prefix overrides.
#
# Any value may be overridden from the command line (-D...) or by a wrapper
# site file that includes this one first.
#

# Composition order matters: the first entry owns the toolchain (generator,
# composer, verifier, literals header); leaves are appended last.
if(NOT DEFINED UFSRV_CONFIG_HOST_LIBRARIES)
    set(UFSRV_CONFIG_HOST_LIBRARIES "ufsrvcorelib;ufsrvstun")
endif()

# Sibling checkouts by convention: <repos>/ufsrvstun, <repos>/ufsrvcorelib.  local: = no clone, no network, and the toolchain is
# read from the same tree the application builds against.
get_filename_component(_site_sibling_root "${UFSRV_CONFIG_SOURCE_DIR}/.." ABSOLUTE)

if(NOT DEFINED UFSRV_CONFIG_HOST_SOURCE_ufsrvcorelib)
    set(UFSRV_CONFIG_HOST_SOURCE_ufsrvcorelib "local:${_site_sibling_root}/ufsrvcorelib")
endif()
if(NOT DEFINED UFSRV_CONFIG_HOST_SOURCE_ufsrvstun)
    set(UFSRV_CONFIG_HOST_SOURCE_ufsrvstun "local:${UFSRV_CONFIG_SOURCE_DIR}")
endif()

# Invariant ufsrv_config sources and the shim are corelib-owned.
if(NOT DEFINED UFSRV_CONFIG_HOST_CANONICAL_SUB)
    set(UFSRV_CONFIG_HOST_CANONICAL_SUB "${_site_sibling_root}/ufsrvcorelib")
endif()

# UFSRV_CONFIG_HOST_MODULES_DIR is supplied by the build (CMakeLists.txt) from
# the ufsrvcorelib package export — this site file declares data only.

# Leaf catalogue sits one directory deeper than the library convention.
if(NOT DEFINED UFSRV_CONFIG_COMPOSER_ufsrvstun_CATALOGUE)
    set(UFSRV_CONFIG_COMPOSER_ufsrvstun_CATALOGUE
        "dev/scripts/ufsrv_config_generator/ufsrvstun/ufsrv_config_generator_fields.c.input")
endif()

# The composed config module is installed once, under the ufsrvcorelib include
# prefix.  A leaf-specific prefix would emit <ufsrvstun/...> includes that
# neither ufsrv_config_rewrite_host.cmake nor the deep tree resolve.
if(NOT DEFINED UFSRV_CONFIG_COMPOSER_ufsrvstun_INCLUDE_PREFIX)
    set(UFSRV_CONFIG_COMPOSER_ufsrvstun_INCLUDE_PREFIX "ufsrvcorelib")
endif()
