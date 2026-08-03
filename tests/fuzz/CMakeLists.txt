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

# The DHCP client's option walk. Its parser is static, so fuzz_dhcp.c #includes
# the translation unit rather than linking it -- the .c must NOT be listed here
# as well, or it is compiled twice.
add_executable(fuzz_dhcp
    fuzz_dhcp.c
    fuzz_txstub.c
    "${AMINETXDUO_NETXDUO}/common/src/nxd_udp_socket_send.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_udp_socket_create.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_udp_socket_delete.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_udp_socket_bind.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_udp_socket_unbind.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_udp_socket_receive.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_udp_socket_receive_notify.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_udp_socket_send.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_udp_socket_source_send.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_ip_gateway_address_get.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_ip_gateway_address_clear.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_ip_interface_address_get.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_ip_interface_address_set.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_ip_packet_deferred_receive.c"
    ${_fuzz_nx_src})
target_include_directories(fuzz_dhcp PRIVATE
    "${AMINETXDUO_NETXDUO}/addons/dhcp" ${_fuzz_nx_inc})
# The nxe_/txe_ error-checking wrappers are a second copy of every entry point
# and pull in the whole ThreadX object surface for a driver that starts no
# threads. Disabled here so the client binds straight to the _nx_ functions.
target_compile_definitions(fuzz_dhcp PRIVATE
    NX_DISABLE_ERROR_CHECKING TX_DISABLE_ERROR_CHECKING ${_fuzz_nx_def})
target_compile_options(fuzz_dhcp PRIVATE -UAMINETXDUO_IPV6 ${_fuzz_san})
target_link_options(fuzz_dhcp PRIVATE ${_fuzz_san})

add_executable(fuzz_mdns
    fuzz_mdns.c
    "${AMINETXDUO_NETXDUO}/addons/mdns/nxd_mdns.c"
    ${_fuzz_nx_src})
target_include_directories(fuzz_mdns PRIVATE
    "${AMINETXDUO_NETXDUO}/addons/mdns" ${_fuzz_nx_inc})
target_compile_definitions(fuzz_mdns PRIVATE ${_fuzz_nx_def})
target_compile_options(fuzz_mdns PRIVATE -UAMINETXDUO_IPV6 ${_fuzz_san})
target_link_options(fuzz_mdns PRIVATE ${_fuzz_san})


# ---------------------------------------------------------------------------
# TLS: the record layer, the plaintext handshake, and the X.509 path.
#
# Every byte these parse is a byte the server chose, and all of it is parsed
# before there is any key to authenticate it with -- so "the peer had to
# complete a handshake first" protects none of it. It is also the headline
# feature, which means it is what a hostile host gets pointed at.
#
# The three drivers split the way the code does: fuzz_tls_record stops at the
# handshake message boundary, fuzz_tls_x509 starts at the Certificate message
# and goes down through the DER walk, and fuzz_tls_crypto -- further down, in
# the 32-bit build -- takes the three messages that dispatch through a
# ciphersuite into NX_CRYPTO_METHOD entries. The first two list their nx_secure
# sources explicitly rather than linking the `nx_secure` target -- that target's
# dependency chain reaches aminetxduo_common, which is AmigaOS code and does
# not build on a host.
#
# NX_SECURE_ENABLE_AEAD_CIPHER matches src/tls/CMakeLists.txt, where it is set
# PUBLIC on nx_secure. It changes structure layouts in nx_secure_tls.h, so a
# driver built without it would be fuzzing a different NX_SECURE_TLS_SESSION
# than the one that ships.
#
# NOT COVERED, and why:
#
#   The TLS 1.2 NewSessionTicket. nx_secure only parses one under
#   NX_SECURE_TLS_TLS_1_3_ENABLED, which is off; the TLS 1.2 ticket is read by
#   tls_resume_take_ticket() in src/tlslib/tls_resume.c, which takes a
#   TLSConnection * and so needs tls_internal.h -- the whole AmigaOS library
#   ABI -- on a host. docs/BACKLOG.md has the reasoning and the by-inspection
#   result.
#
#   tls_store.c's issuer-name walk, for the same reason. It calls the same
#   vendored TLV parser fuzz_tls_x509 drives, so the parser underneath it is
#   covered; the four-field walk on top of it is not.
#
#   The record decrypt path. It runs after keys exist, on bytes that have
#   already failed or passed a MAC, and reaching it needs a full key schedule.

set(_fuzz_tls_inc
    ${_fuzz_nx_inc}
    "${AMINETXDUO_NETXDUO}/nx_secure/inc"
    "${AMINETXDUO_NETXDUO}/nx_secure/ports"
    "${AMINETXDUO_NETXDUO}/crypto_libraries/inc")

set(_fuzz_tls_def
    ${_fuzz_nx_def}
    NX_DISABLE_ERROR_CHECKING
    TX_DISABLE_ERROR_CHECKING
    NX_SECURE_ENABLE_AEAD_CIPHER)

set(_fuzz_secure_src "${AMINETXDUO_NETXDUO}/nx_secure/src")

# The record header off the wire, then the handshake messages inside it.
add_executable(fuzz_tls_record
    fuzz_tls_record.c
    "${_fuzz_secure_src}/nx_secure_tls_process_header.c"
    "${_fuzz_secure_src}/nx_secure_tls_process_handshake_header.c"
    "${_fuzz_secure_src}/nx_secure_tls_process_serverhello.c"
    "${_fuzz_secure_src}/nx_secure_tls_process_serverhello_extensions.c"
    "${_fuzz_secure_src}/nx_secure_tls_process_certificate_request.c"
    "${_fuzz_secure_src}/nx_secure_tls_ciphersuite_lookup.c"
    "${_fuzz_secure_src}/nx_secure_tls_check_protocol_version.c"
    "${_fuzz_secure_src}/nx_secure_tls_newest_supported_version.c"
    "${_fuzz_secure_src}/nx_secure_x509_local_device_certificate_get.c"
    "${_fuzz_secure_src}/nx_secure_x509_certificate_list_find.c"
    "${_fuzz_secure_src}/nx_secure_x509_distinguished_name_compare.c"
    "${AMINETXDUO_NETXDUO}/common/src/nx_packet_data_extract_offset.c")
target_include_directories(fuzz_tls_record PRIVATE ${_fuzz_tls_inc})
target_compile_definitions(fuzz_tls_record PRIVATE ${_fuzz_tls_def})
target_compile_options(fuzz_tls_record PRIVATE -UAMINETXDUO_IPV6 ${_fuzz_san})
target_link_options(fuzz_tls_record PRIVATE ${_fuzz_san})

# The Certificate message and everything under it. Both stub files: fuzz_nxstub
# has the mutex the X.509 store takes, fuzz_txstub has the tx_thread_sleep the
# certificate list walk can reach. They define disjoint sets, so linking both
# is not a duplicate.
add_executable(fuzz_tls_x509
    fuzz_tls_x509.c
    fuzz_nxstub.c
    fuzz_txstub.c
    "${_fuzz_secure_src}/nx_secure_x509.c"
    "${_fuzz_secure_src}/nx_secure_x509_asn1_tlv_block_parse.c"
    "${_fuzz_secure_src}/nx_secure_x509_certificate_initialize.c"
    "${_fuzz_secure_src}/nx_secure_x509_certificate_list_add.c"
    "${_fuzz_secure_src}/nx_secure_x509_certificate_list_find.c"
    "${_fuzz_secure_src}/nx_secure_x509_certificate_list_remove.c"
    "${_fuzz_secure_src}/nx_secure_x509_common_name_dns_check.c"
    "${_fuzz_secure_src}/nx_secure_x509_distinguished_name_compare.c"
    "${_fuzz_secure_src}/nx_secure_x509_distinguished_name_parse.c"
    "${_fuzz_secure_src}/nx_secure_x509_ec_private_key_parse.c"
    "${_fuzz_secure_src}/nx_secure_x509_expiration_check.c"
    "${_fuzz_secure_src}/nx_secure_x509_extended_key_usage_extension_parse.c"
    "${_fuzz_secure_src}/nx_secure_x509_extension_find.c"
    "${_fuzz_secure_src}/nx_secure_x509_free_certificate_get.c"
    "${_fuzz_secure_src}/nx_secure_x509_key_usage_extension_parse.c"
    "${_fuzz_secure_src}/nx_secure_x509_oid_parse.c"
    "${_fuzz_secure_src}/nx_secure_x509_pkcs1_rsa_private_key_parse.c"
    "${_fuzz_secure_src}/nx_secure_x509_remote_endpoint_certificate_get.c"
    "${_fuzz_secure_src}/nx_secure_x509_store_certificate_add.c"
    "${_fuzz_secure_src}/nx_secure_x509_store_certificate_remove.c"
    "${_fuzz_secure_src}/nx_secure_x509_subject_alt_names_find.c"
    "${_fuzz_secure_src}/nx_secure_x509_wildcard_compare.c"
    "${_fuzz_secure_src}/nx_secure_tls_process_remote_certificate.c"
    "${_fuzz_secure_src}/nx_secure_tls_remote_certificate_free.c"
    "${_fuzz_secure_src}/nx_secure_tls_remote_certificate_free_all.c"
    "${_fuzz_secure_src}/nx_secure_tls_remote_certificate_verify.c")
# tests/tls is on the path for the seed corpus: the sample leaf and CA from
# tls_test_certs.h and the ISRG Root X1 from tls_root_isrg_x1.h. Real DER, so
# the sweep mutates certificates rather than the author's idea of one.
target_include_directories(fuzz_tls_x509 PRIVATE
    "${CMAKE_SOURCE_DIR}/tests/tls" ${_fuzz_tls_inc})
target_compile_definitions(fuzz_tls_x509 PRIVATE ${_fuzz_tls_def})
target_compile_options(fuzz_tls_x509 PRIVATE -UAMINETXDUO_IPV6 ${_fuzz_san})
target_link_options(fuzz_tls_x509 PRIVATE ${_fuzz_san})

# ---------------------------------------------------------------------------
# TLS: the three handshake messages that dispatch through a ciphersuite into
# the crypto -- ServerKeyExchange, CertificateVerify and Finished.
#
# 32-BIT ONLY, and not for fuzz_mdns's reason. ami_tls_crypto.c checks metadata
# alignment with ((ULONG)crypto_metadata & 3), and
# nx_secure_tls_process_certificate_verify.c bounds its signature with
# ((ULONG)packet_buffer + message_length) < ((ULONG)received_signature +
# length). ULONG is the target's 32 bits everywhere in this tree, so on an LP64
# host both of those truncate a pointer -- and the second one is the bounds
# check under test. tools/ci.sh's host32 stage is where this builds and runs;
# `cmake -DCMAKE_C_FLAGS=-m32` by hand does the same.
#
# The nx_secure and nx_crypto halves are static libraries rather than an
# explicit source list. The two 64-bit drivers can list their files because
# their call graph is a dozen parsers; this one reaches session_create, the
# X.509 store, the ECC curve table, the PRF and the whole huge-number layer,
# and listing that would be a copy of both directories that goes stale on the
# next submodule bump. A static library lets the linker pull what is reached
# and nothing else, which is the same end state.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)

    file(GLOB _fuzz_secure_all "${_fuzz_secure_src}/*.c")
    list(FILTER _fuzz_secure_all EXCLUDE REGEX "self_test")
    list(FILTER _fuzz_secure_all EXCLUDE REGEX "/nx_secure_dtls")
    # src/tls/CMakeLists.txt swaps these two for the rfc7905 copies, because
    # ChaCha20-Poly1305 record framing cannot be supplied by an
    # NX_CRYPTO_METHOD. Same swap here, so the driver links the objects that
    # ship.
    list(FILTER _fuzz_secure_all EXCLUDE REGEX
         "/nx_secure_tls_record_payload_(en|de)crypt\\.c$")

    add_library(fuzz_tls_nx_secure STATIC
        ${_fuzz_secure_all}
        "${CMAKE_SOURCE_DIR}/src/tls/rfc7905/nx_secure_tls_record_payload_encrypt.c"
        "${CMAKE_SOURCE_DIR}/src/tls/rfc7905/nx_secure_tls_record_payload_decrypt.c")

    file(GLOB _fuzz_crypto_all "${AMINETXDUO_NETXDUO}/crypto_libraries/src/*.c")
    list(FILTER _fuzz_crypto_all EXCLUDE REGEX "self_test")
    add_library(fuzz_tls_nx_crypto STATIC ${_fuzz_crypto_all})

    # The NetX Duo surface underneath, without fuzz_nxstub.c -- the executable
    # supplies that, and a stub inside an archive would be pulled or not
    # depending on link order.
    set(_fuzz_tls_netx_src ${_fuzz_nx_src})
    list(REMOVE_ITEM _fuzz_tls_netx_src
         "${CMAKE_CURRENT_SOURCE_DIR}/fuzz_nxstub.c")
    add_library(fuzz_tls_netx STATIC ${_fuzz_tls_netx_src})

    # src/crypto68k, compiled in rather than linked from the `crypto68k`
    # target: that target's dependency chain reaches aminetxduo_common and
    # therefore the NDK. Same pattern as tests/crypto68k's host tier. The
    # portable C only -- the 68020 assembly cannot be assembled here.
    set(_fuzz_c68k_src
        "${CMAKE_SOURCE_DIR}/src/crypto68k/c68k_prim.c"
        "${CMAKE_SOURCE_DIR}/src/crypto68k/c68k_div.c"
        "${CMAKE_SOURCE_DIR}/src/crypto68k/c68k_mont.c"
        "${CMAKE_SOURCE_DIR}/src/crypto68k/c68k_powm.c"
        "${CMAKE_SOURCE_DIR}/src/crypto68k/c68k_crt.c"
        "${CMAKE_SOURCE_DIR}/src/crypto68k/c68k_p256.c"
        "${CMAKE_SOURCE_DIR}/src/crypto68k/c68k_p256_table.c"
        "${CMAKE_SOURCE_DIR}/src/crypto68k/c68k_aes.c"
        "${CMAKE_SOURCE_DIR}/src/crypto68k/c68k_sha256.c"
        "${CMAKE_SOURCE_DIR}/src/crypto68k/c68k_chacha20.c"
        "${CMAKE_SOURCE_DIR}/src/crypto68k/c68k_poly1305.c")

    # The Amiga half: the crypto tables and the module behind them. Its own
    # archive so that the warning set below applies to the driver and not to
    # code the cross build already compiles under the project's.
    add_library(fuzz_tls_ami STATIC
        "${CMAKE_SOURCE_DIR}/src/tls/ami_tls_crypto.c"
        ${_fuzz_c68k_src})

    add_executable(fuzz_tls_crypto
        fuzz_tls_crypto.c
        fuzz_nxstub.c
        fuzz_txstub.c)

    # src/config/test/shim carries the minimal <exec/types.h> that src/tls/tls.h
    # needs; tests/tls carries the sample certificate and its private key.
    set(_fuzz_tls_crypto_inc
        ${_fuzz_tls_inc}
        "${CMAKE_SOURCE_DIR}/src/config/test/shim"
        "${CMAKE_SOURCE_DIR}/src/tls"
        "${CMAKE_SOURCE_DIR}/src/crypto68k"
        "${CMAKE_SOURCE_DIR}/tests/tls")

    foreach(_t fuzz_tls_nx_secure fuzz_tls_nx_crypto fuzz_tls_netx
               fuzz_tls_ami fuzz_tls_crypto)
        target_include_directories(${_t} PRIVATE ${_fuzz_tls_crypto_inc})
        target_compile_definitions(${_t} PRIVATE ${_fuzz_tls_def})
        target_compile_options(${_t} PRIVATE -UAMINETXDUO_IPV6 ${_fuzz_san})
    endforeach()

    target_compile_options(fuzz_tls_crypto PRIVATE -Wall -Wextra)
    target_link_options(fuzz_tls_crypto PRIVATE ${_fuzz_san})
    # Repeated because the archives reference each other's objects and a single
    # pass over each resolves only one direction.
    target_link_libraries(fuzz_tls_crypto PRIVATE
        fuzz_tls_ami fuzz_tls_nx_secure fuzz_tls_nx_crypto fuzz_tls_netx
        fuzz_tls_ami fuzz_tls_nx_secure fuzz_tls_nx_crypto fuzz_tls_netx)

endif()

include(CTest)

# The known case, first and always. If this ever passes silently the whole
# point of the file has been lost.
add_test(NAME fuzz_netdb_regression
         COMMAND ${CMAKE_COMMAND}
                 -DDRIVER=$<TARGET_FILE:fuzz_config>
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/run-regression.cmake")

add_test(NAME fuzz_config_sweep
         COMMAND fuzz_config -r 1 ${AMINETXDUO_FUZZ_CASES})

# DEVS:Internet/service_discovery, alone and on its own seed. It is in the
# sweep above as well; this one exists so that a failure names the parser, and
# so that the service-discovery grammar gets its own run of cases rather than
# sharing them with four netdb files.
add_test(NAME fuzz_dnssd_sweep
         COMMAND fuzz_config -s -r 3 ${AMINETXDUO_FUZZ_CASES})
add_test(NAME fuzz_dns_sweep
         COMMAND fuzz_dns -r 1 ${AMINETXDUO_FUZZ_CASES})

add_test(NAME fuzz_bpf_sweep
         COMMAND fuzz_bpf -r 1 ${AMINETXDUO_FUZZ_CASES})

add_test(NAME fuzz_dhcp_seeds
         COMMAND fuzz_dhcp -s)
add_test(NAME fuzz_dhcp_sweep
         COMMAND fuzz_dhcp -r 1 ${AMINETXDUO_FUZZ_CASES})

add_test(NAME fuzz_tls_record_seeds
         COMMAND fuzz_tls_record -s)
add_test(NAME fuzz_tls_record_sweep
         COMMAND fuzz_tls_record -r 1 ${AMINETXDUO_FUZZ_CASES})

add_test(NAME fuzz_tls_x509_seeds
         COMMAND fuzz_tls_x509 -s)
add_test(NAME fuzz_tls_x509_sweep
         COMMAND fuzz_tls_x509 -r 1 ${AMINETXDUO_FUZZ_CASES})

# fuzz_mdns only runs in a 32-bit build. NetX Duo's mDNS cache stores pointers
# in single ULONG slots, so it is coherent only where sizeof(void*) == 4; on a
# 64-bit host it spins inside nx_mdns_enable() before a datagram arrives.
# tools/ci.sh host32 configures that build; `cmake -DCMAKE_C_FLAGS=-m32` by
# hand does the same.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_test(NAME fuzz_mdns_seeds
             COMMAND fuzz_mdns -s)
    add_test(NAME fuzz_mdns_sweep
             COMMAND fuzz_mdns -r 1 ${AMINETXDUO_FUZZ_CASES})
    set_tests_properties(fuzz_mdns_sweep PROPERTIES TIMEOUT 1800)

    # The TLS crypto driver is in the same build for the reason above the
    # target, not because it has anything to do with mDNS. Every case here
    # rebuilds a session, parses a certificate and can run an RSA public
    # operation and a P-256 scalar multiplication, so it is the slowest case in
    # the set at roughly 5 ms -- AMINETXDUO_FUZZ_CASES all the same, because a
    # second knob is a second thing to remember to raise for a soak.
    add_test(NAME fuzz_tls_crypto_seeds
             COMMAND fuzz_tls_crypto -s)
    add_test(NAME fuzz_tls_crypto_sweep
             COMMAND fuzz_tls_crypto -r 1 ${AMINETXDUO_FUZZ_CASES})
    set_tests_properties(fuzz_tls_crypto_sweep PROPERTIES TIMEOUT 1800)
endif()

# A soak set by AMINETXDUO_FUZZ_CASES needs longer than ctest's default.
set_tests_properties(fuzz_config_sweep fuzz_dnssd_sweep fuzz_dns_sweep
                     fuzz_bpf_sweep fuzz_dhcp_sweep
                     fuzz_tls_record_sweep fuzz_tls_x509_sweep
                     PROPERTIES TIMEOUT 1800)
