# Writes muse_http_proxy_env.h from the build environment.
# MUSE_HTTP_PROXY_HOST set: HTTP CONNECT to that IPv4 address.
# Unset: the firmware dials Muse directly (transparent-proxy LAN).
# MUSE_HTTP_PROXY_PORT defaults to 7890 when the host is set.
# The header is rewritten only when the contents change.

if(NOT DEFINED OUT_FILE OR OUT_FILE STREQUAL "")
    message(FATAL_ERROR "OUT_FILE is required")
endif()

set(host "$ENV{MUSE_HTTP_PROXY_HOST}")
set(port "$ENV{MUSE_HTTP_PROXY_PORT}")
string(STRIP "${host}" host)
string(STRIP "${port}" port)

set(enable 0)
if(NOT host STREQUAL "")
    set(enable 1)
    if(port STREQUAL "")
        set(port 7890)
    endif()
    if(NOT host MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR
            "MUSE_HTTP_PROXY_HOST must be an IPv4 address, got '${host}'")
    endif()
    string(REGEX MATCHALL "[0-9]+" octets "${host}")
    foreach(octet IN LISTS octets)
        if(octet GREATER 255)
            message(FATAL_ERROR
                "MUSE_HTTP_PROXY_HOST must be an IPv4 address, got '${host}'")
        endif()
    endforeach()
    if(NOT port MATCHES "^[0-9]+$" OR port LESS 1 OR port GREATER 65535)
        message(FATAL_ERROR
            "MUSE_HTTP_PROXY_PORT must be 1..65535, got '${port}'")
    endif()
else()
    set(host "")
    set(port 0)
endif()

get_filename_component(out_dir "${OUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${out_dir}")

set(content
"#pragma once
/* Generated from MUSE_HTTP_PROXY_HOST and MUSE_HTTP_PROXY_PORT. Do not edit. */
#define MUSE_HTTP_PROXY_ENABLE ${enable}
#define MUSE_HTTP_PROXY_HOST \"${host}\"
#define MUSE_HTTP_PROXY_PORT ${port}
")

if(EXISTS "${OUT_FILE}")
    file(READ "${OUT_FILE}" previous)
    if(previous STREQUAL content)
        return()
    endif()
endif()
file(WRITE "${OUT_FILE}" "${content}")
