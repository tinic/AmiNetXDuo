# Diagnostic probes.  Built with everything else; NOT registered with ctest.
#
# These are not pass/fail regression tests.  Three of them are deliberate
# fault injectors -- two crash the CPU or Guru the machine on purpose, and the
# third boots a child process specifically to find out whether the machine
# survives it -- so they belong nowhere near a "run all tests" flow.  They are
# here so they keep compiling: every one of them was written to answer a
# question that came back later, and a probe that no longer builds is a probe
# that gets rewritten from scratch instead.
#
# Build them all:
#
#   cmake --build build/cm --parallel --target smoke_probes
#
# Run them (each is an AmigaOS executable; none runs on the build host):
#
#   AMINETXDUO_RUN_TAG=smoke ./tools/fsuae-run.sh -t 90  build/cm/tools/smoke/smoke
#   AMINETXDUO_RUN_TAG=crash ./tools/fsuae-run.sh -t 90  build/cm/tools/smoke/crashtest
#   AMINETXDUO_RUN_TAG=guru  ./tools/fsuae-run.sh -t 90  build/cm/tools/smoke/gurutest
#   AMINETXDUO_RUN_TAG=life  ./tools/fsuae-run.sh -t 120 build/cm/tools/smoke/lifecycle
#   AMINETXDUO_RUN_TAG=kstop ./tools/fsuae-run.sh -t 120 build/cm/tools/smoke/KernelStop
#   AMINETXDUO_RUN_TAG=rand1 ./tools/fsuae-run.sh -t 120 build/cm/tools/smoke/randtest
#
# SPDX-License-Identifier: MIT

# smoke        harness self-test: serial output, ENV:/T: assigns, EClock timing.
# crashtest    CRASHES ON PURPOSE -- jumps to 0x2 to prove the CPU-exception
#              guard in include/aminetxduo/crashguard.h catches and reports it.
#              EXITS 10 AND REPORTS "1 failure" ON A GOOD RUN, which is not a
#              regression: the report works (measured -- "CHK instruction
#              (exception 6) at PC=00000044" reached both the serial log and
#              DH0:crash.txt), but the unwind resumes *after* the
#              ami_crash_install() call site rather than at it, so the probe's
#              "unreachable" assertion fires. crashguard.h documents that under
#              WHAT IS NOT RELIABLE. Read the crash report, not the exit code.
# gurutest     GURUS ON PURPOSE -- double-frees a block to prove the exec
#              Alert() hook decodes AN_FreeTwice (0x01000009).
# lifecycle    ThreadX task lifecycle probe; this is what found the
#              AN_FreeTwice root cause.  Arms the Alert hook, so it may Guru
#              if the port regresses.
# KernelStop   tx_amiga_kernel_stop(): re-execs ITSELF as a child process and
#              then fills free memory with ILLEGAL so a stale tick Task
#              jumping into the child's freed hunk faults visibly.  Includes
#              its own negative control.  The binary name is load-bearing --
#              the parent runs "SYS:KernelStop child".
# randtest     entropy pool probe: dumps the raw entropy sources, reports what
#              src/common/ami_random.c credits itself, checks the obvious DRBG
#              failure modes, and writes its first 32 output bytes to
#              DH0:randtest.txt so two cold-boot runs can be diffed.  Does not
#              crash anything.
set(SMOKE_PROBES
    smoke       smoke.c
    crashtest   crashtest.c
    gurutest    gurutest.c
    lifecycle   lifecycle.c
    KernelStop  kernelstop.c
    randtest    randtest.c)

add_custom_target(smoke_probes)

list(LENGTH SMOKE_PROBES _smoke_len)
math(EXPR _smoke_last "${_smoke_len} - 1")
foreach(i RANGE 0 ${_smoke_last} 2)
    list(GET SMOKE_PROBES ${i} _out)
    math(EXPR _j "${i} + 1")
    list(GET SMOKE_PROBES ${_j} _src)

    set(_target "smoke_${_out}")
    add_executable(${_target} ${_src})
    set_target_properties(${_target} PROPERTIES OUTPUT_NAME "${_out}")

    # threadx_port pulls ThreadX and the port headers in for lifecycle.c and
    # kernelstop.c; the rest only need compat.c/crashguard.c/ami_random.c, and
    # a static archive contributes nothing they do not reference.
    target_link_libraries(${_target} PRIVATE threadx_port aminetxduo_common)

    if(AMIGA_NDK_INCLUDE)
        target_include_directories(${_target} PRIVATE "${AMIGA_NDK_INCLUDE}")
    endif()

    add_dependencies(smoke_probes ${_target})
endforeach()
