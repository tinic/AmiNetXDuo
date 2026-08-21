# tls.library, TLS for ordinary Amiga programs.
#
# Built only when AMINETXDUO_TLS is ON, and shipped alongside a
# bsdsocket.library from the SAME build: the two share struct layouts and the
# private context ABI (include/aminetxduo/nxcontext.h).
#
# tls_library.c MUST stay first in the object order, its "moveq #-1,d0 / rts"
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
    tls_hostname.c
    tls_path.c
    tls_store.c
    tls_resume.c
    tls_expiry.c
    tls_time.c
    tls_runtime.c)

# TLS 1.2 session resumption.  nx_secure has none, and the two vendored
# functions that would have to change are reached through the linker rather
# than by editing third_party/, see the header comment in tls_resume.c for
# why --wrap was chosen over defining the symbols outright (450 lines of state
# machine that would have to be re-merged at every submodule bump).
#
# These belong to tls.library ONLY.  tls_handshake, tls_https and tls_bench
# link the same archives without them, so their behaviour and their baselines
# are untouched by construction.
set(TLSLIB_WRAP_OPTIONS
    -Wl,--wrap=_nx_secure_tls_send_clienthello
    -Wl,--wrap=_nx_secure_tls_client_handshake)

# -DTLSLIB_TRACE_DEFS=-DTLS_RESUME_TRACE puts a running commentary on the
# serial port: what went into each ClientHello, every handshake message type
# seen, and what the ServerHello echoed back against what was offered.  Off by
# default and worth keeping, because a resumption that silently does not happen
# looks exactly like one the server declined, and telling those two apart was
# the whole of finding the empty-session-ID rule (docs/RESEARCH.md 13.4).
set(TLSLIB_TRACE_DEFS "" CACHE STRING
    "Extra defines for tls.library, e.g. -DTLS_RESUME_TRACE")

set(TLSLIB_INCLUDES
    ${AMINETXDUO_CORE_INCLUDES}
    "${AMINETXDUO_NETXDUO}/nx_secure/inc"
    "${AMINETXDUO_NETXDUO}/nx_secure/ports"
    "${AMINETXDUO_NETXDUO}/crypto_libraries/inc"
    "${CMAKE_SOURCE_DIR}/src/tls"
    "${CMAKE_SOURCE_DIR}/src/crypto68k"
    "${CMAKE_CURRENT_SOURCE_DIR}")

if(AMINETXDUO_TLSLIB_LINK)
    add_executable(tls_library ${TLSLIB_SOURCES})
    set_target_properties(tls_library PROPERTIES OUTPUT_NAME "tls.library")

    # Nothing references a romtag; -flto removes it and the link still
    # succeeds.  See the -flto note in the top-level CMakeLists.txt.
    if(CMAKE_CROSSCOMPILING)
        target_link_options(tls_library PRIVATE "-Wl,-u,_tls_romtag")
    endif()

    target_include_directories(tls_library PRIVATE ${TLSLIB_INCLUDES})
    target_compile_options(tls_library PRIVATE -Wall -Wextra ${TLSLIB_TRACE_DEFS})

    # These have to match src/tls/CMakeLists.txt exactly.  They are set PUBLIC
    # on nx_secure there, which carries them to anything that links the TARGET,
    # but the archives below are linked by $<TARGET_FILE:...> path, and a raw
    # path carries no usage requirements at all.  So tls_conn.c was compiling
    # NX_SECURE_TLS_SESSION without the fields the 1.3 macro adds while
    # nx_secure operated on the same object with them: the same struct, two
    # layouts, one of them writing past where the other thinks it ends.
    target_compile_definitions(tls_library PRIVATE
        NX_SECURE_ENABLE_AEAD_CIPHER
        NX_SECURE_KEY_CLEAR
        NX_SECURE_TLS_DISABLE_SECURE_RENEGOTIATION
        NX_SECURE_TLS_ENABLE_TLS_1_3)

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
    #
    # --gc-sections is only 308 bytes here -- src/tls/ and nx_crypto are already
    # close to one function per file -- but it costs nothing and keeps the three
    # libraries linking the same way.  It composes with --wrap: the wrap target
    # is a reference like any other, so the wrapper is a root and the wrapped
    # symbol is reached through it.
    # A link map so tools/profiler/profreport.py --lib can name functions
    # INSIDE tls.library; without it the whole library is one bar.
    target_link_options(tls_library PRIVATE -nostartfiles -Wl,--gc-sections
                        "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/tls.library.map"
                        ${TLSLIB_WRAP_OPTIONS})

    # Before the size is printed, so the number reported is the number that
    # ships.  Same reasoning as src/tools/: profreport.py reads the linker map
    # and the objects rather than HUNK_SYMBOL, so the symbol table is 22 KB
    # nothing on the Amiga reads.
    if(CMAKE_STRIP AND NOT AMINETXDUO_KEEP_SYMBOLS)
        add_custom_command(TARGET tls_library POST_BUILD
            COMMAND "${CMAKE_STRIP}" "$<TARGET_FILE:tls_library>"
            VERBATIM)
    endif()

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
# The bundle is a VENDORED, DATED, HASH-PINNED snapshot, third_party/cacert/,
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
# It skips the output pin, there is nothing to pin it to, says so, and
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
            "Restore it, or record a new pin, third_party/cacert/README.md.")
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

# --------------------------------------------------------------- host test --
#
# tls_expiry.c is the one file here free of AmigaOS calls, and its rule is the
# kind whose failure is silent: a cached master secret that never ages out is
# a key on disk an attacker can use against captured traffic, and it looks
# exactly like resumption working.  Two of its four rules exist only for
# machines with no battery-backed clock, where a stamp is seconds since boot
# rather than since 1970.
#
#   cc -std=c11 -Wall -Wextra -Isrc/tlslib \
#      src/tlslib/test/test_tls_expiry.c src/tlslib/tls_expiry.c \
#      -o test_tls_expiry
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(test_tls_expiry
        test/test_tls_expiry.c
        tls_expiry.c)

    target_include_directories(test_tls_expiry PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")

    target_compile_options(test_tls_expiry PRIVATE -Wall -Wextra)
    set_target_properties(test_tls_expiry PROPERTIES C_STANDARD 11)

    add_test(NAME tls_session_expiry COMMAND test_tls_expiry)

    # ---------------------------------------------------------------------
    # Session resumption, the rest of it.
    #
    # tls_resume.c is 1,582 lines and until now had no test of any kind.  What
    # it decides is which cached master secret may be offered to whom, and
    # every way of getting that wrong is silent: a resumption that does not
    # happen looks like the server declining one, and a resumption that should
    # not have happened looks like a fast connection.
    #
    # It is not free of AmigaOS calls the way tls_expiry.c is, so this needs
    # more than -I: test/shim carries <proto/dos.h>, <proto/exec.h> and the two
    # dos headers, and the test defines them.  Open/Read/Write/Close land on
    # stdio and a real file, because the on-disk format is the part a different
    # build of this same library has to agree with.  tls_runtime.c comes along
    # so tls_alloc(), tls_bzero() and tls_strncpy() are the shipping ones.
    #
    # The defines have to match the tls_library target above EXACTLY, and
    # NX_SECURE_TLS_ENABLE_TLS_1_3 above all: tls_resume_secret_bound() has a
    # live #if on it, and it changes the layout of NX_SECURE_TLS_SESSION.  The
    # comment on that target says what happens when the two disagree.
    #
    # TLSLIB_NO_INLINE_STUBS is why <aminetxduo/tlslib.h> has that guard:
    # tls_internal.h includes the public header, and its inline stubs are m68k
    # assembly a host compiler cannot parse.  Nothing else in the tree defines
    # it, so the shipping library sees the header unchanged.
    add_executable(test_tls_resume
        test/test_tls_resume.c
        tls_resume.c
        tls_runtime.c
        tls_expiry.c)

    target_include_directories(test_tls_resume PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/test/shim"         # proto/, dos/, exec/
        "${CMAKE_SOURCE_DIR}/src/config/test/shim"      # the rest of exec/
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"      # tx_port.h, first
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src/tls"                   # tls.h
        "${CMAKE_SOURCE_DIR}/include"
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"     # nx_port.h
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"    # nx_user.h, ours
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_THREADX}/common/inc"
        "${AMINETXDUO_NETXDUO}/nx_secure/inc"
        "${AMINETXDUO_NETXDUO}/nx_secure/ports"
        "${AMINETXDUO_NETXDUO}/crypto_libraries/inc")

    target_compile_definitions(test_tls_resume PRIVATE
        NX_PACKET_ALIGNMENT=8
        NX_INCLUDE_USER_DEFINE_FILE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h"
        NX_SECURE_ENABLE_AEAD_CIPHER
        NX_SECURE_KEY_CLEAR
        NX_SECURE_TLS_ENABLE_TLS_1_3
        TLSLIB_NO_INLINE_STUBS)

    # -UAMINETXDUO_IPV6 for the reason tests/netstack/host's targets carry it:
    # it changes the layout of NX_PACKET, and the vendored headers here are the
    # linux port's.
    target_compile_options(test_tls_resume PRIVATE -Wall -Wextra -UAMINETXDUO_IPV6)
    set_target_properties(test_tls_resume PROPERTIES C_STANDARD 11)

    add_test(NAME tls_session_resume COMMAND test_tls_resume)

    # TLSClose() uses the ThreadX bracket as the global serialization for
    # NetX Secure's shared session list. Compile the shipping vector itself,
    # then discard its unrelated entry points at link time; the test records
    # the enter/end/delete/leave order and fails if deletion escapes again.
    add_executable(test_tls_close
        test/test_tls_close.c
        tls_conn.c
        tls_hostname.c
        tls_path.c)

    find_package(Threads REQUIRED)
    target_link_libraries(test_tls_close PRIVATE Threads::Threads)

    target_include_directories(test_tls_close PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/test/shim"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim"
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src/tls"
        "${CMAKE_SOURCE_DIR}/include"
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_THREADX}/common/inc"
        "${AMINETXDUO_NETXDUO}/nx_secure/inc"
        "${AMINETXDUO_NETXDUO}/nx_secure/ports"
        "${AMINETXDUO_NETXDUO}/crypto_libraries/inc")

    target_compile_definitions(test_tls_close PRIVATE
        NX_PACKET_ALIGNMENT=8
        NX_INCLUDE_USER_DEFINE_FILE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h"
        NX_SECURE_ENABLE_AEAD_CIPHER
        NX_SECURE_KEY_CLEAR
        NX_SECURE_TLS_DISABLE_SECURE_RENEGOTIATION
        NX_SECURE_TLS_ENABLE_TLS_1_3
        _POSIX_C_SOURCE=200809L
        TLSLIB_NO_INLINE_STUBS
        TLSLIB_HOST_TEST)

    target_compile_options(test_tls_close PRIVATE
        -Wall -Wextra -Wno-int-to-pointer-cast -UAMINETXDUO_IPV6
        -ffunction-sections -fdata-sections)
    if(APPLE)
        target_link_options(test_tls_close PRIVATE -Wl,-dead_strip)
    else()
        target_link_options(test_tls_close PRIVATE -Wl,--gc-sections)
    endif()
    set_target_properties(test_tls_close PROPERTIES C_STANDARD 11)

    add_test(NAME tls_close_serialization COMMAND test_tls_close)

    # tls.library may remain resident across an expunge and reload of
    # bsdsocket.library.  The borrowed context lives in bsdsocket.library's
    # segment, so every TLSOpen() must refresh it instead of keeping the first
    # pointer forever.
    add_executable(test_tls_netx_bind
        test/test_tls_netx_bind.c
        tls_netx.c)

    target_include_directories(test_tls_netx_bind PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/test/shim"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim"
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src/tls"
        "${CMAKE_SOURCE_DIR}/src/crypto68k"
        "${CMAKE_SOURCE_DIR}/include"
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_THREADX}/common/inc"
        "${AMINETXDUO_NETXDUO}/nx_secure/inc"
        "${AMINETXDUO_NETXDUO}/nx_secure/ports"
        "${AMINETXDUO_NETXDUO}/crypto_libraries/inc")

    target_compile_definitions(test_tls_netx_bind PRIVATE
        NX_PACKET_ALIGNMENT=8
        NX_INCLUDE_USER_DEFINE_FILE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h"
        NX_SECURE_ENABLE_AEAD_CIPHER
        NX_SECURE_KEY_CLEAR
        NX_SECURE_TLS_DISABLE_SECURE_RENEGOTIATION
        NX_SECURE_TLS_ENABLE_TLS_1_3
        TLSLIB_NO_INLINE_STUBS
        TLSLIB_HOST_TEST)

    target_compile_options(test_tls_netx_bind PRIVATE
        -Wall -Wextra -UAMINETXDUO_IPV6 -ffunction-sections -fdata-sections)
    if(APPLE)
        target_link_options(test_tls_netx_bind PRIVATE -Wl,-dead_strip)
    else()
        target_link_options(test_tls_netx_bind PRIVATE -Wl,--gc-sections)
    endif()
    set_target_properties(test_tls_netx_bind PROPERTIES C_STANDARD 11)

    add_test(NAME tls_context_rebind COMMAND test_tls_netx_bind)
endif()
