# tls.library -- TLS for ordinary Amiga programs.
#
# Built only when AMINETXDUO_TLS is ON, and shipped alongside a
# bsdsocket.library from the SAME build: the two share struct layouts and the
# private context ABI (include/aminetxduo/nxcontext.h).
#
# tls_library.c MUST stay first in the object order -- its "moveq #-1,d0 / rts"
# has to land at offset 0 of the first code hunk, the same rule
# bsdsocket.library and usergroup.library follow.
#
# WHAT IS DELIBERATELY *NOT* LINKED
#
#   netxduo, threadx, aminetxduo_netstack, aminetxduo_common.
#
#   The first three are the singleton that lives in bsdsocket.library; a second
#   copy here would be a second stack with no interfaces.  The fourth would
#   drag in src/common/ami_random.c and give the machine a second entropy pool.
#   All four are reached at run time through the private context instead, and
#   src/tlslib/tls_netx.c is the forwarding layer.
#
#   That is why the archives are named as FILES below rather than as CMake
#   targets: target_link_libraries(nx_secure) would pull `netxduo` in
#   transitively and undo the whole design, silently, at link time.
#
# SPDX-License-Identifier: MIT

option(AMINETXDUO_TLSLIB_LINK
       "Link LIBS:tls.library as an AmigaOS shared library" ON)

set(TLSLIB_SOURCES
    tls_library.c       # keep first
    tls_vectors.c
    tls_netx.c
    tls_conn.c
    tls_store.c
    tls_resume.c
    tls_time.c
    tls_runtime.c)

# TLS 1.2 session resumption.  nx_secure has none, and the two vendored
# functions that would have to change are reached through the linker rather
# than by editing third_party/ -- see the header comment in tls_resume.c for
# why --wrap was chosen over defining the symbols outright (450 lines of state
# machine that would have to be re-merged at every submodule bump).
#
# These belong to tls.library ONLY.  tls_handshake, tls_https and tls_bench
# link the same archives without them, so their behaviour and their baselines
# are untouched by construction.
set(TLSLIB_WRAP_OPTIONS
    -Wl,--wrap=_nx_secure_tls_send_clienthello
    -Wl,--wrap=_nx_secure_tls_client_handshake)

set(TLSLIB_INCLUDES
    ${AMINETXDUO_CORE_INCLUDES}
    "${AMINETXDUO_NETXDUO}/nx_secure/inc"
    "${AMINETXDUO_NETXDUO}/nx_secure/ports"
    "${AMINETXDUO_NETXDUO}/crypto_libraries/inc"
    "${CMAKE_SOURCE_DIR}/src/tls"
    "${CMAKE_CURRENT_SOURCE_DIR}")

if(AMINETXDUO_TLSLIB_LINK)
    add_executable(tls_library ${TLSLIB_SOURCES})
    set_target_properties(tls_library PROPERTIES OUTPUT_NAME "tls.library")
    target_include_directories(tls_library PRIVATE ${TLSLIB_INCLUDES})
    target_compile_options(tls_library PRIVATE -Wall -Wextra ${TLSLIB_TRACE_DEFS})

    add_dependencies(tls_library aminetxduo_tls nx_secure nx_crypto crypto68k)

    # Listed twice rather than wrapped in --start-group: nx_secure and
    # nx_crypto reference each other, aminetxduo_tls references both, and a
    # second pass is portable across every linker this toolchain has shipped.
    set(TLSLIB_ARCHIVES
        $<TARGET_FILE:aminetxduo_tls>
        $<TARGET_FILE:crypto68k>
        $<TARGET_FILE:nx_secure>
        $<TARGET_FILE:nx_crypto>)

    target_link_libraries(tls_library PRIVATE
        ${TLSLIB_ARCHIVES} ${TLSLIB_ARCHIVES})

    if(TARGET aminetxduo_m68k_rt)
        add_dependencies(tls_library aminetxduo_m68k_rt)
        target_link_libraries(tls_library PRIVATE $<TARGET_FILE:aminetxduo_m68k_rt>)
    endif()

    # -nostartfiles, not -nostdlib: no crt0 in a shared library, but nx_crypto
    # wants memcpy/memset/memcmp and the linker only pulls what is referenced.
    target_link_options(tls_library PRIVATE -nostartfiles ${TLSLIB_WRAP_OPTIONS})

    if(AMIGA_SIZE)
        add_custom_command(TARGET tls_library POST_BUILD
            COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:tls_library>"
            COMMENT "tls.library size")
    endif()
endif()

# ---------------------------------------------------------- trust store ---
#
# tools/mkcertstore.py turns a PEM bundle into DEVS:Internet/certificates.
#
# The bundle is a VENDORED, DATED, HASH-PINNED snapshot -- third_party/cacert/,
# which carries the provenance and the licence reasoning.  It used to be
# scavenged from the host (/etc/ssl/cert.pem and friends), and that was wrong
# in both directions: a release carried whatever roots the release engineer's
# laptop had, and a build on a machine with none shipped a tls.library that
# refused every connection with TLS_ERR_TRUSTSTORE while only warning about it.
#
# Two hashes are checked, and BOTH failures are fatal:
#
#   * the input bundle, against curl.se's published checksum, so a corrupted
#     or swapped snapshot cannot be built from;
#   * the generated store, against a pin recorded in the tree.  That second one
#     is what makes "any host produces the same bytes" a checked claim: if a
#     host CA file, a different bundle or an edited mkcertstore.py leaked into
#     the result, the digest moves and the build stops.
#
# -DAMINETXDUO_CA_BUNDLE=<file> still works, for a machine with its own set.
# It skips the output pin -- there is nothing to pin it to -- says so, and
# keeps a floor on the root count so a truncated bundle cannot pass.

set(AMINETXDUO_CA_BUNDLE "" CACHE FILEPATH
    "PEM CA bundle for DEVS:Internet/certificates (empty: the pinned snapshot)")

set(AMINETXDUO_CA_DIR "${CMAKE_SOURCE_DIR}/third_party/cacert")

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_FOUND)
    message(FATAL_ERROR
        "AMINETXDUO_TLS is ON and there is no python3.\n"
        "tools/mkcertstore.py builds DEVS:Internet/certificates, and a TLS "
        "build without a trust store refuses every connection.  Install "
        "python3, or configure with -DAMINETXDUO_TLS=OFF.")
endif()

# The pin on the generated store, and the number of roots below which the
# result is not a usable public trust store whatever its provenance.
set(CERTSTORE_MIN_ROOTS 40)
set(CERTSTORE_PIN "")

if(AMINETXDUO_CA_BUNDLE)
    if(NOT EXISTS "${AMINETXDUO_CA_BUNDLE}")
        message(FATAL_ERROR
            "-DAMINETXDUO_CA_BUNDLE=${AMINETXDUO_CA_BUNDLE} does not exist.")
    endif()
    message(STATUS "  trust store .. ${AMINETXDUO_CA_BUNDLE} (NOT the pinned snapshot)")
else()
    set(AMINETXDUO_CA_BUNDLE "${AMINETXDUO_CA_DIR}/cacert.pem")

    if(NOT EXISTS "${AMINETXDUO_CA_BUNDLE}")
        message(FATAL_ERROR
            "The vendored CA bundle is missing: ${AMINETXDUO_CA_BUNDLE}\n"
            "See third_party/cacert/README.md, or point "
            "-DAMINETXDUO_CA_BUNDLE at a PEM bundle of your own.")
    endif()

    # curl.se's published "<sha256>  cacert.pem" line, copied verbatim.
    file(READ "${AMINETXDUO_CA_DIR}/cacert.pem.sha256" CERTSTORE_BUNDLE_SUM)
    string(REGEX MATCH "[0-9a-fA-F]+" CERTSTORE_BUNDLE_SUM "${CERTSTORE_BUNDLE_SUM}")
    file(SHA256 "${AMINETXDUO_CA_BUNDLE}" CERTSTORE_BUNDLE_ACTUAL)
    if(NOT CERTSTORE_BUNDLE_ACTUAL STREQUAL CERTSTORE_BUNDLE_SUM)
        message(FATAL_ERROR
            "third_party/cacert/cacert.pem is not the pinned snapshot.\n"
            "  expected ${CERTSTORE_BUNDLE_SUM}\n"
            "  got      ${CERTSTORE_BUNDLE_ACTUAL}\n"
            "Restore it, or record a new pin -- third_party/cacert/README.md.")
    endif()

    file(READ "${AMINETXDUO_CA_DIR}/certificates.sha256" CERTSTORE_PIN)
    string(REGEX MATCH "[0-9a-fA-F]+" CERTSTORE_PIN "${CERTSTORE_PIN}")

    message(STATUS "  trust store .. third_party/cacert/cacert.pem (pinned)")
endif()

set(CERTSTORE_PIN_ARG "")
if(CERTSTORE_PIN)
    set(CERTSTORE_PIN_ARG --expect-sha256 "${CERTSTORE_PIN}")
endif()

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/certificates"
    COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/mkcertstore.py"
            --output "${CMAKE_BINARY_DIR}/certificates"
            --min-roots ${CERTSTORE_MIN_ROOTS}
            ${CERTSTORE_PIN_ARG}
            "${AMINETXDUO_CA_BUNDLE}"
    DEPENDS "${AMINETXDUO_CA_BUNDLE}"
            "${CMAKE_SOURCE_DIR}/tools/mkcertstore.py"
    COMMENT "Building the trust store from ${AMINETXDUO_CA_BUNDLE}"
    VERBATIM)
add_custom_target(certstore ALL DEPENDS "${CMAKE_BINARY_DIR}/certificates")
