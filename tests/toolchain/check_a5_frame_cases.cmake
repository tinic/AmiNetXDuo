# Regression cases for cmake/check-a5-frame.cmake, fed as disassembly listings
# so they run on any host.  Each case is a frame the gate must pass or reject.
#
#   cmake -DSOURCE_DIR=<tree> -DWORK=<dir> -P check_a5_frame_cases.cmake
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)

set(T "\t")
function(listing name)
    set(body "")
    foreach(l IN LISTS ARGN)
        string(APPEND body "${l}\n")
    endforeach()
    file(WRITE "${WORK}/${name}.dis" "${body}")
endfunction()

# The beta5 anxnet.device shape: frame pointer loaded with the routine.
listing(clobber
    "     100:${T}4e55 fff4      ${T}link.w a5,#-12"
    "     104:${T}2a40           ${T}movea.l d0,a5"
    "     106:${T}4eae ffe2      ${T}jsr -30(a6)"
    "     10a:${T}4e5d           ${T}unlk a5"
    "     10c:${T}4e75           ${T}rts")
# Saved to the stack and restored before the epilogue.
listing(stack_restore
    "     100:${T}4e55 fff4      ${T}link.w a5,#-12"
    "     104:${T}2f0d           ${T}move.l a5,-(sp)"
    "     106:${T}2a40           ${T}movea.l d0,a5"
    "     108:${T}4eae ffe2      ${T}jsr -30(a6)"
    "     10c:${T}2a5f           ${T}movea.l (sp)+,a5"
    "     10e:${T}4e5d           ${T}unlk a5"
    "     110:${T}4e75           ${T}rts")
# The NDK Supervisor() inline: swapped out and straight back.
listing(exg_pair
    "     100:${T}4e55 ff88      ${T}link.w a5,#-120"
    "     104:${T}cf8d           ${T}exg d7,a5"
    "     106:${T}4eae ffe2      ${T}jsr -30(a6)"
    "     10a:${T}cf8d           ${T}exg d7,a5"
    "     10c:${T}4e5d           ${T}unlk a5"
    "     10e:${T}4e75           ${T}rts")
# Swapped out and never swapped back.
listing(exg_unmatched
    "     100:${T}4e55 ff88      ${T}link.w a5,#-120"
    "     104:${T}cf8d           ${T}exg d7,a5"
    "     106:${T}4eae ffe2      ${T}jsr -30(a6)"
    "     10a:${T}4e5d           ${T}unlk a5"
    "     10c:${T}4e75           ${T}rts")
# Swapped back with a different register: a5 is not what it was.
listing(exg_mismatch
    "     100:${T}4e55 ff88      ${T}link.w a5,#-120"
    "     104:${T}cf8d           ${T}exg d7,a5"
    "     106:${T}4eae ffe2      ${T}jsr -30(a6)"
    "     10a:${T}cd8d           ${T}exg d6,a5"
    "     10c:${T}4e5d           ${T}unlk a5"
    "     10e:${T}4e75           ${T}rts")
# The same clobber in MIT syntax.
listing(clobber_mit
    "     100:${T}4e55 fff4      ${T}linkw %fp,#-12"
    "     104:${T}2a40           ${T}moveal %d0,%a5"
    "     106:${T}4eae ffe2      ${T}jsr %a6@(-30)"
    "     10a:${T}4e5d           ${T}unlk %a5"
    "     10c:${T}4e75           ${T}rts")

set(cases "clobber:FAIL" "stack_restore:PASS" "exg_pair:PASS"
          "exg_unmatched:FAIL" "exg_mismatch:FAIL" "clobber_mit:FAIL")
set(wrong "")
foreach(c IN LISTS cases)
    string(REPLACE ":" ";" c "${c}")
    list(GET c 0 name)
    list(GET c 1 want)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DDISASM=${WORK}/${name}.dis"
                -P "${SOURCE_DIR}/cmake/check-a5-frame.cmake"
        RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
    if(rc EQUAL 0)
        set(got PASS)
    else()
        set(got FAIL)
    endif()
    message(STATUS "check-a5-frame ${name}: want ${want}, got ${got}")
    if(NOT got STREQUAL want)
        list(APPEND wrong "${name}")
    endif()
endforeach()
if(wrong)
    message(FATAL_ERROR "check-a5-frame self-test: wrong verdict for ${wrong}")
endif()
