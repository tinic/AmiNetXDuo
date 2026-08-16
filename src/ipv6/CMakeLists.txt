# src/ipv6, the parts of IPv6 that are ours rather than NetX Duo's.
#
# One file so far: RFC 6724 source address selection, which supplies
# _nxd_ipv6_interface_find() in place of the vendored object the top-level
# CMakeLists drops from the glob.  Same mechanism as src/net68k and the IP
# checksum, and for the same reason -- nothing in third_party/ is edited and
# no call site is patched, so every caller inside NetX Duo picks it up.
#
# The library is linked PUBLIC onto netxduo by the top-level CMakeLists, which
# is what makes the symbol reachable from the vendored archive members that
# reference it.  It depends on nothing of ours: the file reads NX_IP's address
# and interface tables and calls no function outside itself, which is also
# what lets tests/ipv6/host drive it with a hand-built NX_IP.
#
# SPDX-License-Identifier: MIT

add_library(aminetxduo_ipv6 STATIC ipv6_srcsel.c)

target_include_directories(aminetxduo_ipv6 PUBLIC ${AMINETXDUO_CORE_INCLUDES})

# Headers only, deliberately no target_link_libraries(... netxduo): the
# top-level CMakeLists makes netxduo depend on this library and a
# static-library cycle serves no purpose, because nothing here calls back into
# NetX Duo.

# Every Shell command and every library that links the stack pulls this in, and
# --gc-sections can only drop what has a section to drop.  Same pairing as
# src/common: -fno-optimize-sibling-calls travels with -ffunction-sections
# always, see the note there for the relocation this toolchain gets wrong.
target_compile_options(aminetxduo_ipv6 PRIVATE
    -ffunction-sections -fno-optimize-sibling-calls)
