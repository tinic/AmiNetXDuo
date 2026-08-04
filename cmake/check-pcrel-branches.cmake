# Fail the build on a 32-bit PC-relative branch that does not land on a function.
#
# WHAT THIS CATCHES, AND WHY IT IS A POST-LINK CHECK
#
# This toolchain mis-resolves a 32-bit PC-relative relocation (RELRELOC32)
# against a LOCAL SECTION SYMBOL, the shape produced by a tail call from one
# -ffunction-sections section to another section of the SAME object.  The
# assembler leaves a non-zero addend in the displacement field and the linker
# adds it a second time, so the branch lands twelve bytes short of its target:
# in the MIDDLE of the preceding function, at whatever instruction boundary
# happens to sit there.
#
# Nothing before the link can see it.  The object's relocation is well formed
# and the object's disassembly shows only a placeholder; the wrong number
# exists solely in the linked image.  Hence POST_BUILD, on the image.
#
# `ping` rebooted an A1200 on exactly this, and did it silently: six boots
# for a two-command list, which from the outside reads as a hang.
# docs/RESEARCH.md §25.
#
# THE INVARIANT
#
# A 68020 `bra.l`/`bsr.l` (opcode 60FF / 61FF) is emitted by this compiler only
# for a call or a tail call, so the target is always a function entry, an
# address that appears in the symbol table.  A branch that lands anywhere else
# is a mis-resolved relocation.  Cross-OBJECT tail calls relocate against a
# global symbol with a zero addend and come out right, which is why the great
# many of those in bsdsocket.library pass this check unchanged.
#
# Inputs: BINARY, OBJDUMP, NM.
#
# SPDX-License-Identifier: MIT

# Run with `cmake -P`, which starts with no policies set: IN_LIST below is an
# `if` OPERATOR only under CMP0057, and without this the script fails with
# "Unknown arguments specified" on CMake 3.31 while passing on 4.x.
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

# Walk the disassembly.  objdump decodes 60FF/61FF for a plain 68000, so it
# prints them as a two-byte short branch and then carries on decoding the
# 32-bit displacement as if it were code.  The mnemonics below the branch are
# therefore meaningless, what matters is the BYTE COLUMN, which is contiguous
# and in address order, so the displacement is simply the next two words.
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

# DATA READ AS CODE.
#
# On m68k-amigaos there is no .rodata: string literals live in the plain .text
# (docs/RESEARCH.md 46).  objdump disassembles .text linearly, so it renders
# those strings, and the tables beside them, as instructions, and a table
# entry whose bytes are 61 FF at an even address, first on an objdump line, is
# indistinguishable from a bsr.l here.
#
# Not hypothetical: the configuration name table ("bootp\0auto\0...\0none\0")
# and the lookup array after it produced exactly that, and adding one warning
# string elsewhere in the library shifted the bytes onto an even address and
# started failing the build.
#
# The first discriminator is that the m68k fetches instructions on even
# addresses ONLY.  A computed target that is odd cannot be a branch destination
# on any 68k, so the bytes were data.
#
# The second is range: a target outside the disassembled code cannot be a
# branch destination either.  The 68k has no code there to reach.  A switch
# dispatch table in bsdsocket.library hit exactly this, {UWORD case, ULONG
# target} pairs whose second half read as 60FF at an even line start, giving a
# 393216-byte displacement to an address past the end of the image.
#
# The real defect this check exists for lands twelve bytes short of a function
#, even, and inside the code, so neither rule loses anything it was written
# to catch.
#
# Attributing the data to a symbol does not help: objdump counts it as part of
# whatever function precedes it, so the run has no boundary to detect.

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
            # The displacement has to be read from the two words that FOLLOW
            # the opcode, and objdump stops disassembling at a section end
            # ("Address ... is out of bounds"), so the words are not always
            # there.  Reading whatever came next instead would invent a target;
            # abandon this one rather than report a number nobody can trust.
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
            # An instruction START, which is why only the first word of a line
            # qualifies: the same bit pattern occurs constantly as an immediate
            # operand in the middle of a longer instruction, and objdump's line
            # boundaries are the only thing that says which is which.
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
    message(FATAL_ERROR
        "${_name}: a 32-bit PC-relative branch does not land on a function.\n"
        "This is the mis-resolved relocation described in "
        "cmake/check-pcrel-branches.cmake and docs/RESEARCH.md §25; the usual "
        "cause is -ffunction-sections without -fno-optimize-sibling-calls. "
        "Running this binary jumps into the middle of another function.")
endif()
