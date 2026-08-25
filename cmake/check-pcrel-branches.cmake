# Fail the build on a 32-bit PC-relative branch that does not land on a function.
# Function entries are nm UNION the linker map: an Amiga hunk symbol table holds no
# locals, so a correct tail call to a static needs MAPFILE or this fails closed.
#
# Inputs: BINARY, OBJDUMP, NM, MAPFILE (optional).
#
# SPDX-License-Identifier: MIT

# cmake -P starts with no policies set; IN_LIST needs CMP0057.
cmake_minimum_required(VERSION 3.20)

if(NOT EXISTS "${BINARY}")
    message(FATAL_ERROR "check-pcrel-branches: no such file: ${BINARY}")
endif()

if(NOT OBJDUMP OR NOT NM)
    # A check that quietly does nothing is worse than no check: say so.
    message(WARNING "check-pcrel-branches: no objdump/nm, ${BINARY} unchecked")
    return()
endif()

execute_process(COMMAND "${NM}" "${BINARY}"
                OUTPUT_VARIABLE nm_out ERROR_VARIABLE nm_err RESULT_VARIABLE nm_rc)
execute_process(COMMAND "${OBJDUMP}" -d "${BINARY}"
                OUTPUT_VARIABLE dis_out ERROR_VARIABLE dis_err RESULT_VARIABLE dis_rc)

if(NOT nm_rc EQUAL 0 OR NOT dis_rc EQUAL 0)
    message(WARNING "check-pcrel-branches: could not read ${BINARY}")
    return()
endif()

# Every code symbol address, in decimal, so the comparison needs no hex
# formatting, CMake has no printf.
set(code_addrs "")
string(REPLACE "\n" ";" nm_lines "${nm_out}")
foreach(line IN LISTS nm_lines)
    if(line MATCHES "^([0-9a-fA-F]+) [Tt] ")
        math(EXPR a "0x${CMAKE_MATCH_1}")
        list(APPEND code_addrs "${a}")
    endif()
endforeach()

# Input sections start at column one, output sections at column zero; a long name
# wraps its address onto the next line.  Requiring a second 0x<size> keeps symbol
# lines out, and the gate on the header keeps the discarded listing out.
set(map_addrs "")
set(map_seen 0)
if(MAPFILE AND EXISTS "${MAPFILE}")
    file(READ "${MAPFILE}" map_out)
    string(REPLACE ";" "\\;" map_out "${map_out}")
    string(REPLACE "\n" ";" map_lines "${map_out}")
    set(in_map 0)
    set(pending "")
    foreach(line IN LISTS map_lines)
        if(NOT in_map)
            if(line MATCHES "^Linker script and memory map")
                set(in_map 1)
            endif()
            continue()
        endif()
        if(pending)
            if(line MATCHES "^[ \t]+0x0*([0-9a-fA-F]+)[ \t]+0x[0-9a-fA-F]+")
                math(EXPR a "0x0${CMAKE_MATCH_1}")
                list(APPEND map_addrs "${a}")
            endif()
            set(pending "")
        elseif(line MATCHES "^ (\\.text[^ \t]*)[ \t]+0x0*([0-9a-fA-F]+)[ \t]+0x[0-9a-fA-F]+")
            math(EXPR a "0x0${CMAKE_MATCH_2}")
            list(APPEND map_addrs "${a}")
        elseif(line MATCHES "^ (\\.text[^ \t]*)[ \t]*$")
            set(pending "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    list(LENGTH map_addrs map_seen)
    list(APPEND code_addrs ${map_addrs})
    list(REMOVE_DUPLICATES code_addrs)
endif()

# objdump decodes 60FF/61FF as a short branch for a plain 68000 and then decodes
# the displacement as code, so the mnemonics are meaningless: read the BYTE
# COLUMN, which is contiguous and in address order.
string(REPLACE "\n" ";" dis_lines "${dis_out}")
set(want 0)          # words still needed to complete a displacement
set(words "")
set(bad "")

# The highest address objdump printed, for the second data discriminator
# below.  One pre-pass, because the walk needs the answer from its first line.
set(max_addr 0)
foreach(line IN LISTS dis_lines)
    if(line MATCHES "^ +([0-9a-fA-F]+):\t")
        math(EXPR a "0x${CMAKE_MATCH_1}")
        if(a GREATER max_addr)
            set(max_addr "${a}")
        endif()
    endif()
endforeach()

# DATA READ AS CODE: there is no .rodata here, so objdump renders literals and
# tables as instructions and 60FF/61FF occurs in them.  An odd target cannot be
# fetched by a 68k and one past the image has no code, so both are data.

foreach(line IN LISTS dis_lines)
    # Byte column: "   5acc:\t60ff           \tbras ..."
    if(NOT line MATCHES "^ +([0-9a-fA-F]+):\t([0-9a-fA-F][0-9a-fA-F 	]*)")
        continue()
    endif()
    set(line_addr "${CMAKE_MATCH_1}")
    string(REGEX MATCHALL "[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]"
           line_words "${CMAKE_MATCH_2}")

    math(EXPR word_at "0x${line_addr}")
    set(first_on_line 1)
    foreach(w IN LISTS line_words)
        if(want GREATER 0 AND NOT word_at EQUAL expect_at)
            # objdump stops at a section end, so the two words after the opcode
            # are not always there.  Abandon rather than invent a target.
            set(want 0)
            set(words "")
        endif()
        if(want GREATER 0)
            list(APPEND words "${w}")
            math(EXPR want "${want} - 1")
            math(EXPR expect_at "${expect_at} + 2")
            if(want EQUAL 0)
                list(GET words 0 hi)
                list(GET words 1 lo)
                math(EXPR disp "0x${hi} * 65536 + 0x${lo}")
                if(disp GREATER 2147483647)
                    math(EXPR disp "${disp} - 4294967296")
                endif()
                math(EXPR target "${branch_at} + 2 + (${disp})")
                math(EXPR odd "${target} % 2")
                if(odd EQUAL 1)
                    # Data, not an instruction, see the note above.
                elseif(target LESS 0 OR target GREATER max_addr)
                    # Ditto: nothing to branch to out there.
                elseif(NOT target IN_LIST code_addrs)
                    list(APPEND bad
                         "0x${branch_hex}: branch by ${disp} bytes lands inside a function, not on one")
                endif()
                set(words "")
            endif()
        elseif(first_on_line AND w MATCHES "^6[01][fF][fF]$")
            # Only the first word of a line is an instruction START; the same bit
            # pattern is constantly an immediate inside a longer instruction.
            set(branch_hex "${line_addr}")
            math(EXPR branch_at "0x${line_addr}")
            set(want 2)
            set(words "")
            math(EXPR expect_at "${branch_at} + 2")
        endif()
        set(first_on_line 0)
        math(EXPR word_at "${word_at} + 2")
    endforeach()
endforeach()

if(bad)
    message("")
    foreach(b IN LISTS bad)
        message("  ${b}")
    endforeach()
    get_filename_component(_name "${BINARY}" NAME)
    list(LENGTH code_addrs _entries)
    if(map_seen GREATER 0)
        set(_ground "${_entries} function entries known, ${map_seen} of them\
 from ${MAPFILE}")
    else()
        set(_ground "${_entries} function entries known, ALL FROM nm -- no\
 usable linker map, so a static or LTO-internalised callee cannot be\
 recognised and this may be a false refusal; see the header of\
 cmake/check-pcrel-branches.cmake")
    endif()
    message(FATAL_ERROR
        "${_name}: a 32-bit PC-relative branch does not land on a function.\n"
        "This is the mis-resolved relocation described in "
        "cmake/check-pcrel-branches.cmake and docs/RESEARCH.md §25; the usual "
        "cause is -ffunction-sections without -fno-optimize-sibling-calls. "
        "Running this binary jumps into the middle of another function.\n"
        "(${_ground}.)")
endif()
