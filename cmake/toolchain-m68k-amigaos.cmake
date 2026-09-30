# CMake toolchain for m68k-amigaos-gcc (AmigaPorts / newlib).
#
# Usage:
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake
#
# Override the toolchain root with -DAMIGA_TOOLCHAIN_ROOT=<path> if it is not the
# default location used by the local amigaos/ checkout.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR m68k)

# Search order, first hit wins.  Kept identical to tools/amiga-toolchain.sh so
# that a shell script and a CMake configure never pick different compilers:
#
#   1. -DAMIGA_TOOLCHAIN_ROOT / $AMIGA_TOOLCHAIN_ROOT  , explicit
#   2. the PINNED tree in the fetch cache              , what CI uses
#   3. m68k-amigaos-gcc on $PATH                       , container, module
#   4. /opt/m68k-amigaos                               , crosstools layout
#   5. $HOME/amigaos/tools/m68k-amigaos-gcc            , local default
#
# Before this list existed there was only entry 5, which meant a clean checkout
# on any machine but one configured a compiler that was not there and failed
# with a CMake internal error rather than an explanation.
#
# 2 through 5 must also RUN here, not merely exist.  The fetch cache holds a
# linux/amd64 tree and can be populated on a host that cannot execute it (a
# Mac, say, where the headers and the pin are still worth having), and an
# existence test alone would rank that ahead of a working native install.
if(NOT AMIGA_TOOLCHAIN_ROOT)
    if(DEFINED ENV{AMIGA_TOOLCHAIN_ROOT})
        set(AMIGA_TOOLCHAIN_ROOT "$ENV{AMIGA_TOOLCHAIN_ROOT}")
    else()
        if(DEFINED ENV{AMINETXDUO_TOOLCHAIN_CACHE})
            set(_amiga_cache "$ENV{AMINETXDUO_TOOLCHAIN_CACHE}")
        elseif(DEFINED ENV{XDG_CACHE_HOME})
            set(_amiga_cache "$ENV{XDG_CACHE_HOME}/aminetxduo/toolchain")
        else()
            set(_amiga_cache "$ENV{HOME}/.cache/aminetxduo/toolchain")
        endif()

        # THE CACHE CANDIDATE IS THE PIN, NOT <cache>/current.  tools/
        # amiga-toolchain.sh carries the long version; the short one is that
        # `current` is a symlink tools/fetch-toolchain.sh writes when it
        # INSTALLS, so a cache that already holds the pinned tree leaves it
        # addressing whatever was current last and nothing ever compared the
        # two.  The self-hosted emulator runner held both eabb6789378f (the
        # pin, GCC 16.2.0b) and c63033fd4473 (GCC 15.2) with `current` on the
        # older one, and every cross build there ran under GCC 15.2.
        #
        # --print-root prints <cache>/<sha12> for this platform and touches no
        # network.  It exits non-zero on a host with no published asset, where
        # there is no pin to compare anything to.
        set(_amiga_pin "")
        execute_process(
            COMMAND "${CMAKE_CURRENT_LIST_DIR}/../tools/fetch-toolchain.sh"
                    --print-root
            OUTPUT_VARIABLE _amiga_pin
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _amiga_pin_rc)
        if(NOT _amiga_pin_rc EQUAL 0)
            set(_amiga_pin "")
        endif()

        set(_amiga_candidates "")
        if(_amiga_pin)
            list(APPEND _amiga_candidates "${_amiga_pin}")
        endif()

        # A `current` that runs HERE and is not the pin is an error rather than
        # a lower-ranked candidate.  Ranking around it is what let a build use
        # a toolchain nobody asked for and report success: the only symptom was
        # tools/gen-developer.sh --check calling the committed headers stale.
        if(_amiga_pin AND EXISTS "${_amiga_cache}/current/bin/m68k-amigaos-gcc")
            execute_process(
                COMMAND "${_amiga_cache}/current/bin/m68k-amigaos-gcc" -dumpversion
                RESULT_VARIABLE _amiga_cur_rc
                OUTPUT_VARIABLE _amiga_cur_ver
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            get_filename_component(_amiga_cur_real
                                   "${_amiga_cache}/current" REALPATH)
            get_filename_component(_amiga_pin_real "${_amiga_pin}" REALPATH)
            # "Runs here", not "exists", on both sides -- the same test the
            # candidate loop below uses, so the two cannot disagree about
            # whether the pin was available.
            set(_amiga_pin_rc 1)
            if(EXISTS "${_amiga_pin}/bin/m68k-amigaos-gcc")
                execute_process(
                    COMMAND "${_amiga_pin}/bin/m68k-amigaos-gcc" -dumpversion
                    RESULT_VARIABLE _amiga_pin_rc
                    OUTPUT_QUIET ERROR_QUIET)
            endif()
            if(_amiga_cur_rc EQUAL 0 AND NOT _amiga_cur_real STREQUAL _amiga_pin_real)
                if(NOT _amiga_pin_rc EQUAL 0)
                    message(FATAL_ERROR
                        "${_amiga_cache}/current is not the toolchain this "
                        "tree pins.\n"
                        "  want ${_amiga_pin}\n"
                        "  got  ${_amiga_cur_real}  (GCC ${_amiga_cur_ver})\n"
                        "Refusing to build with it: the headers, the codegen "
                        "and the library ABI all move.\n"
                        "Run tools/fetch-toolchain.sh, or pass "
                        "-DAMIGA_TOOLCHAIN_ROOT=<path> to say so on purpose.")
                endif()
            endif()
        endif()

        find_program(_amiga_gcc_on_path m68k-amigaos-gcc)
        if(_amiga_gcc_on_path)
            get_filename_component(_amiga_bin "${_amiga_gcc_on_path}" DIRECTORY)
            get_filename_component(_amiga_from_path "${_amiga_bin}" DIRECTORY)
            list(APPEND _amiga_candidates "${_amiga_from_path}")
        endif()

        list(APPEND _amiga_candidates
             "/opt/m68k-amigaos"
             "$ENV{HOME}/amigaos/tools/m68k-amigaos-gcc")

        foreach(_c IN LISTS _amiga_candidates)
            if(EXISTS "${_c}/bin/m68k-amigaos-gcc")
                execute_process(
                    COMMAND "${_c}/bin/m68k-amigaos-gcc" -dumpversion
                    RESULT_VARIABLE _amiga_runs
                    OUTPUT_QUIET ERROR_QUIET)
                if(_amiga_runs EQUAL 0)
                    set(AMIGA_TOOLCHAIN_ROOT "${_c}")
                    break()
                endif()
            endif()
        endforeach()

        if(NOT AMIGA_TOOLCHAIN_ROOT)
            message(FATAL_ERROR
                "No m68k-amigaos cross toolchain that runs on this host.\n"
                "Looked in: ${_amiga_candidates}\n"
                "Fix it with tools/fetch-toolchain.sh, or configure with "
                "-DAMIGA_TOOLCHAIN_ROOT=<path to the dir holding "
                "bin/m68k-amigaos-gcc>.")
        endif()
    endif()
endif()

if(NOT EXISTS "${AMIGA_TOOLCHAIN_ROOT}/bin/m68k-amigaos-gcc")
    message(FATAL_ERROR
        "AMIGA_TOOLCHAIN_ROOT=${AMIGA_TOOLCHAIN_ROOT} has no "
        "bin/m68k-amigaos-gcc.")
endif()

# Validate the startup objects here, before CMake links even its compiler
# probe.  CI also runs this check explicitly, but a direct local configure or
# an explicitly selected external toolchain must not be able to bypass it.
# The checker is semantic and per multilib: it accepts the reviewed upstream
# backing-storage shape as well as our repaired prebuilt objects, and refuses
# an instruction shape it cannot prove.
find_program(_AMIGA_PYTHON3 NAMES python3 REQUIRED)
execute_process(
    COMMAND "${_AMIGA_PYTHON3}"
            "${CMAKE_CURRENT_LIST_DIR}/../tools/fix-toolchain-crt0.py"
            "${AMIGA_TOOLCHAIN_ROOT}" --check
    RESULT_VARIABLE _amiga_crt0_result
    OUTPUT_VARIABLE _amiga_crt0_stdout
    ERROR_VARIABLE _amiga_crt0_stderr)
if(NOT _amiga_crt0_result EQUAL 0)
    message(FATAL_ERROR
        "The selected m68k-amigaos toolchain has an unsafe or unrecognized "
        "crt0:\n${_amiga_crt0_stdout}${_amiga_crt0_stderr}\n"
        "Repair it with tools/fix-toolchain-crt0.py "
        "${AMIGA_TOOLCHAIN_ROOT}, or fetch the pinned toolchain.")
endif()
unset(_amiga_crt0_result)
unset(_amiga_crt0_stdout)
unset(_amiga_crt0_stderr)
unset(_AMIGA_PYTHON3 CACHE)

set(AMIGA_TOOLCHAIN_BIN "${AMIGA_TOOLCHAIN_ROOT}/bin")
set(AMIGA_TOOLCHAIN_PREFIX "${AMIGA_TOOLCHAIN_BIN}/m68k-amigaos-")

set(CMAKE_C_COMPILER   "${AMIGA_TOOLCHAIN_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${AMIGA_TOOLCHAIN_PREFIX}c++")
set(CMAKE_ASM_COMPILER "${AMIGA_TOOLCHAIN_PREFIX}gcc")
# gcc-ar/gcc-ranlib/gcc-nm, not the bare ones, because of -flto.
#
# A slim LTO object carries its symbols in the IR, not in the native symbol
# table, and only the LTO plugin can read them. An `ar' that has not loaded the
# plugin writes an archive index holding exactly one name per member,
# ___gnu_lto_slim, so ld pulls in no member at all and every symbol the archive
# was meant to provide comes out undefined. The gcc-* wrappers are the same
# programs with --plugin already pointed at liblto_plugin.so.
#
# The toolchain also symlinks the plugin into <prefix>/lib/bfd-plugins, which
# makes the bare tools work as well. Both, deliberately: the symlink covers
# every other consumer of this toolchain, and these three cover a toolchain
# built somewhere that missed it. Getting this wrong does not fail at the
# archive step, it fails much later at the link, which is why it is worth the
# belt and the braces.
set(CMAKE_AR           "${AMIGA_TOOLCHAIN_PREFIX}gcc-ar"     CACHE FILEPATH "")
set(CMAKE_RANLIB       "${AMIGA_TOOLCHAIN_PREFIX}gcc-ranlib" CACHE FILEPATH "")
set(CMAKE_NM           "${AMIGA_TOOLCHAIN_PREFIX}gcc-nm"     CACHE FILEPATH "")
set(CMAKE_STRIP        "${AMIGA_TOOLCHAIN_PREFIX}strip"  CACHE FILEPATH "")
set(CMAKE_OBJDUMP      "${AMIGA_TOOLCHAIN_PREFIX}objdump" CACHE FILEPATH "")
set(AMIGA_SIZE         "${AMIGA_TOOLCHAIN_PREFIX}size"   CACHE FILEPATH "")

# The toolchain produces AmigaOS hunk executables, not something the host can run.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(AMIGA_NDK_INCLUDE "${AMIGA_TOOLCHAIN_ROOT}/m68k-amigaos/ndk-include"
    CACHE PATH "NDK include directory (Roadshow bsdsocket + SANA-II headers live here)")

# The NDK headers ship WITH the toolchain, so a root without them is a broken
# or partial install rather than something to work around.  Say so here: the
# alternative is a hundred "exec/types.h: No such file" errors at build time.
if(NOT EXISTS "${AMIGA_NDK_INCLUDE}/exec/types.h")
    message(FATAL_ERROR
        "No NDK headers at ${AMIGA_NDK_INCLUDE}. They come with the toolchain; "
        "if yours keeps them elsewhere, pass -DAMIGA_NDK_INCLUDE=<path>.")
endif()

# The NDK inline headers const-qualify 46 of their register parameters, and
# GCC drops the register binding on a const one whose value is a link-time
# constant: Write(fh, static_buffer, len) then puts the address in d0 and the
# library reads d2, which nobody wrote.  There is no diagnostic and the wrong
# code links, so this is checked here rather than left to be discovered.
# tools/fetch-toolchain.sh repairs a toolchain as it installs it; a cache from
# before that repair existed is still on disk on every machine that had one.
file(READ "${AMIGA_NDK_INCLUDE}/inline/dos.h" _amiga_dos_inline LIMIT 65536)
if(_amiga_dos_inline MATCHES "LP3\\(0x30, LONG, Write[^\n]*const APTR")
    message(FATAL_ERROR
        "${AMIGA_NDK_INCLUDE}/inline/dos.h still declares Write()'s buffer "
        "const APTR, so every call passing a static buffer puts the address "
        "in the wrong register and the file written is whatever d2 held.\n"
        "Repair the toolchain in place:\n"
        "  tools/fix-toolchain-inline-const.py ${AMIGA_TOOLCHAIN_ROOT}\n"
        "tools/fetch-toolchain.sh does this for a tree it installs.")
endif()
unset(_amiga_dos_inline)

# -noixemul is NOT usable with this newlib-based toolchain: it breaks sys/reent.h.
# See docs/RESEARCH.md §5.4.

# ------------------------------------------------------------- target CPU ---
#
# -DAMINETXDUO_CPU=any|68000|68020|68040|68060.  `any` is the default and is
# what ships; the four CPU values build for one part and exist to measure
# against.  docs/RESEARCH.md §45 is where the four entries below come from.
#
# THE FLAGS ARE NOT THE OBVIOUS ONES, because this toolchain ships exactly
# three multilibs, `.` (68000), `libm020` (@mcpu=68020) and `libm060`
# (@mcpu=68060), and the multilib is selected by the canonical -mcpu value:
#
#   68000   -m68020 is the whole difference; the C library is the `.` one.
#   68020   as before.
#   68040   -m68020 -mtune=68040, NOT -m68040.  There is no 68040 multilib, so
#           -m68040 silently selects `.` and links the 68000 C library, code
#           that works but has had every 32-bit multiply and divide turned
#           into a subroutine call.  -m68020 -mtune=68040 keeps libm020 and
#           schedules for the 040, and the 040 implements every 68020
#           instruction, so nothing is lost.  This is also what AmiSSL does:
#           it ships one `68020-40` build and one `68060` build.
#   68060   -mcpu=68060, which is a genuinely different target rather than a
#           tuning choice: the 68060 DROPPED the 64-bit-result forms of MULU.L
#           and DIVU.L, so GCC must not emit them.  They trap to vector 61 and
#           are emulated by 68060.library, which is correct but slow, and it
#           is why the hand-written 68020 assembly must stay off there.
#
# The FPU is deliberately absent from all four.  No -m68881 anywhere: nothing
# in this stack uses floating point (the trust store, the checksums and the
# bignums are all integer), the m68881 multilibs exist only in the 68020 row,
# and a library that requires an FPU would refuse to load on the 68020s and
# 68EC020s that do not have one.  A soft-float build runs everywhere.
#
# `any` is the fifth value and it is not a CPU: it is -m68000 codegen, which
# every 68k runs, plus src/net68k's inner loops assembled once per CPU class
# and chosen from SysBase->AttnFlags at library init.  One binary for the whole
# family.  What it costs is measured, on the A1200 profile, 512 KB loopback,
# six runs an arm: -m68000 throughout is 987.8 ms against the -m68020 build's
# 973.8, and giving the assembly back its 68020 form recovers 6 of those 14 ms.
# The rest is instruction selection spread over the C, and none of it is the
# 32-bit multiply and divide helpers, which no data-path object references
# (src/net68k/n68k_cpu.c).
# THE DEFAULT IS `any`, and the per-CPU values are measurement configurations
# now rather than shipping ones.  The archive has no CPU in it: one build of
# every library, device and command, choosing its own inner loops at init.  Keep
# the specific values -- every figure in this file and in src/net68k was taken
# by building one of them and comparing back to back, and that is the only way
# to take the next one.
set(AMINETXDUO_CPU "any" CACHE STRING "Target CPU: any (every 68k), or 68000, 68020, 68040, 68060 for a measurement build")
set_property(CACHE AMINETXDUO_CPU PROPERTY STRINGS 68000 68020 68040 68060 any)

set(_amiga_cpu_flags_68000 "-m68000")
set(_amiga_cpu_flags_68020 "-m68020")
set(_amiga_cpu_flags_68040 "-m68020;-mtune=68040")
set(_amiga_cpu_flags_68060 "-mcpu=68060")
set(_amiga_cpu_flags_any   "-m68000")

if(NOT DEFINED _amiga_cpu_flags_${AMINETXDUO_CPU})
    message(FATAL_ERROR
        "AMINETXDUO_CPU=${AMINETXDUO_CPU} is not one of 68000, 68020, 68040, "
        "68060, any.  A 68010 runs the 68000 build and a 68030 runs the 68020 "
        "one; there is no separate configuration for either.")
endif()

# AMIGA_ARCH_FLAGS is what the rest of the tree reads, including the two
# CMakeLists that hand it to the assembler by name.  Setting it explicitly
# still works and wins, the probe builds that established §45 were done
# that way, so this only fills it in when nobody has said otherwise.
set(AMIGA_ARCH_FLAGS "${_amiga_cpu_flags_${AMINETXDUO_CPU}}"
    CACHE STRING "Target CPU flags (derived from AMINETXDUO_CPU)")

# A CONFIGURED DIRECTORY CANNOT BE MOVED TO ANOTHER CPU, AND SAYING NOTHING
# ABOUT IT IS HOW THREE TREES CAME TO COMPILE -m68000 WHILE REPORTING 68020 --
# this machine's build/cm and BOTH A/B worktrees on the rig, so every rate
# number taken out of them was of an architecture nobody asked for.  The
# default is `any`, which is -m68000; add -DAMINETXDUO_CPU=68020 to a
# directory that already exists and the label moves while the compiler does
# not.  `set(... CACHE ...)` without FORCE leaves an existing entry alone, and
# CMAKE_C_FLAGS_INIT above is consulted only when the cache is created, so the
# flags that actually reach gcc keep their first value forever.
#
# Refuse, rather than repair: CMAKE_C_FLAGS is already written and forcing
# AMIGA_ARCH_FLAGS would fix the label and not the build, which is the same
# lie one level down.  A fresh directory costs a rebuild; a silent mismatch
# costs every measurement taken since.
#
# An AMIGA_ARCH_FLAGS matching no entry in the table is a deliberate override
# -- the probe builds behind docs/RESEARCH.md 45 were done that way -- and is
# left alone, which is what the comment above promises.
if(NOT AMIGA_ARCH_FLAGS STREQUAL "${_amiga_cpu_flags_${AMINETXDUO_CPU}}")
    set(_amiga_flags_are_a_cpu FALSE)
    foreach(_amiga_c 68000 68020 68040 68060 any)
        if(AMIGA_ARCH_FLAGS STREQUAL "${_amiga_cpu_flags_${_amiga_c}}")
            set(_amiga_flags_are_a_cpu TRUE)
        endif()
    endforeach()
    if(_amiga_flags_are_a_cpu)
        message(FATAL_ERROR
            "AMINETXDUO_CPU=${AMINETXDUO_CPU} wants "
            "'${_amiga_cpu_flags_${AMINETXDUO_CPU}}', but this build directory "
            "was configured with '${AMIGA_ARCH_FLAGS}' and CMAKE_C_FLAGS still "
            "carries it.  A cached entry is not rewritten by -D, so the "
            "compiler would keep the old architecture while every report said "
            "the new one.  Configure a FRESH build directory.")
    endif()
endif()

string(REPLACE ";" " " AMIGA_ARCH_FLAGS_STR "${AMIGA_ARCH_FLAGS}")

# ------------------------------------------------------- argument passing --
#
# Arguments used to travel on the stack: a push per argument at every call site
# and a read at the callee, six to twelve bytes each, thousands of times.
#
# -mregparm=N is PER CLASS, not one list: the first N integer arguments go to
# d0..d(N-1) and the first N pointer arguments to a0..a(N-1), so N=3 is six
# slots, and a class that runs out spills into what the other class left.
# Measured with a probe calling ext(a,b,c,d,e,f): six ints give d0,d1,d2,a0,a1,a2,
# six pointers give a0,a1,a2,d0,d1,d2, (int,char*,int,char*) gives d0,a0,d1,a1.
# "d0/d1/d2" is the wrong shorthand -- a pointer can arrive in d0 and an int in
# a0, and a callee that assumes otherwise returns a wrong number, not a crash.
# That is why every such callee is pinned rather than taught the register set.
#
# Measured against the same tree built with AMINETXDUO_REGPARM=0, the flag the
# only difference (m68k-amigaos-gcc 16.2.0b, -flto), in LOADED bytes -- CODE +
# DATA + BSS, what LoadSeg has to find room for -- and in file bytes.  The two
# images a machine keeps resident, default drawer:
#
#   bsdsocket.library    353,412 -> 329,468 loaded    370,888 -> 346,612 file
#   anxnet.device         43,592 ->  40,148 loaded     45,712 ->  42,232 file
#
# so the pair costs 27,388 bytes less loaded (23,944 + 3,444).  bsdsocket.library
# loses 23,944 loaded bytes in default (343,428 -> 319,484 of code, DATA and BSS
# unchanged), 15,888 in minimal (231,140 -> 215,252) and 13,356 in micro
# (198,248 -> 184,892).  Over every image BOTH arms link: 137 images 7,906,460
# -> 7,696,820 loaded (-209,640, -2.65%), 123 minimal -93,460 (-2.12%), 122
# micro -87,356 (-2.06%).  Four test images grew, by 4 to 16 bytes
# (tests/perf/n68kmv +16, tests/perf/chipscreen +16, tests/tools/PtrProbe +4,
# ResolveBreak +4); nothing that ships did, and no image is present in one arm
# only.
#
# SAFE ONLY WHERE THE CONVENTION IS PINNED at both ends, which it is at every
# boundary this project has but one:
#
#   - library and device entry points (bsdsocket_vectors.h, library.c, the
#     netdev entries) pin with `__asm("d0")` / `__asm("a6")`, which GCC honours
#     whatever -mregparm says;
#   - user-supplied hooks (loghook.c, errno.c, netmonitor.c) pin a0/a1/a2, so a
#     caller compiled the ordinary way is still called correctly;
#   - the hand-written routines in src/net68k and src/crypto68k read the stack,
#     and every C declaration of one carries AMIGA_ASM_ARGS
#     (__attribute__((__stkparm__))); see include/aminetxduo/asm_abi.h.
#
# The one exception is ami_rt_cpu_select(), deliberately: it follows whatever
# convention the build uses so that src/common/ami_udivdi3.c and its callers
# stay header-free.  Safe because its one non-C caller covers both at once --
# tool_startup.S .Lrtgo loads the flags into d0/d1 AND pushes the same
# registers.  It is the only such boundary.
#
# main() IS THE ONE THAT GOT AWAY, and -include asm_main.h below is its pin.
# Nothing in this tree calls it: crt0.o and tool_startup.S both push argv then
# argc and `jsr _main`, neither is ours to edit or compiled with these flags, so
# a C main() under -mregparm=3 reads argc out of d0 instead.  Measured on the
# first build that carried the option: iperf's _main at 0x207e was `tst.l d0`;
# it is `tst.l 8(a5)` now, same address.  A command that reads argc == 0 thinks
# Workbench launched it.  The pin is a force-included header rather than
# -Dmain=... because the -D reaches /bin/sh with unquoted parentheses and the
# configure dies before gcc runs; see the header for the rest.  It is gated on
# the same option, so AMINETXDUO_REGPARM=0 reproduces the old convention
# exactly, and 0 is a supported build.
set(AMINETXDUO_REGPARM "3" CACHE STRING
    "Integer arguments passed in registers d0-d2 (0 disables, as before)")
set_property(CACHE AMINETXDUO_REGPARM PROPERTY STRINGS 0 1 2 3)

if(AMINETXDUO_REGPARM GREATER 0)
    get_filename_component(_amiga_top "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    set(_amiga_regparm_flags
        "-mregparm=${AMINETXDUO_REGPARM} -include ${_amiga_top}/include/aminetxduo/asm_main.h")
else()
    set(_amiga_regparm_flags "")
endif()

set(CMAKE_C_FLAGS_INIT
    "${AMIGA_ARCH_FLAGS_STR} -fomit-frame-pointer -fno-strict-aliasing ${_amiga_regparm_flags}")

# -mregparm and the pin travel in CMAKE_C_FLAGS, and CMAKE_C_FLAGS_INIT is
# consulted only when the cache is created: -DAMINETXDUO_REGPARM=0 on an
# existing directory would leave -mregparm=3 on the command line while the
# cache said 0, so the tree would report one convention and build the other.
# Unlike the CPU guard above, this one repairs rather than refuses.  Both flags
# reach the preprocessor and the C compiler and nothing else -- the assembler
# never sees them -- so rewriting CMAKE_C_FLAGS is the whole change.  The first
# configure has no cache entry and is left to CMAKE_C_FLAGS_INIT, which is
# already right.  Stripping the pin matters as much as stripping -mregparm:
# removing one and leaving the other is the same lie with the halves swapped.
if(DEFINED CACHE{CMAKE_C_FLAGS})
    set(_amiga_cf "${CMAKE_C_FLAGS}")
    string(REGEX REPLACE " ?-mregparm=[0-9]+" "" _amiga_cf "${_amiga_cf}")
    string(REGEX REPLACE " ?-include +[^ ]+asm_main\\.h" "" _amiga_cf "${_amiga_cf}")
    string(STRIP "${_amiga_cf} ${_amiga_regparm_flags}" _amiga_cf)
    if(NOT _amiga_cf STREQUAL CMAKE_C_FLAGS)
        message(STATUS "regparm ${AMINETXDUO_REGPARM}: CMAKE_C_FLAGS is now '${_amiga_cf}'")
        set(CMAKE_C_FLAGS "${_amiga_cf}" CACHE STRING "C compiler flags" FORCE)
    endif()
endif()

# -Os, everywhere, and stated rather than inherited.  CMake's Compiler/GNU
# module APPENDS its own "-O3 -DNDEBUG" after CMAKE_C_FLAGS_RELEASE_INIT and
# the last -O on the command line wins, so this line has to name the level it
# wants or it gets -O3 whatever it says.  It said -O2 for a long time and
# produced -O3 builds throughout, which is how every published figure in this
# project came to be measured at a level the file denied.
#
# WHY -Os RATHER THAN -O3 (docs/RESEARCH.md 57)
#
# On a 68000 or an 020 with a 16-bit path to memory, instruction fetch IS the
# bottleneck: there is no cache worth the name on a 68000, the 020's is 256
# bytes, and everything competes for the same slow bus that the data is on.
# Smaller code is faster code far more often than it is on a modern machine,
# and -O3's unrolling and inlining buy speed with exactly the resource that is
# scarcest here.
#
# The hot paths do not depend on the compiler for their speed and never did:
# the checksum and copy primitives, the AES and ChaCha20 kernels are hand
# written 68020 assembly, chosen and measured against C alternatives
# (tests/crypto68k/crypto68k_bulk prints both).  So the optimiser is being
# asked to make the OTHER 95% small, which is what it is good at.  The one
# exception is AMINETXDUO_HOT_O2 (CMakeLists.txt:474): eight profiled NetX Duo
# files build at -O2.
set(CMAKE_C_FLAGS_RELEASE_INIT "-Os -DNDEBUG")
set(CMAKE_C_FLAGS_DEBUG_INIT "-O1 -g -DAMINETXDUO_DEBUG=1")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
