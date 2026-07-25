add_library(aminetxduo_common STATIC compat.c crashguard.c ami_random.c)
target_include_directories(aminetxduo_common PUBLIC ${AMINETXDUO_CORE_INCLUDES})
target_link_libraries(aminetxduo_common PUBLIC threadx)

# ------------------------------------------------------------- NX_RAND ---
#
# port/netxduo-amiga/inc/nx_port.h points NX_RAND/NX_SRAND at ami_random.c,
# so every translation unit of the NetX Duo core, the add-ons and nx_secure
# can reference it.  The dependency therefore belongs on the netxduo target
# itself rather than on each consumer -- ram_driver_test, for one, links
# netxduo_port and nothing else of ours.
#
# No cycle: aminetxduo_common depends on threadx, never on netxduo.
target_link_libraries(netxduo PUBLIC aminetxduo_common)

# ------------------------------------------------- compiler runtime shim ---
#
# This toolchain ships a ZERO BYTE libgcc.a, so 64-bit division does not link.
# ami_udivdi3.c is the single copy for the whole tree -- nx_crypto's
# huge-number long division (src/tls, src/crypto68k), newlib's printf
# (tests/conformance/build.sh compiles this same file), and the E-Clock
# microsecond conversions.  See the header comment in that file.
#
# Its own target, not part of aminetxduo_common: static archives resolve left
# to right in one pass, so the definitions have to be able to sit last on the
# link line.  Cross builds only -- on a host, libgcc has the real ones and a
# second definition would collide.
if(CMAKE_CROSSCOMPILING)
    add_library(aminetxduo_m68k_rt STATIC ami_udivdi3.c)
endif()
