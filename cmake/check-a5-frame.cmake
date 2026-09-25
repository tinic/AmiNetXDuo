# Fail the build on a write to a5 inside a function that uses a5 as its frame
# pointer.
#
# WHAT THIS CATCHES
#
# netdev_cache.c entered Supervisor() through a local register variable pinned
# to a5, the register Supervisor() takes its routine in.  GCC does not support
# a register variable on the frame pointer of a function that needs a frame:
# when LTO inlined the helper into a caller it chose to frame with `link a5`,
# the call became `movea.l d0,a5; jsr -30(a6)` and nothing reloaded a5, so the
# caller's `unlk a5; rts` set the stack pointer to the TT-write stub and
# returned into its opcode words.  An A3000 with an X-Surf 100 took a Line-F
# dead-end alert in ramlib and rebooted on every load of that anxnet.device,
# and v1.0.0-beta5 shipped the same code.  -fomit-frame-pointer is on at
# compile time; the LTO link step framed the caller anyway, so the source alone
# cannot promise this and the linked image has to be read.
#
# THE INVARIANT
#
# In a function framed with `link a5`, the `unlk a5` must not be reached with
# a5 last written by anything other than a restore from the stack: that is the
# epilogue returning through a clobbered frame pointer.  Checked in address
# order (an LTO map has one entry per ltrans object, not per function), with
# every `link` starting clean, so an unframed stretch or a frame whose epilogue
# is laid out elsewhere is not judged.  It can miss; it does not flag code
# whose frame is intact at its epilogue.
#
# Both disassembly syntaxes are accepted (Motorola `movea.l d0,a5`, MIT
# `moveal %d0,%a5` / `%fp`), for the reason check-pcrel-branches.cmake gives.
#
# Inputs: BINARY, MAPFILE, OBJDUMP; or DISASM, a listing, for the ctest cases.
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)

if(DISASM)
    # A disassembly listing instead of an image: the ctest fixtures
    # (tests/toolchain/check_a5_frame_cases.cmake).
    file(READ "${DISASM}" dis_out)
    set(BINARY "${DISASM}")
else()
    if(NOT EXISTS "${BINARY}")
        message(FATAL_ERROR "check-a5-frame: no such file: ${BINARY}")
    endif()
    if(NOT OBJDUMP)
        # A gate that cannot read the image must not pass it.
        message(FATAL_ERROR "check-a5-frame: no objdump, cannot check ${BINARY}")
    endif()
    execute_process(COMMAND "${OBJDUMP}" -d "${BINARY}"
                    OUTPUT_VARIABLE dis_out ERROR_VARIABLE dis_err
                    RESULT_VARIABLE dis_rc)
    if(NOT dis_rc EQUAL 0)
        message(FATAL_ERROR "check-a5-frame: objdump failed on ${BINARY}:\n${dis_err}")
    endif()
endif()

# Function entries from the map, same reading as check-pcrel-branches.cmake.
set(entries "")
if(MAPFILE AND EXISTS "${MAPFILE}")
    file(STRINGS "${MAPFILE}" map_lines)
    set(pending "")
    foreach(line IN LISTS map_lines)
        if(pending)
            if(line MATCHES "^[ \t]+0x0*([0-9a-fA-F]+)[ \t]+0x[0-9a-fA-F]+")
                math(EXPR a "0x0${CMAKE_MATCH_1}")
                list(APPEND entries "${a}")
            endif()
            set(pending "")
        elseif(line MATCHES "^ (\\.text[^ \t]*)[ \t]+0x0*([0-9a-fA-F]+)[ \t]+0x[0-9a-fA-F]+")
            math(EXPR a "0x0${CMAKE_MATCH_2}")
            list(APPEND entries "${a}")
        elseif(line MATCHES "^ (\\.text[^ \t]*)[ \t]*$")
            set(pending "${CMAKE_MATCH_1}")
        elseif(line MATCHES "^[ \t]+0x0*([0-9a-fA-F]+)[ \t]+_[A-Za-z_][A-Za-z0-9_.]*[ \t]*$")
            math(EXPR a "0x0${CMAKE_MATCH_1}")
            list(APPEND entries "${a}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES entries)
endif()

# Is register a5 (alias fp) in this destination operand, alone or in a list?
function(_a5_in dest out)
    string(REPLACE "%" "" d "${dest}")
    string(REGEX REPLACE "(^|[/-])fp($|[/-])" "\\1a5\\2" d "${d}")
    set(hit 0)
    if(d STREQUAL "a5")
        set(hit 1)
    elseif(d MATCHES "^[ad][0-7]([-/][ad][0-7])+$")
        string(REPLACE "/" ";" parts "${d}")
        foreach(p IN LISTS parts)
            if(p STREQUAL "a5")
                set(hit 1)
            elseif(p MATCHES "^a([0-7])-a([0-7])$")
                if(CMAKE_MATCH_1 LESS_EQUAL 5 AND CMAKE_MATCH_2 GREATER_EQUAL 5)
                    set(hit 1)
                endif()
            elseif(p MATCHES "^d[0-7]-a([0-7])$")
                if(CMAKE_MATCH_1 GREATER_EQUAL 5)
                    set(hit 1)
                endif()
            endif()
        endforeach()
    endif()
    set(${out} ${hit} PARENT_SCOPE)
endfunction()

string(REPLACE "\n" ";" dis_lines "${dis_out}")
set(framed 0)
set(frame_at "")
set(pending "")      # the last a5 write in this frame, not yet superseded
set(pending_exg "")  # the register a5 was exchanged with, if that is the write
set(bad "")
foreach(line IN LISTS dis_lines)
    # "    380a:\t2a40           \tmovea.l d0,a5"
    if(NOT line MATCHES "^ +([0-9a-fA-F]+):\t[0-9a-fA-F ]+\t([a-z][a-z0-9.]*)[ \t]*(.*)$")
        continue()
    endif()
    set(addr_hex "${CMAKE_MATCH_1}")
    set(mnem "${CMAKE_MATCH_2}")
    set(ops "${CMAKE_MATCH_3}")
    math(EXPR addr "0x${addr_hex}")
    if(addr IN_LIST entries)
        set(framed 0)
        set(pending "")
    endif()
    string(REPLACE "%" "" ops_n "${ops}")
    if(mnem MATCHES "^link")
        # Any new frame starts clean; only an a5 frame is tracked.
        set(framed 0)
        set(pending "")
        if(ops_n MATCHES "^(a5|fp),")
            set(framed 1)
            set(frame_at "${addr_hex}")
        endif()
        continue()
    endif()
    if(NOT framed)
        continue()
    endif()
    if(mnem MATCHES "^unlk" AND ops_n MATCHES "^(a5|fp)$")
        # The epilogue reads the saved frame through a5: it must be intact.
        if(pending)
            list(APPEND bad "${pending}, then unlk a5 at 0x${addr_hex} (frame 0x${frame_at})")
        endif()
        set(framed 0)
        set(pending "")
        continue()
    endif()
    if(mnem MATCHES "^(cmp|tst|btst|chk)")
        continue()
    endif()
    # exg is a swap, not a write: `exg d7,a5; jsr -30(a6); exg d7,a5` is the
    # NDK's own Supervisor() inline, and the second exg puts the frame back.
    if(mnem MATCHES "^exg" AND ops_n MATCHES "(^|,)(a5|fp)(,|$)")
        string(REGEX REPLACE "(^|,)(a5|fp)(,|$)" "" other "${ops_n}")
        string(STRIP "${other}" other)
        if(pending_exg STREQUAL other)
            set(pending "")
            set(pending_exg "")
        else()
            set(pending "0x${addr_hex}: ${mnem} ${ops} swaps a5 out")
            set(pending_exg "${other}")
        endif()
        continue()
    endif()
    # Destination = the operand after the last comma outside parentheses.
    string(REGEX REPLACE "\\([^)]*\\)" "" flat "${ops_n}")
    string(REGEX REPLACE "@[^,]*" "" flat "${flat}")
    if(NOT flat MATCHES ",")
        continue()
    endif()
    string(REGEX REPLACE "^.*,[ \t]*" "" dest "${flat}")
    string(STRIP "${dest}" dest)
    _a5_in("${dest}" hit)
    if(hit)
        # A restore from the stack (movem/move (sp)+) puts the frame back.
        if(ops_n MATCHES "^\\(sp\\)\\+|^sp@\\+")
            set(pending "")
        else()
            set(pending "0x${addr_hex}: ${mnem} ${ops} overwrites a5")
            set(pending_exg "")
        endif()
    endif()
endforeach()

if(bad)
    list(JOIN bad "\n  " msg)
    message(FATAL_ERROR
        "check-a5-frame: ${BINARY} overwrites its frame pointer:\n  ${msg}\n"
        "The caller's unlk a5 / rts will return through whatever a5 now holds. "
        "Keep a5 out of register variables; see cmake/check-a5-frame.cmake.")
endif()
