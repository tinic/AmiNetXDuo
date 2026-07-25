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
    tls_time.c
    tls_runtime.c)

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
    target_compile_options(tls_library PRIVATE -Wall -Wextra)

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
    target_link_options(tls_library PRIVATE -nostartfiles)

    if(AMIGA_SIZE)
        add_custom_command(TARGET tls_library POST_BUILD
            COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:tls_library>"
            COMMENT "tls.library size")
    endif()
endif()

# ---------------------------------------------------------- trust store ---
#
# tools/mkcertstore.py turns a PEM bundle into DEVS:Internet/certificates.  The
# bundle is not vendored: it changes every few weeks, it is 186 KB of somebody
# else's data, and a stale copy in git is worse than none.  Point
# AMINETXDUO_CA_BUNDLE at one, or let this find the host's.
set(AMINETXDUO_CA_BUNDLE "" CACHE FILEPATH
    "PEM CA bundle to build DEVS:Internet/certificates from")

if(NOT AMINETXDUO_CA_BUNDLE)
    foreach(candidate
            "${CMAKE_SOURCE_DIR}/dist/cacert.pem"
            "/etc/ssl/cert.pem"
            "/etc/ssl/certs/ca-certificates.crt"
            "/etc/pki/tls/certs/ca-bundle.crt")
        if(EXISTS "${candidate}")
            set(AMINETXDUO_CA_BUNDLE "${candidate}")
            break()
        endif()
    endforeach()
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)

if(Python3_FOUND AND AMINETXDUO_CA_BUNDLE AND EXISTS "${AMINETXDUO_CA_BUNDLE}")
    add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/certificates"
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/mkcertstore.py"
                --output "${CMAKE_BINARY_DIR}/certificates"
                "${AMINETXDUO_CA_BUNDLE}"
        DEPENDS "${AMINETXDUO_CA_BUNDLE}"
                "${CMAKE_SOURCE_DIR}/tools/mkcertstore.py"
        COMMENT "Building the trust store from ${AMINETXDUO_CA_BUNDLE}"
        VERBATIM)
    add_custom_target(certstore ALL DEPENDS "${CMAKE_BINARY_DIR}/certificates")
    message(STATUS "  trust store .. ${AMINETXDUO_CA_BUNDLE}")
else()
    add_custom_target(certstore
        COMMAND ${CMAKE_COMMAND} -E echo
                "No CA bundle found. Set -DAMINETXDUO_CA_BUNDLE=<cacert.pem>.")
    message(STATUS "  trust store .. NOT BUILT (no CA bundle; set AMINETXDUO_CA_BUNDLE)")
endif()
