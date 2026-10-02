# Host-only bounded fault-injection test for the TLS 1.3 handshake send
# builders (N-107 server / N-109 client).
#
# tests/x509 answers "does the verifier reject what it must and accept what it
# must". This answers the handshake half of the same question for the builder
# failure paths: when _nx_secure_tls_send_certificate/_certificate_verify/
# _finished/_certificate_request fails, the driver must (a) return that status
# unchanged, (b) release the packet it allocated exactly once, and (c) not send
# a record for the failed builder. It also asserts that a record-send failure
# is NOT double-freed (the record layer already owns that release).
#
# Unlike tests/x509, this does NOT glob the nx_secure/nx_crypto trees into
# archives. It compiles ONLY the two drivers under test
# (_nx_secure_tls_1_3_client_handshake, _nx_secure_tls_1_3_server_handshake)
# and links them against a hand-written stub surface for every other symbol
# they reference -- no NX_PACKET pools, no record layer, no ThreadX scheduler,
# no broad netstack/guest. That is the point: each builder failure can be
# injected and its release contract observed directly.
#
# Base vs fix: on the fixed drivers (bda9d37d) all 9 checks pass. On the
# unfixed drivers (0fb76b63) the three client builders leak the packet (one
# check each) and the four server builders drop the status, skip the release
# and still send the record (three checks each) -- 15 failures. With the
# shipping flags above, probing the unfixed pin hangs instead: the unfixed
# server send_certificate_request guards its builder with
# NX_ASSERT(status == NX_SUCCESS), and NX_ASSERT is gated on NX_DISABLE_ASSERT,
# not NX_DISABLE_ERROR_CHECKING, so the assert is live and spins forever --
# the DoS the fix removes. Run the unfixed pin under a timeout, or with
# -DNX_DISABLE_ASSERT for the deterministic 15-failure count.
#
# HOST ONLY: the stubs are native host C; nothing here runs on m68k.

if(CMAKE_CROSSCOMPILING)
    return()
endif()

set(_hs_src "${AMINETXDUO_NETXDUO}/nx_secure/src")

add_library(test_tls13_hs_stubs STATIC
    nx_secure_tls_1_3_handshake_fault_stubs.c)
add_library(test_tls13_hs_stubs_shipping STATIC
    nx_secure_tls_1_3_handshake_fault_stubs.c)

# Identical define/port surface to src/tls/CMakeLists.txt + tests/x509, so the
# shipping arm omits the optional server client-certificate feature. The
# separate optional arm enables it to reach send_certificate_request; compile
# its stubs and drivers together so the session layouts cannot be mixed.
foreach(lib test_tls13_hs_stubs test_tls13_hs_stubs_shipping)
    target_include_directories(${lib} PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"      # tx_port.h, first
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"     # nx_port.h
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"    # nx_user.h, ours
        "${AMINETXDUO_NETXDUO}/common/inc"
        "${AMINETXDUO_THREADX}/common/inc"
        "${AMINETXDUO_NETXDUO}/nx_secure/inc"
        "${AMINETXDUO_NETXDUO}/nx_secure/ports"
        "${AMINETXDUO_NETXDUO}/crypto_libraries/inc")

    target_compile_definitions(${lib} PUBLIC
        NX_PACKET_ALIGNMENT=8
        NX_INCLUDE_USER_DEFINE_FILE
        AMINETXDUO_HOST_VENDORED_TX_PORT="${AMINETXDUO_THREADX}/ports/linux/gnu/inc/tx_port.h"
        NX_DISABLE_ERROR_CHECKING
        TX_DISABLE_ERROR_CHECKING
        NX_SECURE_KEY_CLEAR
        NX_SECURE_ENABLE_ECC_CIPHERSUITE
        NX_SECURE_ENABLE_AEAD_CIPHER
        NX_SECURE_TLS_ENABLE_TLS_1_3)
endforeach()
target_compile_definitions(test_tls13_hs_stubs PUBLIC
    NX_SECURE_ENABLE_CLIENT_CERTIFICATE_VERIFY)

# The two drivers under test are compiled as PART of the executable so their
# failure paths are exercised with the exact same flags as the stubs.
add_executable(test_tls13_handshake_fault
    nx_secure_tls_1_3_handshake_fault_test.c
    "${_hs_src}/nx_secure_tls_1_3_client_handshake.c"
    "${_hs_src}/nx_secure_tls_1_3_server_handshake.c")

target_link_libraries(test_tls13_handshake_fault PRIVATE test_tls13_hs_stubs)

target_compile_options(test_tls13_handshake_fault PRIVATE -Wall -Wextra -UAMINETXDUO_IPV6)

include(CTest)
add_test(NAME tls13_handshake_fault COMMAND test_tls13_handshake_fault)

add_executable(test_tls13_handshake_fault_shipping
    nx_secure_tls_1_3_handshake_fault_test.c
    "${_hs_src}/nx_secure_tls_1_3_client_handshake.c"
    "${_hs_src}/nx_secure_tls_1_3_server_handshake.c")
target_link_libraries(test_tls13_handshake_fault_shipping PRIVATE
    test_tls13_hs_stubs_shipping)
target_compile_options(test_tls13_handshake_fault_shipping PRIVATE
    -Wall -Wextra -UAMINETXDUO_IPV6)
add_test(NAME tls13_handshake_fault_shipping
    COMMAND test_tls13_handshake_fault_shipping)
