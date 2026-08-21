add_library(aminetxduo_common STATIC compat.c crashguard.c ami_random.c
                                     alloccensus.c)
target_include_directories(aminetxduo_common PUBLIC ${AMINETXDUO_CORE_INCLUDES})
target_link_libraries(aminetxduo_common PUBLIC threadx)

# Same reason as src/config: every Shell command in src/tools/ links this and
# uses two or three functions out of compat.c, and --gc-sections can only drop
# what has a section to drop.  See docs/RESEARCH.md §21.
target_compile_options(aminetxduo_common PRIVATE -ffunction-sections)
# -fno-optimize-sibling-calls TRAVELS WITH -ffunction-sections, ALWAYS.
#
# A tail call to a function in ANOTHER SECTION of the SAME object is the one
# construct that makes this toolchain emit a 32-bit PC-relative branch against
# a local section symbol, and the linker resolves that relocation TWELVE BYTES
# SHORT, into the middle of the preceding function.  `ping` rebooted the
# machine on exactly this: `return tool_break();` at the end of
# tool_delay_ticks() landed inside tool_break_arm()'s `move.l #4096,d1`.
#
# -ffunction-sections is what creates the cross-section reference, so the two
# flags are one decision and must not be separated.  cmake/check-pcrel-branches
# .cmake fails the build if one gets through anyway.  docs/RESEARCH.md §25.
target_compile_options(aminetxduo_common PRIVATE -fno-optimize-sibling-calls)

# ------------------------------------------------------------- NX_RAND ---
#
# port/netxduo-amiga/inc/nx_port.h points NX_RAND/NX_SRAND at ami_random.c,
# so every translation unit of the NetX Duo core, the add-ons and nx_secure
# can reference it.  The dependency therefore belongs on the netxduo target
# itself rather than on each consumer, ram_driver_test, for one, links
# netxduo_port and nothing else of ours.
#
# No cycle: aminetxduo_common depends on threadx, never on netxduo.
target_link_libraries(netxduo PUBLIC aminetxduo_common)

# ------------------------------------------------- compiler runtime shim ---
#
# This toolchain ships a ZERO BYTE libgcc.a, so 64-bit division does not link.
# ami_udivdi3.c is the single copy for the whole tree, nx_crypto's
# huge-number long division (src/tls, src/crypto68k), newlib's printf
# (tests/conformance/build.sh compiles this same file), and the E-Clock
# microsecond conversions.  See the header comment in that file.
#
# Its own target, not part of aminetxduo_common: static archives resolve left
# to right in one pass, so the definitions have to be able to sit last on the
# link line.  Cross builds only, on a host, libgcc has the real ones and a
# second definition would collide.
if(CMAKE_CROSSCOMPILING)
    add_library(aminetxduo_m68k_rt STATIC ami_udivdi3.c)

    # DO NOT put -fno-lto on this file.  It was tried and reverted, and the
    # reasoning that led to it was wrong in a way worth writing down.
    #
    # The argument was that these are libcall targets -- nothing calls
    # __udivdi3 in any source, GCC synthesises the call at RTL expansion, which
    # is LTRANS, after the plugin has chosen what to keep -- so a whole-program
    # build would drop them and the link would fail with no libgcc.a to fall
    # back on.  That does not happen.  The plugin resolves the libcall against
    # the archive member at the end, and the whole tree links: 147 targets,
    # zero failures.
    #
    # -fno-lto turns this member from a slim object, whose index holds only
    # ___gnu_lto_slim, into a real one that exports every symbol here --
    # including __mulsi3, __divsi3, __udivsi3, __umodsi3 and __modsi3, which
    # are 68000-only and which newlib's libc.a also defines.  Two real
    # definitions is a multiple-definition error, and crypto68k_test and
    # crypto68k_bench stop linking.  Being last on the link line does not save
    # it: the linker names libc's copy as the first definition, so under LTO
    # the effective pull order is not the command-line order.
    #
    # The 64-bit helpers are ours alone and collide with nothing.  Only the
    # 32-bit five are duplicated, and only because -fno-lto made them visible.
endif()
