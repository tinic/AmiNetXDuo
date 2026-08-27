# The nx_secure feasibility gate (docs/RESEARCH.md 9, decision 4).
#
#   cmake -S . -B build/cm -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
#         -DCMAKE_BUILD_TYPE=Release -DAMINETXDUO_TLS=ON
#   cmake --build build/cm --parallel --target tls_bench
#   AMINETXDUO_RUN_TAG=tls20 ./tools/amiberry-run.sh      -t 300 build/cm/tests/tls/tls_bench
#   AMINETXDUO_RUN_TAG=tls30 ./tools/amiberry-run.sh -c 68030 -t 300 build/cm/tests/tls/tls_bench

add_executable(tls_bench tls_bench.c)

# aminetxduo_common is here for the crash guard only; the benchmark deliberately
# does not start ThreadX (see the header comment in tls_bench.c).
target_link_libraries(tls_bench PRIVATE aminetxduo_tls aminetxduo_common netxduo_port)

if(AMIGA_NDK_INCLUDE)
    target_include_directories(tls_bench PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

set_target_properties(tls_bench PROPERTIES OUTPUT_NAME "tls_bench")

# --gc-sections for correctness, not size: under -flto the 64-bit helpers
# are synthesised in LTRANS, after libaminetxduo_m68k_rt.a is scanned, so
# libgcc.a(_muldi3) is pulled instead of ours, and its DWARF becomes
# loadable data hunks with no relocation table behind them.  The
# measurement and the two rejected alternatives are in
# tests/crypto68k/CMakeLists.txt.
target_link_options(tls_bench PRIVATE -Wl,--gc-sections)

if(AMIGA_SIZE)
    add_custom_command(TARGET tls_bench POST_BUILD
        COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:tls_bench>"
        COMMENT "tls_bench size")
endif()

# A real TLS 1.2 handshake, client and server both on the Amiga, over the
# in-tree RAM driver.  Needs ThreadX and NetX Duo up, unlike tls_bench.
#
# Three executables from one source; see the header comment in tls_handshake.c.
# They are split rather than looped in one binary because their run times differ
# by an order of magnitude, and a correctness regression should be catchable in
# a short run:
#
#   AMINETXDUO_RUN_TAG=hs   ./tools/amiberry-run.sh -t 300 build/tls/tests/tls/tls_handshake
#   AMINETXDUO_RUN_TAG=hsd  ./tools/amiberry-run.sh -t 900 build/tls/tests/tls/tls_decompose
#   AMINETXDUO_RUN_TAG=hsi  ./tools/amiberry-run.sh -t 900 build/tls/tests/tls/tls_interop
foreach(prog tls_handshake tls_decompose tls_interop)
    add_executable(${prog} tls_handshake.c)

    target_link_libraries(${prog} PRIVATE
        aminetxduo_tls aminetxduo_common netxduo_port)

    if(AMIGA_NDK_INCLUDE)
        target_include_directories(${prog} PRIVATE "${AMIGA_NDK_INCLUDE}")
    endif()

    set_target_properties(${prog} PROPERTIES OUTPUT_NAME "${prog}")

    # --gc-sections for correctness, not size: under -flto the 64-bit helpers
    # are synthesised in LTRANS, after libaminetxduo_m68k_rt.a is scanned, so
    # libgcc.a(_muldi3) is pulled instead of ours, and its DWARF becomes
    # loadable data hunks with no relocation table behind them.  The
    # measurement and the two rejected alternatives are in
    # tests/crypto68k/CMakeLists.txt.
    target_link_options(${prog} PRIVATE -Wl,--gc-sections)

    if(AMIGA_SIZE)
        add_custom_command(TARGET ${prog} POST_BUILD
            COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:${prog}>"
            COMMENT "${prog} size")
    endif()
endforeach()

target_compile_definitions(tls_decompose PRIVATE H_ROUNDS_DECOMPOSE=1)
target_compile_definitions(tls_interop   PRIVATE H_ROUNDS_INTEROP=1)

# A handshake against a real public HTTPS server, over SLIRP.  Built, never a
# baseline: it depends on the internet, on a third party's server and on a
# certificate that rotates.  See the header comment.
#
#   tests/tls/run-https.sh -b build/tls
# The decisive test for the API and the trust store: a program linked against
# NOTHING of ours, opening LIBS:bsdsocket.library and LIBS:tls.library by name
# and fetching a real HTTPS URL verified against DEVS:Internet/certificates.
#
#   tests/tls/run-api.sh -b build/tls
#
# Deliberately no target_link_libraries: if this ever needs one, the API has
# failed at the thing it exists to do.
add_executable(tls_api tls_api.c)
target_include_directories(tls_api PRIVATE "${CMAKE_SOURCE_DIR}/include")
if(AMIGA_NDK_INCLUDE)
    target_include_directories(tls_api PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
set_target_properties(tls_api PROPERTIES OUTPUT_NAME "tls_api")
if(TARGET tls_library)
    add_dependencies(tls_api tls_library)
endif()
if(TARGET bsdsocket_library)
    add_dependencies(tls_api bsdsocket_library)
endif()
if(AMIGA_SIZE)
    add_custom_command(TARGET tls_api POST_BUILD
        COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:tls_api>"
        COMMENT "tls_api size")
endif()

# Session resumption, cold against warm, against a real public server.
#
#   tests/tls/run-resume.sh -b build/tls
#
# Same rule as tls_api: linked against nothing of ours, because the API is what
# is under test as much as the arithmetic is.
add_executable(tls_resume tls_resume.c)
target_include_directories(tls_resume PRIVATE "${CMAKE_SOURCE_DIR}/include")
if(AMIGA_NDK_INCLUDE)
    target_include_directories(tls_resume PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
set_target_properties(tls_resume PROPERTIES OUTPUT_NAME "tls_resume")
if(TARGET tls_library)
    add_dependencies(tls_resume tls_library)
endif()
if(TARGET bsdsocket_library)
    add_dependencies(tls_resume bsdsocket_library)
endif()
if(AMIGA_SIZE)
    add_custom_command(TARGET tls_resume POST_BUILD
        COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:tls_resume>"
        COMMENT "tls_resume size")
endif()

add_executable(tls_https tls_https.c)
target_link_libraries(tls_https PRIVATE
    aminetxduo_tls aminetxduo_netstack aminetxduo_common netxduo_port)
if(AMIGA_NDK_INCLUDE)
    target_include_directories(tls_https PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
set_target_properties(tls_https PROPERTIES OUTPUT_NAME "tls_https")

# Where tls_https points, as cache options rather than -DCMAKE_C_FLAGS.
# Overriding CMAKE_C_FLAGS on the command line REPLACES the toolchain's
# CMAKE_C_FLAGS_INIT, which is where -m68020 lives, so doing it that way
# silently produced a 68000-codegen build and a profile in which the
# four-mulu.w software multiply was 21.5% of a TLS handshake.
set(AMINETXDUO_TLS_TEST_HOST "" CACHE STRING
    "Host tests/tls/tls_https connects to (empty: the public default)")
set(AMINETXDUO_TLS_TEST_PORT "" CACHE STRING
    "Port tests/tls/tls_https connects to (empty: 443)")
option(AMINETXDUO_TLS_TEST_LOCAL_ROOT
       "Compile in tests/tls/tls_root_local.h instead of ISRG Root X1" OFF)

if(AMINETXDUO_TLS_TEST_HOST)
    target_compile_definitions(tls_https PRIVATE
        "W_HOST=\"${AMINETXDUO_TLS_TEST_HOST}\"")
endif()
if(AMINETXDUO_TLS_TEST_PORT)
    target_compile_definitions(tls_https PRIVATE
        "W_PORT=${AMINETXDUO_TLS_TEST_PORT}")
endif()
if(AMINETXDUO_TLS_TEST_LOCAL_ROOT)
    target_compile_definitions(tls_https PRIVATE W_ROOT_LOCAL)
endif()

# A link map so tools/profiler/profreport.py can recover file-static symbols;
# without it every sample in a static lands on the preceding global.
target_link_options(tls_https PRIVATE -Wl,--gc-sections
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/tls_https.map")

# tls.library as the SERVER, against tls.library as the client, over 127.0.0.1
# in one guest.  The row this closes said the server state machine was
# unexercised; what was unexercised was the door to it -- TLSA_Server,
# tls_server.c and the server branch of tls_conn.c.
#
#   tests/tls/run-tlsloop.sh -b build/cm
#
# Linked against nothing of ours, for tls_api's reason: the published API is
# half of what is under test.
add_executable(tls_loop tls_loop.c)
target_include_directories(tls_loop PRIVATE "${CMAKE_SOURCE_DIR}/include")
if(AMIGA_NDK_INCLUDE)
    target_include_directories(tls_loop PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
set_target_properties(tls_loop PROPERTIES OUTPUT_NAME "TlsLoop")
if(TARGET tls_library)
    add_dependencies(tls_loop tls_library)
endif()
if(TARGET bsdsocket_library)
    add_dependencies(tls_loop bsdsocket_library)
endif()
