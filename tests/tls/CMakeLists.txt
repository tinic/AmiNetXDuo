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
#
# Three executables from one source; see the header comment in tls_handshake.c.
# They are split rather than looped in one binary because their run times differ
# by an order of magnitude, and a correctness regression should be catchable in
# a short run:
#
#   AMINETXDUO_RUN_TAG=hs   ./tools/fsuae-run.sh -t 300 build/tls/tests/tls/tls_handshake
#   AMINETXDUO_RUN_TAG=hsd  ./tools/fsuae-run.sh -t 900 build/tls/tests/tls/tls_decompose
#   AMINETXDUO_RUN_TAG=hsi  ./tools/fsuae-run.sh -t 900 build/tls/tests/tls/tls_interop
foreach(prog tls_handshake tls_decompose tls_interop)
    add_executable(${prog} tls_handshake.c)

    target_link_libraries(${prog} PRIVATE
        aminetxduo_tls aminetxduo_common netxduo_port)

    if(AMIGA_NDK_INCLUDE)
        target_include_directories(${prog} PRIVATE "${AMIGA_NDK_INCLUDE}")
    endif()

    set_target_properties(${prog} PROPERTIES OUTPUT_NAME "${prog}")

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
add_executable(tls_https tls_https.c)
target_link_libraries(tls_https PRIVATE
    aminetxduo_tls aminetxduo_netstack aminetxduo_common netxduo_port)
if(AMIGA_NDK_INCLUDE)
    target_include_directories(tls_https PRIVATE "${AMIGA_NDK_INCLUDE}")
endif()
set_target_properties(tls_https PROPERTIES OUTPUT_NAME "tls_https")
