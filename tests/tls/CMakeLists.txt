# The nx_secure feasibility gate (docs/RESEARCH.md 9, decision 4).
#
#   cmake -S . -B build/cm -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
#         -DCMAKE_BUILD_TYPE=Release -DAMINETXDUO_TLS=ON
#   cmake --build build/cm --parallel --target tls_bench
#   AMINETXDUO_RUN_TAG=tls20 ./tools/fsuae-run.sh        -t 300 build/cm/tests/tls/tls_bench
#   AMINETXDUO_RUN_TAG=tls30 ./tools/fsuae-run.sh -c 68030 -t 300 build/cm/tests/tls/tls_bench

add_executable(tls_bench tls_bench.c)

# aminetxduo_common is here for the crash guard only; the benchmark deliberately
# does not start ThreadX (see the header comment in tls_bench.c).
target_link_libraries(tls_bench PRIVATE aminetxduo_tls aminetxduo_common netxduo_port)

if(AMIGA_NDK_INCLUDE)
    target_include_directories(tls_bench PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

set_target_properties(tls_bench PROPERTIES OUTPUT_NAME "tls_bench")

if(AMIGA_SIZE)
    add_custom_command(TARGET tls_bench POST_BUILD
        COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:tls_bench>"
        COMMENT "tls_bench size")
endif()

# A real TLS 1.2 handshake, client and server both on the Amiga, over the
# in-tree RAM driver.  Needs ThreadX and NetX Duo up, unlike tls_bench.
add_executable(tls_handshake tls_handshake.c)

target_link_libraries(tls_handshake PRIVATE
    aminetxduo_tls aminetxduo_common netxduo_port)

if(AMIGA_NDK_INCLUDE)
    target_include_directories(tls_handshake PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()

set_target_properties(tls_handshake PROPERTIES OUTPUT_NAME "tls_handshake")

if(AMIGA_SIZE)
    add_custom_command(TARGET tls_handshake POST_BUILD
        COMMAND "${AMIGA_SIZE}" "$<TARGET_FILE:tls_handshake>"
        COMMENT "tls_handshake size")
endif()
