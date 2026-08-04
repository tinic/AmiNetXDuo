# Host unit tests for the certificate and signature checks.
#
# tests/fuzz answers "does this parser stay inside its buffer". This answers
# "does it reject what it must reject, and still accept what it must accept" --
# which a sanitizer cannot see, since a verifier that returns success on
# everything is memory-safe.
#
# HOST ONLY, for the same reason tests/fuzz is: the point is to run it on every
# build, and the m68k binaries only run under an emulator or on hardware.
#
# The nx_secure and nx_crypto halves are globbed into static libraries rather
# than listed as sources. The call graph reaches the X.509 store, the RSA and
# SHA-256 methods, the ECC curve table and the whole huge-number layer; listing
# that would be a copy of both directories that goes stale on the next
# submodule bump. An archive also keeps the record and session objects out --
# they reach NX_PACKET and the TCP socket, and nothing here does.
#
# The two stub files come from tests/fuzz: fuzz_nxstub.c has the mutex the
# store takes, fuzz_txstub.c the tx_thread_sleep the certificate list walk can
# reach. They define disjoint sets, so linking both is not a duplicate.

if(CMAKE_CROSSCOMPILING)
    return()
endif()

set(_x509_secure_src "${AMINETXDUO_NETXDUO}/nx_secure/src")
set(_x509_crypto_src "${AMINETXDUO_NETXDUO}/crypto_libraries/src")

file(GLOB _x509_secure_all "${_x509_secure_src}/*.c")
list(FILTER _x509_secure_all EXCLUDE REGEX "self_test")
list(FILTER _x509_secure_all EXCLUDE REGEX "/nx_secure_dtls")
# src/tls/CMakeLists.txt swaps these two for the rfc7905 copies. Neither is
# reached from here, and both need an NX_PACKET, so drop them.
list(FILTER _x509_secure_all EXCLUDE REGEX
     "/nx_secure_tls_record_payload_(en|de)crypt\\.c$")

file(GLOB _x509_crypto_all "${_x509_crypto_src}/*.c")
list(FILTER _x509_crypto_all EXCLUDE REGEX "self_test")

add_library(test_x509_nx_secure STATIC ${_x509_secure_all})
add_library(test_x509_nx_crypto STATIC ${_x509_crypto_all})

# tests/tls is on the path for the sample leaf, the sample CA and ISRG Root X1
#, the certificates that have to keep parsing.
#
# NX_SECURE_ENABLE_AEAD_CIPHER matches src/tls/CMakeLists.txt, where it is set
# PUBLIC on nx_secure: it changes structure layouts in nx_secure_tls.h, so
# without it this would be testing a different NX_SECURE_TLS_SESSION than the
# one that ships.
foreach(lib test_x509_nx_secure test_x509_nx_crypto)
    target_include_directories(${lib} PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/tests/tls"
        "${CMAKE_SOURCE_DIR}/tests/perf/host/shim"      # tx_port.h, first
        "${AMINETXDUO_NETXDUO}/ports/linux/gnu/inc"     # nx_port.h
        "${CMAKE_SOURCE_DIR}/port/netxduo-amiga/inc"    # nx_user.h -- ours
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
        NX_SECURE_ENABLE_AEAD_CIPHER)

    target_compile_options(${lib} PRIVATE -UAMINETXDUO_IPV6)
endforeach()

add_executable(test_tls_x509
    test_tls_x509.c
    "${CMAKE_SOURCE_DIR}/tests/fuzz/fuzz_nxstub.c"
    "${CMAKE_SOURCE_DIR}/tests/fuzz/fuzz_txstub.c")

target_link_libraries(test_tls_x509 PRIVATE
    test_x509_nx_secure test_x509_nx_crypto)

target_compile_options(test_tls_x509 PRIVATE -Wall -Wextra -UAMINETXDUO_IPV6)

# The ECDSA section only, and for the reason fuzz_tls_crypto is 32-bit only:
# nx_crypto_ec.c does pointer arithmetic through ULONG, `(ULONG)ptr -
# (ULONG)naf_data` at :2606, and a loop bound compared the same way at :2713 --
# and ULONG is the target's 32 bits everywhere in this tree, so on an LP64 host
# every one of those truncates a pointer. It is correct on m68k, where ULONG
# and a pointer are the same width.
#
# Raising ULONG to the host's width instead is not available: HN_UBASE follows
# it, and the huge-number carry needs a type twice as wide again.
#
# The other three sections do no such cast, the PKCS#1 and DER work is byte
# arithmetic, and the RSA path stays inside nx_crypto_rsa.c and
# nx_crypto_huge_number.c, neither of which casts a pointer to ULONG, so they
# run in every build. tools/ci.sh's host32 stage is where the ECDSA checks run.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_compile_definitions(test_tls_x509 PRIVATE X509_TEST_ECDSA=1)
endif()

include(CTest)
add_test(NAME tls_x509_checks COMMAND test_tls_x509)
