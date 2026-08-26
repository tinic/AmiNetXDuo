add_library(aminetxduo_common STATIC compat.c crashguard.c ami_random.c
                                     alloccensus.c events.c budget.c)
target_include_directories(aminetxduo_common PUBLIC ${AMINETXDUO_CORE_INCLUDES})
target_link_libraries(aminetxduo_common PUBLIC threadx)

# The event ring's size, here and nowhere else: events.c is its only reader.
#
# Not add_compile_definitions() at the top level.  A define on every compile
# line of the tree changes the LTO options record of every object, and
# anxnet.device -- which does not link this archive and has no ring in it --
# moved 8 bytes for it.  A build option that changes an image it does not
# belong to makes every size measurement afterwards unreadable.
target_compile_definitions(aminetxduo_common
    PRIVATE AMINETXDUO_EVENT_RING=${AMINETXDUO_EVENT_RING})

# ---------------------------------------------------------------- host test --
#
# The ring and the table that decodes it live in different binaries on the
# machine: bsdsocket.library keeps the ring, ShowNetStatus prints the words.
# Nothing on the Amiga links both, so nothing there can notice a code that was
# added to one and not the other.  This links both, and does.
#
# Neither source makes an AmigaDOS call -- events.c needs Disable()/Enable()
# and ami_millis_quick(), all three stubbed in the test, and tool_events.c
# needs nothing -- so the sources under test are the sources that ship.
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(test_events
        test/test_events.c
        events.c
        "${CMAKE_SOURCE_DIR}/src/tools/tool_events.c")

    target_include_directories(test_events PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src/tools"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/shim"
        "${CMAKE_SOURCE_DIR}/src/config/test/shim")

    target_compile_definitions(test_events
        PRIVATE AMINETXDUO_EVENT_RING=${AMINETXDUO_EVENT_RING})

    target_compile_options(test_events PRIVATE -Wall -Wextra)

    include(CTest)
    add_test(NAME event_ring COMMAND test_events)
endif()

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
# The original 15.2.0 toolchain shipped a zero-byte libgcc.a.  The current
# toolchain has fallbacks, but ami_udivdi3.c remains the single deliberately
# selected, CPU-dispatched copy for the whole tree: nx_crypto's
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

    # THE LIBCALL CONCERN IS REAL.  An earlier version of this comment said it
    # was not; that was wrong, and the correction is why this is written out at
    # length.
    #
    # These are libcall targets: nothing calls __muldi3 in any source, GCC
    # synthesises the call during RTL expansion, which is LTRANS -- after WPA
    # has already decided which LTO definitions survive.  WPA sees no GIMPLE
    # call naming __muldi3 and deletes it, the call is created too late for the
    # definition to come back, and the stale dummy left behind stops the real
    # fallback being pulled either.
    #
    # IT DOES NOT FAIL AS A LINK ERROR.  ld publishes __muldi3 at a ZERO-SIZE
    # symbol on an unrelated function's boundary and exits 0.  P-256 jumps
    # there, fe_sqr returns nonsense, tls.library sends an off-curve key share,
    # and every https: URL fails with BAD_ECPOINT.  It was mis-diagnosed twice
    # on the way -- once as an EC arithmetic miscompile, once as this very
    # concern being unfounded.
    #
    # THE TEST THAT MISSED IT was "the whole tree links, 147 targets, zero
    # failures".  A link-success check cannot see a symbol resolved to the
    # wrong address.  Do not accept one as evidence here again.
    #
    # tls.library was the only image affected, and that is not luck: it is the
    # only one calling ami_rt_cpu_select(), which pulls this archive EARLY,
    # before any libcall exists.  That early pull publishes the dummy WPA then
    # empties.  src/bsdsocket/library.c:497 documents not calling it, which is
    # why that image escaped.
    #
    # The fix is __attribute__((used)) on the definitions in ami_udivdi3.c --
    # the mechanism by which an application-supplied compiler-runtime
    # definition declares reachability the compiler cannot see.  Reduced to 28
    # bytes of linked text against the pinned toolchain.
    #
    # -fno-lto on this file is NOT the fix, for the reason below.
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

    # `used' forces every possible late-libcall definition through WPA.  The
    # named section on each one (ami_udivdi3.c) is what --gc-sections then
    # collects on; an interface -ffunction-sections used to do that job and
    # cost the consumer's WHOLE LTRANS output 0.8-6.6% for it.
    # This toolchain can resolve a cross-section sibling call twelve bytes
    # short.  Keep this pair indivisible, as it is for every other target that
    # enables function sections (see the tree-wide rule in the top-level
    # CMakeLists.txt and cmake/check-pcrel-branches.cmake).
    target_compile_options(aminetxduo_m68k_rt PRIVATE
                           -ffunction-sections -fno-optimize-sibling-calls)
endif()
