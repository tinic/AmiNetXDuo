# net68k, 68k data-path primitives for NetX Duo.
#
# AMINETXDUO_NET68K_ASM selects the hand-written 68020 inner loops.  It
# defaults ON when cross-compiling and OFF otherwise, exactly like
# AMINETXDUO_CRYPTO68K_ASM, so a host build still compiles the module in
# portable C, which is what the host tier of the differential test runs --
# and nothing silently picks up assembly that cannot run.  Turning it off is
# the supported way to bisect a suspected assembly bug: the C fallback is the
# same carry chain, longword for longword.
#
# THE TWO SWITCHES HAVE COME APART, and this is now the one that stays on for
# every CPU.  It used to be paired with AMINETXDUO_CRYPTO68K_ASM as a single
# "hand-written 68020 code" decision, which was fine while 68020 was the only
# target.  Adding the 68000 and 68060 builds made the difference matter: both
# files here use nothing but add.l, addx.l, movem.l and dbf, all of which are
# in the original 68000 instruction set and none of which the 68060 dropped,
# so both assemble and run on all four targets, checked, not assumed.
# crypto68k's do not, and it is gated on the CPU accordingly.
#
# This is the good outcome for the 68000 in particular: the checksum and copy
# inner loops are the data path, and they keep their assembly on the slowest
# machine, which is the one that needs it most.  docs/RESEARCH.md §45.
#
# SPDX-License-Identifier: MIT

include(CMakeDependentOption)

option(AMINETXDUO_NET68K_ASM
       "Use the hand-written 68020 inner loops in net68k"
       ${CMAKE_CROSSCOMPILING})

# Replacing the C library's memcpy() is a separate decision from replacing
# NetX Duo's checksum, so it is a separate switch.  Default follows the
# measurement in docs/RESEARCH.md; build both and compare before changing it.
#
# CROSS BUILDS ONLY, and not merely because the movem.l loop is m68k.  A host
# build links a host libc, and defining memcpy() there would replace the C
# library's own, in a test binary, for the whole process.  It also does not
# compile: on macOS <string.h> may define memcpy as a macro, so the definition
# is a syntax error rather than an override.  Neither is a thing to work
# around; the hook has no business existing off target.
cmake_dependent_option(AMINETXDUO_NET68K_MEMCPY
       "Resolve memcpy() to net68k's movem.l copy" ON
       "CMAKE_CROSSCOMPILING" OFF)

set(NET68K_SOURCES
    n68k_checksum.c
    n68k_checksum_hook.c
    n68k_copy.c
    n68k_cpu.c)

if(AMINETXDUO_NET68K_MEMCPY)
    list(APPEND NET68K_SOURCES n68k_memcpy_hook.c)
endif()

# One assembly of each routine per CPU class, or one for the CPU this tree is
# being built for.  AMINETXDUO_CPU=any takes the first: -m68000 codegen with
# every form of the inner loops present and the choice made from AttnFlags at
# init, which is the whole of what makes one binary serve the family.  Any
# other value keeps the single-variant build, unchanged and byte for byte
# comparable, which is what the measurement in the toolchain file is against.
set(NET68K_MV OFF)
if(AMINETXDUO_NET68K_ASM AND AMINETXDUO_CPU STREQUAL "any")
    set(NET68K_MV ON)
endif()

if(AMINETXDUO_NET68K_ASM AND NOT NET68K_MV)
    list(APPEND NET68K_SOURCES n68k_checksum.S n68k_copy.S)
endif()

if(NET68K_MV)
    list(APPEND NET68K_SOURCES n68k_dispatch.S)
endif()

add_library(aminetxduo_net68k STATIC ${NET68K_SOURCES}
    n68k_rx_verify.c)

target_include_directories(aminetxduo_net68k PUBLIC
    ${AMINETXDUO_CORE_INCLUDES}
    "${CMAKE_CURRENT_SOURCE_DIR}")

# Headers only, deliberately no target_link_libraries(... netxduo): the
# top-level CMakeLists makes netxduo depend on THIS, and a static-library
# cycle would be pointless here, nothing in net68k calls back into NetX Duo.

if(AMINETXDUO_NET68K_ASM)
    # The .S is #ifdef'd end to end, so it needs the define as well, an
    # accidental host build then assembles to nothing rather than failing.
    # It also needs the arch flag explicitly: CMAKE_C_FLAGS_INIT in the
    # toolchain file does not reach the assembler.  Same arrangement as
    # src/crypto68k/CMakeLists.txt, and for the same reasons.
    target_compile_definitions(aminetxduo_net68k PRIVATE AMINETXDUO_NET68K_ASM=1)
    set_source_files_properties(n68k_checksum.S n68k_copy.S n68k_dispatch.S
        PROPERTIES
        COMPILE_DEFINITIONS AMINETXDUO_NET68K_ASM=1
        COMPILE_OPTIONS "${AMIGA_ARCH_FLAGS}")
endif()

if(NET68K_MV)
    # Four assemblies of the same two files, one per class, each through a
    # generated one-line wrapper.  Two things force that shape:
    #
    #   * a translation unit of its own, because the variants share local label
    #     names and two in one TU is a duplicate-symbol error;
    #   * a FILE NAME of its own, because the object file name is how
    #     tools/profiler/profreport.py gets from a sample address to a symbol,
    #     through the linker map.  Four archive members called
    #     n68k_checksum.S.obj is legal and builds and links, and it makes a
    #     profile name the wrong variant: the first arm read as
    #     _n68k_copy_sum_longwords_mv40 and _n68k_copy_bytes_mv60 in the same
    #     run on a 68020, which the dispatch cannot produce -- it sets all
    #     three vectors in one branch.  The code was right and the profile was
    #     not, which is the worse of the two failures.
    #
    # ALL FOUR ARE ASSEMBLED WITH THE SAME -m68000 AS THE REST OF THE BINARY,
    # and that is not an oversight.  Nothing in either file is an instruction a
    # 68000 lacks -- movem.l, add.l, addx.l, move.l, lea and dbf, all original
    # -- so what the variants differ in is which of those to use on which part,
    # not what the assembler will accept.  The arch flag would only change what
    # __mc68020__ says, and N68K_MV says it now.
    #
    # Four rather than the three distinct answers the two files actually have
    # (the copy has nothing to say about a 68040, the sum nothing about the
    # difference between a 68000 and a 68020): a uniform table costs about
    # 2 KB of a 330 KB library and removes a per-routine special case from
    # n68k_cpu.c.
    foreach(mv 0 20 40 60)
        foreach(src n68k_checksum n68k_copy)
            set(N68K_MV ${mv})
            set(N68K_MV_SRC "${src}.S")
            set(_gen "${CMAKE_CURRENT_BINARY_DIR}/${src}_mv${mv}.S")
            configure_file(n68k_mv.S.in "${_gen}" @ONLY)
            list(APPEND NET68K_MV_SOURCES "${_gen}")
            set_source_files_properties("${_gen}" PROPERTIES
                COMPILE_DEFINITIONS "AMINETXDUO_NET68K_ASM=1;N68K_MV_MULTI=1"
                COMPILE_OPTIONS "${AMIGA_ARCH_FLAGS}"
                INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
        endforeach()
    endforeach()
    unset(N68K_MV)
    unset(N68K_MV_SRC)
    target_sources(aminetxduo_net68k PRIVATE ${NET68K_MV_SOURCES})

    # The C half of the dispatch needs to know it is in this build too: the
    # vector table and n68k_cpu_select()'s body are both behind it, and without
    # it the file is the empty function every other build gets.
    #
    # PUBLIC, because net68k.h resolves N68K_COPY_BYTES and its two siblings on
    # it, and src/sana2 is one of the callers that must get the same answer as
    # this directory does.  A translation unit that saw it differently would
    # call the trampoline instead of the vector: slower, not wrong, and
    # invisible -- which is a worse failure than a link error.
    target_compile_definitions(aminetxduo_net68k PUBLIC N68K_MV_MULTI=1)

    # THE C IN THIS DIRECTORY IS NOT MULTIVERSIONED, MEASURED AND NOT TAKEN,
    # 2026-08-13.  n68k_checksum.c and n68k_rx_verify.c were built a second
    # time for the 68020-and-up machines, symbols renamed, two more vectors,
    # the checksum hook and src/sana2's two receive call sites dispatched
    # through them.  A1200 profile, 512 KB loopback, six runs an arm, quoted
    # against the -m68020 control in the same run because the control itself
    # drifts ~2 ms between rounds:
    #
    #     assembly dispatch only     +8.0 ms
    #     and the C at -mcpu=68060   +8.9 ms
    #     and the C at -m68020       +11.5 ms
    #
    # Nothing, or slightly worse, for 1.6 KB and an indirect call per packet
    # on two more paths.  It also revises what the earlier arm meant: relinking
    # the whole 68020-built net68k archive was worth 4.4 ms, and this says
    # ~3.7 ms of that was the assembly variants and the rest is noise.  The
    # remaining gap to a per-CPU build is C codegen spread over the whole tree,
    # not concentrated here.
    #
    # One caveat is written down rather than resolved: loopback never enters
    # src/sana2, so n68k_rx_verify() ran zero times in those arms.  Its half of
    # the question needs the bridged rig, where the spread is 13% and the whole
    # effect under test is under 1%.  A per-primitive instrument (perf_test)
    # would be the way in, not another throughput run.
endif()

message(STATUS "  net68k asm .... ${AMINETXDUO_NET68K_ASM}")
message(STATUS "  net68k mv ..... ${NET68K_MV}")
