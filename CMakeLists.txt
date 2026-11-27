cmake_minimum_required(VERSION 3.21)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake-modules")
project(ufsrvstun LANGUAGES C)

find_package(ufsrvcorelib CONFIG REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

# ── Debug verbosity ─────────────────────────────────────────────────────────
# L3 implies L2, and either implies the base debug build switch. This is this
# repository's own gate; every package carries its own.
option(UFSRVSTUN_DEBUG_BUILD_L2 "Verbose level-2 debug logging" OFF)
option(UFSRVSTUN_DEBUG_BUILD_L3 "Very verbose level-3 debug logging (per-object traces)" OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(UFSRVSTUN_DEBUG_BUILD 1)
else()
    set(UFSRVSTUN_DEBUG_BUILD 0)
endif()
if(UFSRVSTUN_DEBUG_BUILD_L3)
    set(UFSRVSTUN_DEBUG_L3 1)
    set(UFSRVSTUN_DEBUG_L2 1)
else()
    set(UFSRVSTUN_DEBUG_L3 0)
    if(UFSRVSTUN_DEBUG_BUILD_L2)
        set(UFSRVSTUN_DEBUG_L2 1)
    else()
        set(UFSRVSTUN_DEBUG_L2 0)
    endif()
endif()
if(UFSRVSTUN_DEBUG_L2 OR UFSRVSTUN_DEBUG_L3)
    set(UFSRVSTUN_DEBUG_BUILD 1)
endif()
add_compile_definitions(UFSRVSTUN_DEBUG_BUILD=${UFSRVSTUN_DEBUG_BUILD}
                        UFSRVSTUN_DEBUG_BUILD_L2=${UFSRVSTUN_DEBUG_L2}
                        UFSRVSTUN_DEBUG_BUILD_L3=${UFSRVSTUN_DEBUG_L3}
                        HAVE_CONFIG_UFSRVSTUN_H)

include(config_defaults_definitions)
find_path(VALGRIND_INCLUDE_DIR valgrind/valgrind.h
    HINTS ${VALGRIND_ROOT}/include /opt/include)
include(CheckIncludeFile)
if(VALGRIND_INCLUDE_DIR)
    set(CMAKE_REQUIRED_INCLUDES "${VALGRIND_INCLUDE_DIR}")
endif()
check_include_file(valgrind/valgrind.h HAVE_VALGRIND)
check_include_file(valgrind/drd.h HAVE_VALGRIND_DRD)
check_include_file(valgrind/memcheck.h HAVE_VALGRIND_MEMCHECK)
unset(CMAKE_REQUIRED_INCLUDES)

configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/config_ufsrvstun.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/config_ufsrvstun.h
        @ONLY
)

# Generated config header lands in the build root; make it reachable to every
# target that does #include <config_ufsrvstun.h> under HAVE_CONFIG_UFSRVSTUN_H.
include_directories(${CMAKE_BINARY_DIR})

add_library(ufsrvstun_turn_dispatch STATIC src/protocol/turn_dispatch.c)
target_include_directories(ufsrvstun_turn_dispatch PUBLIC src/include)
target_link_libraries(ufsrvstun_turn_dispatch PRIVATE ufsrvcorelib::ufsrvcorelib)

add_library(ufsrvstun_turn_credentials STATIC
    src/turn/auth/turn_credential.c
    src/turn/auth/turn_unfacd_credential.c
)
target_include_directories(ufsrvstun_turn_credentials PUBLIC src/include)
target_link_libraries(ufsrvstun_turn_credentials PUBLIC ufsrvcorelib::ufsrvcorelib PRIVATE OpenSSL::Crypto)

add_library(ufsrvstun_turn_auth STATIC src/turn/auth/turn_auth.c)
target_include_directories(ufsrvstun_turn_auth PUBLIC src/include)
target_link_libraries(ufsrvstun_turn_auth PRIVATE ufsrvstun_turn_credentials ufsrvcorelib::ufsrvcorelib)

add_compile_options(-fblocks)

add_executable(ufsrvstun src/main.c src/utils_console_handlers.c src/protocol/protocol_stun.c src/turn/auth/turn_auth_runtime.c
    src/turn/model/turn_session.c
    src/turn/model/turn_allocation.c)
add_custom_target(ufsrvstun_version
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/src/version.c.sh"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/version.c"
    COMMENT "Generating version.c with a fresh compilation timestamp"
    VERBATIM
)
target_sources(ufsrvstun PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/version.c")
add_dependencies(ufsrvstun ufsrvstun_version)

target_include_directories(ufsrvstun PRIVATE src/include)
target_link_libraries(ufsrvstun PRIVATE ufsrvcorelib::ufsrvcorelib ufsrvstun_turn_dispatch
                      ufsrvstun_turn_auth ufsrvstun_turn_credentials bsd)

option(UFSRV_CONFIG_COMPOSE "Enable the ufsrv_config maintainer targets" ON)
if(UFSRV_CONFIG_COMPOSE)
    # One implementation of the pipeline lives in ufsrvcorelib and is exported by
    # its package config; this host carries only site data (ufsrv_config_site.cmake).
    if(NOT DEFINED ufsrvcorelib_HOST_MODULES_DIR OR ufsrvcorelib_HOST_MODULES_DIR STREQUAL "")
        message(FATAL_ERROR
            "ufsrv_config: ufsrvcorelib does not export ufsrvcorelib_HOST_MODULES_DIR.\n"
            "  Reinstall ufsrvcorelib (>= the release that ships cmake-modules/ufsrv_config/host).")
    endif()
    set(_UFSRV_CONFIG_HOST_MODULES_DIR "${ufsrvcorelib_HOST_MODULES_DIR}")

    include("${_UFSRV_CONFIG_HOST_MODULES_DIR}/ufsrv_config_clean_host.cmake")

    include("${_UFSRV_CONFIG_HOST_MODULES_DIR}/ufsrv_config_bootstrap_host.cmake")

    add_custom_target(ufsrv_config_compose_host
        COMMAND ${CMAKE_COMMAND}
            "-DUFSRV_CONFIG_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DUFSRV_CONFIG_BINARY_DIR=${CMAKE_BINARY_DIR}"
            "-DUFSRV_CONFIG_C_COMPILER=${CMAKE_C_COMPILER}"
            "-DUFSRV_CONFIG_HOST_MODULES_DIR=${_UFSRV_CONFIG_HOST_MODULES_DIR}"
            "-DUFSRV_CONFIG_HOST_SITE_FILE=${CMAKE_SOURCE_DIR}/cmake-modules/ufsrv_config/host/ufsrv_config_site.cmake"
            -P "${_UFSRV_CONFIG_HOST_MODULES_DIR}/ufsrv_config_compose_host.cmake"
        COMMENT "ufsrv_config: compose (clone → generate → compose → deep-install → rewrite)"
        VERBATIM)

    add_custom_target(ufsrv_config_check_host
        COMMAND ${CMAKE_COMMAND}
            "-DUFSRV_CONFIG_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
            -P "${_UFSRV_CONFIG_HOST_MODULES_DIR}/ufsrv_config_check_host.cmake"
        COMMENT "ufsrv_config: integrity check (deep tree complete)"
        VERBATIM)
    set(_ufsrv_config_check_target ufsrv_config_check_host)
endif()

set(_config_dir "${CMAKE_CURRENT_SOURCE_DIR}/src/ufsrv_core/ufsrv_config")
set(_config_generated_sources
    "${_config_dir}/ufsrv_config.c"
    "${_config_dir}/ufsrv_config_generic_callbacks.c"
    "${_config_dir}/generated/ufsrv_config_getters_setters_generated.c")
# The composed config module defines the generated UfsrvConfig* accessors that
# corelib references; it is compiled into every executable that links corelib.
set(_ufsrv_config_sources
    "${_config_dir}/ufsrv_config_host.c"
    "${_config_dir}/ufsrv_config_stubs_turn_cidrs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/turn/model/turn_relay_policy.c"
    ${_config_generated_sources})
set(_ufsrv_config_include_dirs
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/include/ufsrvcorelib"
    "${_config_dir}"
    "${_config_dir}/generated")

target_sources(ufsrvstun PRIVATE ${_ufsrv_config_sources})
target_include_directories(ufsrvstun BEFORE PRIVATE ${_ufsrv_config_include_dirs})
if(_ufsrv_config_check_target)
    add_dependencies(ufsrvstun ${_ufsrv_config_check_target})
endif()

get_filename_component(_core_source "${CMAKE_CURRENT_SOURCE_DIR}/../ufsrvcorelib" ABSOLUTE)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/etc/lua")
foreach(_schema core_schema_ufsrv.lua core_schema_validator.lua)
    configure_file("${_core_source}/etc/${_schema}" "etc/lua/${_schema}" COPYONLY)
endforeach()
configure_file(etc/adjunct_schema_ufsrvstun.lua etc/lua/adjunct_schema_ufsrvstun.lua COPYONLY)
configure_file(etc/ufsrvstun-1.lua etc/ufsrvstun-1.lua COPYONLY)
target_compile_definitions(ufsrvstun PRIVATE
    UFSRVSTUN_CONFIG_DIR="${CMAKE_CURRENT_BINARY_DIR}")

option(UFSRV_ENABLE_TESTS "Enable the STUN loopback test" OFF)
if(UFSRV_ENABLE_TESTS)
    enable_testing()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(turn_auth_test tests/turn_auth.c)
    target_link_libraries(turn_auth_test PRIVATE ufsrvstun_turn_auth ufsrvstun_turn_credentials
                          ufsrvstun_turn_dispatch OpenSSL::Crypto
                          ufsrvcorelib::ufsrvcorelib bsd)
    target_sources(turn_auth_test PRIVATE ${_ufsrv_config_sources})
    target_include_directories(turn_auth_test BEFORE PRIVATE ${_ufsrv_config_include_dirs})
    target_compile_definitions(turn_auth_test PRIVATE UFSRVSTUN_CONFIG_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME turn_auth COMMAND turn_auth_test)
    add_executable(turn_credential_test tests/turn_credential.c)
    target_link_libraries(turn_credential_test PRIVATE ufsrvstun_turn_credentials
                          ufsrvcorelib::ufsrvcorelib bsd)
    target_sources(turn_credential_test PRIVATE ${_ufsrv_config_sources})
    target_include_directories(turn_credential_test BEFORE PRIVATE ${_ufsrv_config_include_dirs})
    target_compile_definitions(turn_credential_test PRIVATE UFSRVSTUN_CONFIG_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME turn_credential COMMAND turn_credential_test)
    add_executable(turn_dispatch_test tests/turn_dispatch.c)
    target_link_libraries(turn_dispatch_test PRIVATE ufsrvstun_turn_dispatch ufsrvcorelib::ufsrvcorelib)
    target_sources(turn_dispatch_test PRIVATE ${_ufsrv_config_sources})
    target_include_directories(turn_dispatch_test BEFORE PRIVATE ${_ufsrv_config_include_dirs})
    target_compile_definitions(turn_dispatch_test PRIVATE UFSRVSTUN_CONFIG_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME turn_dispatch COMMAND turn_dispatch_test)
    add_test(NAME stun_loopback
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/stun_loopback.py"
                "$<TARGET_FILE:ufsrvstun>")
endif()
