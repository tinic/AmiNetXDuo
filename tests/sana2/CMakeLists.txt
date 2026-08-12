# The SANA-II shim's arithmetic, on the host.
#
# src/sana2 is 3,721 lines with no suite of its own. What reaches it today is
# incidental: tests/tcpdrill's tapdev.c drives the real copy hooks, but every
# assertion it makes is about TCP frames on the wire, in one device shape, and
# only where an emulator runs.
#
# sana2_copy.c is the part that can be compiled here as it ships, no Exec
# calls, no NetX Duo objects beyond the three NX_PACKET fields it reads, and
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
    "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"    # nx_user.h, ours
    "${AMINETXDUO_NETXDUO}/common/inc"
    "${AMINETXDUO_THREADX}/common/inc")

target_compile_definitions(test_sana2_copy PRIVATE
    NX_INCLUDE_USER_DEFINE_FILE
    AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

include(CTest)
add_test(NAME sana2_copy COMMAND test_sana2_copy)

# The transmit path's framing, sana2_tx.c beside the copy hook it feeds.  The
# exec calls it makes are the shim in host/shim/proto/exec.h and are defined
# by the test itself, so what runs is the real ami_sana2_tx_send(),
# ami_sana2_tx_reap() and ami_sana2_copy_from_buff().
add_executable(test_sana2_tx
    host/test_sana2_tx_host.c
    "${CMAKE_SOURCE_DIR}/src/sana2/sana2_tx.c"
    "${CMAKE_SOURCE_DIR}/src/sana2/sana2_copy.c")

target_compile_options(test_sana2_tx PRIVATE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/host/shim/host_regargs.h")
# -UAMINETXDUO_IPV6 for the reason the target above carries it, and
# -UAMINETXDUO_BPF because bpf.h asserts a 68k structure layout that a host
# compiler does not produce.  The tap is not what this tests.
target_compile_options(test_sana2_tx PRIVATE -UAMINETXDUO_IPV6 -UAMINETXDUO_BPF)

target_include_directories(test_sana2_tx PRIVATE
    "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"
    "${CMAKE_CURRENT_SOURCE_DIR}/host/shim"
    "${CMAKE_SOURCE_DIR}/src/config/test/shim"
    "${CMAKE_SOURCE_DIR}/src/sana2"
    "${CMAKE_SOURCE_DIR}/src/net68k"
    "${CMAKE_SOURCE_DIR}/include"
    "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"
    "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"
    "${AMINETXDUO_NETXDUO}/common/inc"
    "${AMINETXDUO_THREADX}/common/inc")

target_compile_definitions(test_sana2_tx PRIVATE
    NX_INCLUDE_USER_DEFINE_FILE
    AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

add_test(NAME sana2_tx COMMAND test_sana2_tx)

# The receive path's delivery, sana2_rx.c compiled whole.
#
# ami_sana2_rx_deliver() is the extern; everything else in that file is the
# reader task, its Exec port and its ThreadX plumbing, and none of it is
# called here.  The whole translation unit is compiled anyway rather than a
# copy of the function, so what runs is the object that ships -- which is also
# why the stub list in the test is as long as it is.
#
# The two -Wno-*-cast are the pointer <-> ULONG round trip in
# tx_thread_create()'s entry_input argument.  ULONG and a pointer are the same
# width on m68k and are not on an LP64 host; the reader task is not reached
# from here, so the truncation the host warns about cannot happen in this
# binary.  Same reason tests/x509 confines its ECDSA section to a 32-bit build.
add_executable(test_sana2_rx
    host/test_sana2_rx_host.c
    "${CMAKE_SOURCE_DIR}/src/sana2/sana2_rx.c")

target_compile_options(test_sana2_rx PRIVATE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/host/shim/host_regargs.h")
target_compile_options(test_sana2_rx PRIVATE
    -UAMINETXDUO_IPV6 -UAMINETXDUO_BPF
    -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast)

target_include_directories(test_sana2_rx PRIVATE
    "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"
    "${CMAKE_CURRENT_SOURCE_DIR}/host/shim"
    "${CMAKE_SOURCE_DIR}/src/config/test/shim"
    "${CMAKE_SOURCE_DIR}/src/sana2"
    "${CMAKE_SOURCE_DIR}/src/net68k"
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/port/threadx-amiga/inc"     # tx_amiga.h
    "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"
    "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"
    "${AMINETXDUO_NETXDUO}/common/inc"
    "${AMINETXDUO_THREADX}/common/inc")

target_compile_definitions(test_sana2_rx PRIVATE
    NX_INCLUDE_USER_DEFINE_FILE
    AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

# The default build has it on (see the option in the top-level CMakeLists), and
# it changes the layout of AmiRxSlot, so the test has to agree with the library
# about which one it is compiling.  The checksum-offload section of the test is
# behind the same #ifdef.
if(AMINETXDUO_RX_VERIFY)
    target_compile_definitions(test_sana2_rx PRIVATE AMINETXDUO_RX_VERIFY)
endif()

add_test(NAME sana2_rx COMMAND test_sana2_rx)

# The NX_IP driver entry, sana2_driver.c.
#
# Every NX_LINK_* command NetX Duo can issue, the binding table that resolves
# each one to an interface, and the EtherType the send path is framed with.
# sana2_copy.c comes along for ami_sana2_copy_bytes(), which
# NX_LINK_SET_PHYSICAL_ADDRESS uses; the six SANA-II entry points and the four
# nx_ip_interface_* setters are defined by the test.
add_executable(test_sana2_driver
    host/test_sana2_driver_host.c
    "${CMAKE_SOURCE_DIR}/src/sana2/sana2_driver.c"
    "${CMAKE_SOURCE_DIR}/src/sana2/sana2_copy.c")

target_compile_options(test_sana2_driver PRIVATE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/host/shim/host_regargs.h")
target_compile_options(test_sana2_driver PRIVATE
    -UAMINETXDUO_IPV6 -UAMINETXDUO_BPF)

target_include_directories(test_sana2_driver PRIVATE
    "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"
    "${CMAKE_CURRENT_SOURCE_DIR}/host/shim"
    "${CMAKE_SOURCE_DIR}/src/config/test/shim"
    "${CMAKE_SOURCE_DIR}/src/sana2"
    "${CMAKE_SOURCE_DIR}/src/net68k"
    "${CMAKE_SOURCE_DIR}/include"
    "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"
    "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"
    "${AMINETXDUO_NETXDUO}/common/inc"
    "${AMINETXDUO_THREADX}/common/inc")

target_compile_definitions(test_sana2_driver PRIVATE
    NX_INCLUDE_USER_DEFINE_FILE
    AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

# NX_LINK_INITIALIZE advertises the checksum offload only in this build, so the
# test has to know which one it is compiling, the same as test_sana2_rx.
if(AMINETXDUO_RX_VERIFY)
    target_compile_definitions(test_sana2_driver PRIVATE AMINETXDUO_RX_VERIFY)
endif()

add_test(NAME sana2_driver COMMAND test_sana2_driver)
