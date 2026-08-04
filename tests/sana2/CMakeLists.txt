# The SANA-II shim's arithmetic, on the host.
#
# src/sana2 is 3,721 lines with no suite of its own. What reaches it today is
# incidental: tests/tcpdrill's tapdev.c drives the real copy hooks, but every
# assertion it makes is about TCP frames on the wire, in one device shape, and
# only where an emulator runs.
#
# sana2_copy.c is the part that can be compiled here as it ships -- no Exec
# calls, no NetX Duo objects beyond the three NX_PACKET fields it reads -- and
# it is the part where a mistake is least recoverable: the hooks run at
# interrupt level, called by somebody else's driver, on a machine with no MMU.
#
#   cc -std=c11 -Wall -Wextra "-D__asm(x)=" ... \
#      host/test_sana2_copy_host.c ../../src/sana2/sana2_copy.c -o test_sana2_copy

if(CMAKE_CROSSCOMPILING)
    return()
endif()

add_executable(test_sana2_copy
    host/test_sana2_copy_host.c
    "${CMAKE_SOURCE_DIR}/src/sana2/sana2_copy.c")

# host/shim/host_regargs.h says why, and why it is a forced include rather than
# a -D: it pulls libc in ahead of the macro so the macro cannot reach it.
target_compile_options(test_sana2_copy PRIVATE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/host/shim/host_regargs.h")

# -UAMINETXDUO_IPV6 for the reason tests/netstack/host's targets carry it: it
# changes the layout of NX_PACKET, and the vendored headers here are the linux
# port's.
target_compile_options(test_sana2_copy PRIVATE -UAMINETXDUO_IPV6)

target_include_directories(test_sana2_copy PRIVATE
    "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"      # tx_port.h, first
    "${CMAKE_CURRENT_SOURCE_DIR}/host/shim"         # exec/io.h and friends
    "${CMAKE_SOURCE_DIR}/src/config/test/shim"      # the rest of exec/
    "${CMAKE_SOURCE_DIR}/src/sana2"
    "${CMAKE_SOURCE_DIR}/src/net68k"
    "${CMAKE_SOURCE_DIR}/include"
    "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"     # nx_port.h
    "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"    # nx_user.h -- ours
    "${AMINETXDUO_NETXDUO}/common/inc"
    "${AMINETXDUO_THREADX}/common/inc")

target_compile_definitions(test_sana2_copy PRIVATE
    NX_INCLUDE_USER_DEFINE_FILE
    AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

include(CTest)
add_test(NAME sana2_copy COMMAND test_sana2_copy)
