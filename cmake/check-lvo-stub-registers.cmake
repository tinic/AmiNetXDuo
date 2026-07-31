# Fail the build on a call placed between a library stub's argument registers
# and its `jsr`.
#
# WHAT THIS CATCHES
#
# src/tools/toolsock.c calls bsdsocket.library by hand: each argument is a
# local register variable bound to the register the ABI names, and the vector
# is entered with `jsr a6@(-LVO:W)`.  A local register variable lives in its
# hard register from its initialiser onwards and GCC does not reload it, so a
# call made after one of them has been set returns having clobbered d0, d1, a0
# and a1 -- and the library is entered with the callee's leftovers in the
# caller's arguments.
#
# `tool_sock_sendto()` computed the sockaddr length that way.  At -Os the
# helper inlines and nothing is wrong; at -O0 it is a real `jsr`, and sendto()
# reached the library with d0 holding 16 instead of the descriptor, which the
# stack answered with EBADF.  `ping` printed "could not send the request" on
# every packet in every build that did not inline the helper -- which is every
# build without CMAKE_BUILD_TYPE=Release, since the commands only get their -Os
# under $<CONFIG:Release>.
#
# So this is compiled at -O0 DELIBERATELY.  A Release build inlines the defect
# out of sight; the check exists to see it anyway.
#
# THE INVARIANT
#
# At a `jsr <disp>(a6)`, no argument register may have been written before the
# last subroutine call and left alone since.  d0, d1, a0 and a1 are the four an
# AmigaOS library call may destroy, so they are the four tracked; a call is
# free to sit ahead of the vector as long as every one of them is reloaded
# afterwards, which is what correct code does and what -O0 does once the helper
# is hoisted out of the register variables.
#
# Written as a rule about registers rather than "no calls before a vector",
# because the second is not true of ordinary C -- a function may call anything
# and then make an NDK inline call, since GCC reloads the arguments after it.
# The rule here is the one that actually distinguishes the two.
#
# Inputs: CC, CFLAGS, SOURCE, OBJDUMP, WORKDIR.
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)

if(NOT EXISTS "${SOURCE}")
    message(FATAL_ERROR "check-lvo-stub-registers: no such file: ${SOURCE}")
endif()

if(NOT CC OR NOT OBJDUMP)
    # A check that quietly does nothing is worse than no check: say so.
    message(WARNING "check-lvo-stub-registers: no compiler/objdump, ${SOURCE} unchecked")
    return()
endif()

file(MAKE_DIRECTORY "${WORKDIR}")
set(obj "${WORKDIR}/toolsock-o0.obj")

separate_arguments(cflags NATIVE_COMMAND "${CFLAGS}")

execute_process(COMMAND "${CC}" ${cflags} -O0 -ffunction-sections
                        -c "${SOURCE}" -o "${obj}"
                OUTPUT_VARIABLE cc_out ERROR_VARIABLE cc_err RESULT_VARIABLE cc_rc)
if(NOT cc_rc EQUAL 0)
    message(FATAL_ERROR
        "check-lvo-stub-registers: could not compile ${SOURCE} at -O0:\n${cc_err}")
endif()

execute_process(COMMAND "${OBJDUMP}" -d "${obj}"
                OUTPUT_VARIABLE dis ERROR_VARIABLE dis_err RESULT_VARIABLE dis_rc)
if(NOT dis_rc EQUAL 0)
    message(FATAL_ERROR "check-lvo-stub-registers: objdump failed:\n${dis_err}")
endif()

string(REPLACE ";" "\\;" dis "${dis}")
string(REPLACE "\n" ";" lines "${dis}")

set(argregs d0 d1 a0 a1)

set(fn "")
set(called "")          # the last subroutine this function called, "" if none
set(stale "")           # argument registers written before it and not since
set(written "")         # argument registers written since the last call
set(bad "")
set(checked 0)

foreach(line IN LISTS lines)
    if(line MATCHES "^Disassembly of section [^:]*:" OR
       line MATCHES "^[0-9a-f]+ [0-9a-f]+ _?([A-Za-z_][A-Za-z0-9_]*):")
        set(fn "")
        set(called "")
        set(stale "")
        set(written "")
        if(line MATCHES "^[0-9a-f]+ [0-9a-f]+ _?([A-Za-z_][A-Za-z0-9_]*):")
            set(fn "${CMAKE_MATCH_1}")
        endif()
        continue()
    endif()

    if(fn STREQUAL "" OR NOT line MATCHES "\t[^\t]+\t(.+)$")
        continue()
    endif()
    set(insn "${CMAKE_MATCH_1}")

    if(insn MATCHES "^(jsr|bsr)[a-z.]*[ \t]")
        if(insn MATCHES "\\(a6\\)")
            # The vector. Anything still standing from before the last call is
            # not the value this stub meant to pass.
            math(EXPR checked "${checked} + 1")
            foreach(reg IN LISTS stale)
                list(APPEND bad
                     "${fn} enters the vector with ${reg} holding what ${called} left")
            endforeach()
            set(called "")
            set(stale "")
            set(written "")
        else()
            string(REGEX MATCH "_?[A-Za-z_][A-Za-z0-9_]*$" target "${insn}")
            if(target STREQUAL "")
                set(target "a subroutine")
            endif()
            set(called "${target}")

            # Everything loaded so far is now the callee's leftovers.
            set(stale "")
            foreach(reg IN LISTS argregs)
                if(reg IN_LIST written)
                    list(APPEND stale "${reg}")
                endif()
            endforeach()
            set(written "")
        endif()
        continue()
    endif()

    # The destination of an m68k instruction is its last operand, and a bare
    # single operand is one too (`clr.l d2`).
    string(REGEX REPLACE "^[a-z]+[a-z.0-9]*[ \t]+" "" ops "${insn}")
    string(REGEX MATCH "[^,]*$" dest "${ops}")
    string(STRIP "${dest}" dest)
    if(dest IN_LIST argregs)
        list(APPEND written "${dest}")
        list(REMOVE_ITEM stale "${dest}")
    endif()
endforeach()

if(checked EQUAL 0)
    message(FATAL_ERROR
        "check-lvo-stub-registers: found no library vector call in ${SOURCE} -- "
        "the disassembly was not read as expected, so nothing was checked.")
endif()

if(NOT bad STREQUAL "")
    string(REPLACE ";" "\n  " report "${bad}")
    message(FATAL_ERROR
        "check-lvo-stub-registers: ${SOURCE} enters bsdsocket.library with "
        "clobbered arguments:\n  ${report}\n"
        "A local register variable is not reloaded after a call, so the "
        "library sees whatever the callee left in d0/d1/a0/a1. Compute the "
        "value into a plain local BEFORE the first register variable. See the "
        "hazard note at the head of that file.")
endif()
