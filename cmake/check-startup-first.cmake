# Assert that a linked AmigaOS command begins with src/tools/tool_startup.S.
#
#   cmake -DTOOL_MAP=<file>.map -P cmake/check-startup-first.cmake
#
# WHY THIS EXISTS
#
# The commands are linked -nostartfiles, so the entry point is ours
# (tool_startup.S) rather than the toolchain's crt0.o.  AmigaOS enters a
# LoadSeg'd command at the first byte of the first hunk; the m68k-amigaos
# linker script carries no ENTRY(), and its .text output section is
#
#     .text : { __stext = .; *(.text) *(.text.main) *(.text*) ... }
#
# so which object lands at address 0 is decided by the order the objects appear
# on the link line, and that in turn by the order of add_executable()'s source
# list.  Nothing in CMake promises that ordering, and every command's own
# object also has a non-empty plain .text: with -ffunction-sections the string
# literals stay there, and there is no .rodata on this target for them to go to.
#
# A command whose first hunk started with a string pool would be entered on
# whatever those bytes disassemble to.  There is no build-time symptom and no
# test that would fail; it simply gurus on the guest.  So the map is read back.
#
# SPDX-License-Identifier: MIT

if(NOT DEFINED TOOL_MAP)
    message(FATAL_ERROR "check-startup-first.cmake: -DTOOL_MAP=<file> required")
endif()

file(STRINGS "${TOOL_MAP}" _lines)

# The first line after `*(.text)` that names an input contribution of non-zero
# size.  ld writes the section, the address, the size and the object on one
# line, or splits the section onto a line of its own when the name is long --
# only the short form can appear here, since the section is exactly ".text".
set(_seen_rule OFF)
set(_first "")
foreach(_line IN LISTS _lines)
    if(NOT _seen_rule)
        if(_line MATCHES "^ \\*\\(\\.text\\)$")
            set(_seen_rule ON)
        endif()
        continue()
    endif()
    if(_line MATCHES "^ \\.text +0x[0-9a-fA-F]+ +0x0*([1-9a-fA-F][0-9a-fA-F]*) +(.+)$")
        set(_first "${CMAKE_MATCH_2}")
        break()
    endif()
    # Any other input-section rule means *(.text) contributed nothing at all.
    if(_line MATCHES "^ \\*\\(")
        break()
    endif()
endforeach()

if(NOT _seen_rule)
    message(FATAL_ERROR
        "check-startup-first: ${TOOL_MAP} has no *(.text) rule.  This is not a\n"
        "linker map for the m68k-amigaos default script, so nothing was checked.")
endif()

if(NOT _first MATCHES "tool_startup")
    get_filename_component(_name "${TOOL_MAP}" NAME)
    message(FATAL_ERROR
        "${_name}: the first thing in the executable is\n"
        "    ${_first}\n"
        "and not tool_startup.S.  The command is entered at hunk 0 offset 0, so\n"
        "it would run whatever that object begins with.  tool_startup.S must be\n"
        "the first source named in add_executable().")
endif()
