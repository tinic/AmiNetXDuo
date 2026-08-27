# nx_secure TLS, built only when AMINETXDUO_TLS is ON.
#
# This is deliberately NOT linked into bsdsocket.library.  docs/RESEARCH.md 9
# records the decision: TLS ships as a separate library behind its own build
# option, and is benchmarked on 68020 before any API is promised.  The gate is
# tests/tls/tls_bench, which prints the numbers it produced.
#
# Two static libraries, because they have very different dependency shapes:
#
#   nx_crypto , crypto_libraries/src.  Compiles against nx_api.h for its
#                 types only; it makes no ThreadX or NetX Duo call at runtime
#                 (verified: zero tx_*/nx_* references in crypto_libraries/src).
#                 That is what makes a primitive benchmark possible without
#                 bringing the scheduler up.
#   nx_secure , nx_secure/src.  The TLS/DTLS/X.509 state machines.  These do
#                 need NetX Duo (packets, TCP sockets) and therefore ThreadX.
#
# SPDX-License-Identifier: MIT

set(NX_SECURE_ROOT "${AMINETXDUO_NETXDUO}/nx_secure")
set(NX_CRYPTO_ROOT "${AMINETXDUO_NETXDUO}/crypto_libraries")

option(AMINETXDUO_DTLS "Build the DTLS half of nx_secure (55 files)" OFF)

set(AMINETXDUO_TLS_INCLUDES
    ${AMINETXDUO_CORE_INCLUDES}
    "${NX_SECURE_ROOT}/inc"
    "${NX_SECURE_ROOT}/ports"
    "${NX_CRYPTO_ROOT}/inc")

# ------------------------------------------------- compiler runtime shim ---

# The original 15.2.0 toolchain had no libgcc implementation of `unsigned long
# long` division.  The current toolchain has a fallback, but nx_crypto still
# uses AmiNetXDuo's faster CPU-dispatched implementation.  The single copy
# lives in src/common (aminetxduo_m68k_rt); see its header comment for why we
# retain it rather than dropping the huge-number radix to 16 bits.
#
# src/common is configured unconditionally and before this directory, so on a
# cross build the target is simply there.  On a host build it is absent and
# libgcc supplies the real helpers, so link nothing.
if(TARGET aminetxduo_m68k_rt)
    set(AMINETXDUO_TLS_RT aminetxduo_m68k_rt)
else()
    set(AMINETXDUO_TLS_RT "")
endif()

# ------------------------------------------------------------- nx_crypto ---

file(GLOB NX_CRYPTO_SOURCES "${NX_CRYPTO_ROOT}/src/*.c")

# The self-test objects are empty translation units unless NX_CRYPTO_SELF_TEST
# is defined, but they still carry ~40 KB of test vectors in .rodata when it is.
# We never define it; keep them out so nobody is surprised by the size.
list(FILTER NX_CRYPTO_SOURCES EXCLUDE REGEX "self_test")

add_library(nx_crypto STATIC ${NX_CRYPTO_SOURCES})
target_include_directories(nx_crypto PUBLIC ${AMINETXDUO_TLS_INCLUDES})
target_link_libraries(nx_crypto PUBLIC netxduo ${AMINETXDUO_TLS_RT})

# ------------------------------------------------------------- nx_secure ---

file(GLOB NX_SECURE_SOURCES "${NX_SECURE_ROOT}/src/*.c")
list(FILTER NX_SECURE_SOURCES EXCLUDE REGEX "self_test")
if(NOT AMINETXDUO_DTLS)
    list(FILTER NX_SECURE_SOURCES EXCLUDE REGEX "/nx_secure_dtls")
endif()

# rfc7905/ supplies the two record-payload objects, so the vendored ones must
# not.  Same mechanism as _nx_ip_checksum_compute() at the top level: drop the
# file from the glob and every internal caller resolves to ours instead.  The
# copies differ from the originals by one branch each, the ChaCha20-Poly1305
# record framing, which is decided before an NX_CRYPTO_METHOD is ever called
# and therefore cannot be supplied by one.  src/tls/rfc7905/README is the diff
# and the reason.
list(FILTER NX_SECURE_SOURCES EXCLUDE REGEX "/nx_secure_tls_record_payload_(en|de)crypt\\.c$")
list(APPEND NX_SECURE_SOURCES
     "${CMAKE_CURRENT_SOURCE_DIR}/rfc7905/nx_secure_tls_record_payload_encrypt.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/rfc7905/nx_secure_tls_record_payload_decrypt.c")

# RFC 7301, ALPN.  nx_secure has none, which is why nothing built on it could
# negotiate HTTP/2: h2 over TLS is defined only over a negotiated "h2"
# (RFC 7540 3.2), never by assumption.  This adds it rather than replacing
# anything, so it is an append and not an exclusion.  It lives here and not in
# third_party/ so that a submodule bump is a merge of the seven one-line call
# sites in nx_secure and nothing else; the ProtocolNameList field in
# NX_SECURE_TLS_SESSION is the one thing that cannot live outside the vendored
# header.
list(APPEND NX_SECURE_SOURCES
     "${CMAKE_CURRENT_SOURCE_DIR}/alpn/nx_secure_tls_alpn.c")

add_library(nx_secure STATIC ${NX_SECURE_SOURCES})
target_include_directories(nx_secure PUBLIC ${AMINETXDUO_TLS_INCLUDES})
target_link_libraries(nx_secure PUBLIC nx_crypto netxduo)

# The AEAD record path.  nx_secure_tls.h defines this itself when TLS 1.3 or
# EC-JPAKE is on, and the block below turns TLS 1.3 on, so this is stated
# rather than inherited: the macro has to be identical in every translation
# unit and inheriting it through another header is how they drift.  (This
# comment used to say both were off, which stopped being true when the block
# below was added.)  It is what makes
# the AEAD branches in the two files above, and the 0xCCA8/0xCCA9 rows in
# ami_tls_crypto.c's ciphersuite table, exist at all.  PUBLIC so that every
# translation unit that includes nx_secure_tls.h agrees with this one.
target_compile_definitions(nx_secure PUBLIC NX_SECURE_ENABLE_AEAD_CIPHER)

# TLS 1.3, ON in every build including what ships.  Everything it needs was
# already here and only ever compiled out: nx_secure's seven nx_secure_tls_1_3_*.c files, _nx_crypto_rsa_pss_verify
# for the CertificateVerify that RFC 8446 4.4.3 requires instead of PKCS#1
# v1.5, crypto_method_hkdf for the key schedule, and the three
# NX_SECURE_TLS_TLS_1_3_ENABLED rows plus the hkdf/hmac/ecdhe block in
# ami_tls_crypto.c's table.  PUBLIC for the same reason as the line above: the
# macro changes structure layouts in nx_secure_tls.h, so a translation unit
# that disagrees gets a different NX_SECURE_TLS_SESSION.
target_compile_definitions(nx_secure PUBLIC NX_SECURE_TLS_ENABLE_TLS_1_3)
target_compile_definitions(nx_crypto PUBLIC NX_SECURE_TLS_ENABLE_TLS_1_3)

# tls.library has no renegotiation API and deliberately treats one connection
# as one handshake (see tls_resume.c).  Leaving this compiled in does not just
# retain dead entry points: nx_secure automatically accepts a server
# HelloRequest and starts a second TLS 1.2 handshake when the peer advertised
# RFC 5746, without an application callback being required.  Refuse that
# unsolicited state-machine restart and compile out the unused handshake path.
# PUBLIC because the macro changes NX_SECURE_TLS_SESSION's layout.
target_compile_definitions(nx_secure PUBLIC
    NX_SECURE_TLS_DISABLE_SECURE_RENEGOTIATION)

# The schoolbook inner loop in _nx_crypto_huge_number_multiply is addmul_1, and
# src/crypto68k has that in 68020 assembly over the same limb type.  Only on a
# cross build: crypto68k_ref supplies a C fallback on a host, but the point of
# the substitution is the assembly.
option(AMINETXDUO_C68K_LIMBS
       "Route nx_crypto's huge-number limb loops through src/crypto68k" ON)

# The yield hook, and it is not part of the limb substitution.  One scalar
# multiplication runs for tens of seconds here and the AmigaOS ThreadX port
# does not preempt a thread that makes no ThreadX call, so the vendored curve
# arithmetic has to hand the machine back or the IP thread cannot answer the
# network for the length of a P-384 or P-521 key generation.  Needed whether
# or not the limb loops are substituted, so it is linked outside that test.
if(CMAKE_CROSSCOMPILING)
    target_compile_definitions(nx_crypto PRIVATE NX_CRYPTO_AMIGA_YIELD_HOOK)
    target_link_libraries(nx_crypto PUBLIC crypto68k)
endif()

if(CMAKE_CROSSCOMPILING AND AMINETXDUO_C68K_LIMBS)
    target_compile_definitions(nx_crypto PRIVATE NX_CRYPTO_AMIGA_C68K_LIMBS)
    # crypto68k is configured after this directory; naming a target that does
    # not exist yet is fine, CMake resolves target names at generate time.
    target_link_libraries(nx_crypto PUBLIC crypto68k)

    # nx_crypto_gcm.c already has a longword path for its 16-byte XOR and only
    # takes it when this is defined.  A 68020 handles a misaligned longword;
    # a 68000 takes an address error, so this is 68020-and-up only.  Separately
    # switchable from the limb loops so the two can be bisected against each
    # other, which is how the 68060 TLS 1.3 failure was pinned down.
    option(AMINETXDUO_C68K_GCM
           "Longword GHASH and the longword GCM xor" ON)
    # THE TWO ARE NOT THE SAME QUESTION, and pairing them cost the 68000 the
    # faster GHASH for nothing.
    #
    #   NX_CRYPTO_ENABLE_UNALIGNED_ACCESS is the vendored one: it turns
    #   sixteen byte moves into four longword ones by CASTING the caller's
    #   buffer, which a 68000 answers with an address error.  68020 and up,
    #   and `any` is out with the 68000 because one binary has to run there.
    #
    #   NX_CRYPTO_AMIGA_C68K_GCM is ours, and it copies through an aligned
    #   ULONG[4] with NX_CRYPTO_MEMCPY rather than casting -- the comment at
    #   nx_crypto_gcm.c:184 says so and the code does it.  It needs a
    #   big-endian machine and nothing else, so every 68k can run it, and the
    #   bit-serial GF(2^128) multiply it replaces is 128 iterations of byte
    #   shifting and a call per block on every AES-GCM record.
    if(NOT AMINETXDUO_CPU MATCHES "^(68000|any)$" AND AMINETXDUO_C68K_GCM)
        target_compile_definitions(nx_crypto PRIVATE NX_CRYPTO_ENABLE_UNALIGNED_ACCESS)
    endif()
    if(AMINETXDUO_C68K_GCM)
        target_compile_definitions(nx_crypto PRIVATE NX_CRYPTO_AMIGA_C68K_GCM)
    endif()
endif()

# Wipe key material when it stops being needed.  174 sites across nx_secure
# guard on this, and ami_tls_crypto.c has five of its own; without it the
# handshake hashes, the decrypted signature blocks, the premaster secret and
# every record's plaintext staging buffer are left in whatever memory they were
# in.  On a machine with no MMU that memory goes straight back out to the next
# AllocVec() caller as its uninitialised contents.
#
# It also switches nx_secure_tls_packet_release to the variant that zeroes a
# packet's payload before releasing it, so the cost is one memset per record.
# That macro lives in nx_secure_tls.h, so this has to be PUBLIC or two
# translation units would disagree about which function the name means.
target_compile_definitions(nx_secure PUBLIC NX_SECURE_KEY_CLEAR)
target_compile_definitions(nx_crypto PUBLIC NX_SECURE_KEY_CLEAR)

# ------------------------------------------------------------ amiga glue ---

# ami_tls_crypto.c is what connects src/crypto68k/ to a handshake: it supplies
# our own NX_CRYPTO_METHOD entries for RSA and P-256 and the ciphersuite tables
# that use them.  `crypto68k` is defined in src/crypto68k, which the top level
# configures AFTER this directory, naming a target that does not exist yet is
# fine, CMake resolves target names at generate time.
# The record path's A/B switch.  ON puts the VENDORED AES and SHA-256 back in
# the ciphersuite table and leaves everything else alone, so that "did the
# record path get faster on the wire" can be answered by two tls.library
# binaries built from one commit rather than by comparing against a figure
# from before the TCP layer moved.  See the block above the tables in
# ami_tls_crypto.c.  Not a supported configuration; it is a measuring stick.
option(AMINETXDUO_TLS_STOCK_BULK
       "Use nx_crypto's AES and SHA-256 instead of crypto68k's, for A/B" OFF)

add_library(aminetxduo_tls STATIC tls_amiga.c ami_tls_crypto.c)
if(AMINETXDUO_TLS_STOCK_BULK)
    target_compile_definitions(aminetxduo_tls PRIVATE AMINETXDUO_TLS_STOCK_BULK=1)
    message(STATUS "  TLS bulk path . nx_crypto (A/B baseline)")
else()
    message(STATUS "  TLS bulk path . crypto68k")
endif()
target_include_directories(aminetxduo_tls PUBLIC
    ${AMINETXDUO_TLS_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aminetxduo_tls PUBLIC nx_secure crypto68k ${AMINETXDUO_TLS_RT})

if(AMIGA_NDK_INCLUDE)
    target_include_directories(aminetxduo_tls PUBLIC "${AMIGA_NDK_INCLUDE}")
endif()

# -DAMINETXDUO_SCHEDCOUNT=ON DOES NOT REACH THIS SIDE OF THE TREE.
#
# The option is a global add_compile_definitions(), and the counters it turns on
# are emitted by two headers that every one of these sources pulls in through
# nx_api.h: tx_port.h counts inside TX_DISABLE/TX_RESTORE, and nx_port.h counts
# every tx_mutex_get()/tx_mutex_put().  Both index _tx_amiga_sched_count[], which
# is defined in the ThreadX port -- and the block above this one is the list of
# things tls.library deliberately does NOT link, threadx among them.  So the
# option built a tls.library with `undefined reference to _tx_amiga_sched_count'
# out of nx_secure, and nobody could turn the counters on at all.
#
# Undefined per target rather than made conditional in the headers: these four
# archives are only ever linked into tls.library, which has no ThreadX port and
# therefore no scheduling primitives to count.  There is nothing to lose here.
#
# -U after the -D: CMake emits <DEFINES> before <FLAGS>, so the last word wins.
if(AMINETXDUO_SCHEDCOUNT)
    foreach(_sc_target nx_crypto nx_secure aminetxduo_tls)
        target_compile_options(${_sc_target} PRIVATE -UAMINETXDUO_SCHEDCOUNT)
    endforeach()
endif()
