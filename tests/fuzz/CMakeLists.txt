# Sanitizer fuzz drivers for the parsers that read bytes we did not write.
#
# WHY THIS EXISTS AT ALL, AND WHY IT IS HOST-ONLY
#
# The target has no MMU and cannot run Enforcer, so a stray write does not
# fault -- it corrupts another task and surfaces later as something unrelated.
# There is no crash to bisect and no sanitizer to run there.
#
# The parsers, though, are portable C. They can be compiled for the host under
# ASan and UBSan and driven with malformed input, which is exactly the tooling
# the Amiga cannot provide. That is how the netdb alias-pool overflow was
# found: the sizing pass counted a run of non-whitespace as one token while the
# parse pass saw one token per "" pair, so `1.2.3.4 h """"..."""` wrote 29
# pointers past a 48-byte block -- silently, on every library open, from a file
# in DEVS:.
#
# THE REGRESSION CASE RUNS EVERY TIME. The random sweep is bounded so `ctest`
# stays quick; AMINETXDUO_FUZZ_CASES raises it for a real soak:
#
#     AMINETXDUO_FUZZ_CASES=200000 ctest -R fuzz
#
# Sanitizers are probed rather than assumed -- a host toolchain without them
# still configures, and says so, rather than failing the build.

if(CMAKE_CROSSCOMPILING)
    return()
endif()

include(CheckCSourceCompiles)

set(_fuzz_san -fsanitize=address,undefined -fno-sanitize-recover=all
              -fno-omit-frame-pointer)

set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
check_c_source_compiles("int main(void){return 0;}" AMINETXDUO_HAVE_SANITIZERS)
unset(CMAKE_REQUIRED_FLAGS)

if(NOT AMINETXDUO_HAVE_SANITIZERS)
    message(STATUS "fuzz: no ASan/UBSan in this host compiler -- drivers skipped")
    return()
endif()

# How many random cases per driver under a plain `ctest`. Small on purpose:
# this is a regression gate, not the soak. The soak is the env var above.
set(AMINETXDUO_FUZZ_CASES "2000" CACHE STRING
    "Random cases per fuzz driver under ctest")

# The config parsers: DEVS:Internet/{hosts,networks,protocols,services} and
# DEVS:NetInterfaces/*.
add_executable(fuzz_config
    fuzz_config.c
    "${CMAKE_SOURCE_DIR}/src/config/config_text.c"
    "${CMAKE_SOURCE_DIR}/src/config/config_text6.c"
    "${CMAKE_SOURCE_DIR}/src/config/config_parse.c"
    "${CMAKE_SOURCE_DIR}/src/config/netdb.c")

target_include_directories(fuzz_config PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src/config"
    "${CMAKE_SOURCE_DIR}/src/config/test/shim")

target_compile_options(fuzz_config PRIVATE -Wall -Wextra ${_fuzz_san})
target_link_options(fuzz_config PRIVATE ${_fuzz_san})

# The BPF filter VM, which runs programs a caller supplies over packets the
# wire supplies. The driver deliberately runs UNVALIDATED programs, and
# wirelen > caplen, and short buffers -- the interpreter claims to be total and
# this is what tests that claim rather than repeating it.
add_executable(fuzz_bpf
    fuzz_bpf.c
    "${CMAKE_SOURCE_DIR}/src/bpf/bpf_filter.c"
    "${CMAKE_SOURCE_DIR}/src/bpf/bpf_validate.c"
    "${CMAKE_SOURCE_DIR}/src/bpf/bpf_channel.c"
    "${CMAKE_SOURCE_DIR}/src/bpf/bpf_tap.c")

target_include_directories(fuzz_bpf PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src/bpf"
    "${CMAKE_SOURCE_DIR}/src/config/test/shim")

target_compile_definitions(fuzz_bpf PRIVATE AMI_BPF_REPLICA)
target_compile_options(fuzz_bpf PRIVATE -Wall -Wextra ${_fuzz_san})
target_link_options(fuzz_bpf PRIVATE ${_fuzz_san})


# ---------------------------------------------------------------------------
# The two parsers that read bytes off the network from whoever sent them.
#
# DNS responses arrive from whatever answered; mDNS is unauthenticated
# multicast, so any host on the segment can send one unprompted. Both walk
# name-compression pointers, which is where DNS parsers historically break --
# a pointer to itself, two that cycle, one past the end of the datagram.
#
# These drive the real entry points through a real NX_IP with a non-echoing
# driver, not a reimplementation of the parser: the assertion is "the stack
# survived this datagram", which is only worth something if the datagram took
# the path a real one takes. tests/perf/host/shim's tx_port.h gives a 32-bit
# ULONG so the length and TTL arithmetic is the m68k's; pointers stay 64-bit,
# so a name walk that would wrap a 32-bit pointer cannot be reached here.
# ---------------------------------------------------------------------------

set(_fuzz_nx_src
    "${AMINETXDUO_NETXDUO}/common/src/nx_ip_driver_packet_send.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_ip_packet_send.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_ip_header_add.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_ip_route_find.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_ip_checksum_compute.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_ip_packet_checksum_compute.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_ipv4_packet_receive.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_ipv4_option_process.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_ip_dispatch_process.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_icmpv4_send_error_message.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_igmp_multicast_check.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_packet_pool_initialize.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_udp_enable.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_udp_bind_cleanup.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_udp_free_port_find.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_udp_packet_receive.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_udp_receive_cleanup.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_packet_pool_create.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_packet_pool_cleanup.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_packet_allocate.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_packet_copy.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_packet_data_append.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_packet_data_extract_offset.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_packet_length_get.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_packet_release.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_packet_transmit_release.c"
    "${AMINETXDUO_NETXDUO}/common/src/nxe_packet_pool_create.c"
    "${AMINETXDUO_NETXDUO}/common/src/nxe_packet_allocate.c"
    "${AMINETXDUO_NETXDUO}/common/src/nxe_packet_data_append.c"
    "${AMINETXDUO_NETXDUO}/common/src/nxe_packet_length_get.c"
    "${AMINETXDUO_NETXDUO}/common/src/nxe_packet_release.c"
    "${AMINETXDUO_NETXDUO}/common/src/nxe_udp_enable.c"
    "${AMINETXDUO_NETXDUO}/common/src/nxe_udp_socket_create.c"
    "${AMINETXDUO_NETXDUO}/common/src/nxe_udp_socket_bind.c"
    "${AMINETXDUO_NETXDUO}/common/src/nxe_udp_socket_send.c"
    "${AMINETXDUO_NETXDUO}/common/src/nxe_udp_socket_receive.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_packet_pool_delete.c"
    "${AMINETXDUO_NETXDUO}/common/src/nxe_packet_pool_delete.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_utility.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fuzz_nxstub.c")

set(_fuzz_nx_inc
    "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"          # tx_port.h, first
    "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"         # nx_port.h
    "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"        # nx_user.h -- ours
    "${AMINETXDUO_NETXDUO}/common/inc"
    "${AMINETXDUO_THREADX}/common/inc")

# NX_PACKET_ALIGNMENT defaults to sizeof(ULONG), and the shim makes ULONG the
# target's 32 bits -- so the pool would align packets to 4 while this host's
# NX_PACKET holds 64-bit pointers and needs 8. Misaligned on every second
# packet, before any datagram is parsed. On m68k, 4 is right and correct.
set(_fuzz_nx_def
    NX_PACKET_ALIGNMENT=8
    NX_INCLUDE_USER_DEFINE_FILE
    AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h")

# nx_udp_socket_send is here and not in _fuzz_nx_src because fuzz_mdns
# supplies its own; the two drivers intercept the wire at different depths.
add_executable(fuzz_dns
    fuzz_dns.c
    "${AMINETXDUO_NETXDUO}/addons/dns/nxd_dns.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_udp_socket_send.c"
    ${_fuzz_nx_src})
target_include_directories(fuzz_dns PRIVATE
    "${AMINETXDUO_NETXDUO}/addons/dns" ${_fuzz_nx_inc})
target_compile_definitions(fuzz_dns PRIVATE FUZZ_HAS_EVENT_FLAGS ${_fuzz_nx_def})
target_compile_options(fuzz_dns PRIVATE -UAMINETXDUO_IPV6 ${_fuzz_san})
target_link_options(fuzz_dns PRIVATE ${_fuzz_san})

add_executable(fuzz_mdns
    fuzz_mdns.c
    "${AMINETXDUO_NETXDUO}/addons/mdns/nxd_mdns.c"
    ${_fuzz_nx_src})
target_include_directories(fuzz_mdns PRIVATE
    "${AMINETXDUO_NETXDUO}/addons/mdns" ${_fuzz_nx_inc})
target_compile_definitions(fuzz_mdns PRIVATE ${_fuzz_nx_def})
target_compile_options(fuzz_mdns PRIVATE -UAMINETXDUO_IPV6 ${_fuzz_san})
target_link_options(fuzz_mdns PRIVATE ${_fuzz_san})

include(CTest)

# The known case, first and always. If this ever passes silently the whole
# point of the file has been lost.
add_test(NAME fuzz_netdb_regression
         COMMAND ${CMAKE_COMMAND}
                 -DDRIVER=$<TARGET_FILE:fuzz_config>
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/run-regression.cmake")

add_test(NAME fuzz_config_sweep
         COMMAND fuzz_config -r 1 ${AMINETXDUO_FUZZ_CASES})
# fuzz_mdns is built but deliberately NOT registered: NetX Duo's mDNS cache
# stores pointers in single ULONG slots, so it is only coherent when
# sizeof(void*) == sizeof(ULONG) == 4 and it spins inside nx_mdns_enable() on
# a 64-bit host. Register it when there is a -m32 build to run it in.
add_test(NAME fuzz_dns_sweep
         COMMAND fuzz_dns -r 1 ${AMINETXDUO_FUZZ_CASES})

add_test(NAME fuzz_bpf_sweep
         COMMAND fuzz_bpf -r 1 ${AMINETXDUO_FUZZ_CASES})

# A soak set by AMINETXDUO_FUZZ_CASES needs longer than ctest's default.
set_tests_properties(fuzz_config_sweep fuzz_bpf_sweep PROPERTIES TIMEOUT 1800)
