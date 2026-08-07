# Assert that bsdsocket.library still begins with "moveq #-1,d0 / rts".
#
#   cmake -DLIBRARY_BINARY=<file> -P cmake/check-library-entry.cmake
#
# WHY THIS EXISTS
#
# A library file on this system is an ordinary loadable segment, and nothing
# stops somebody typing its name at a Shell.  src/bsdsocket/library.c opens
# with the two instructions that make that harmless -- return -1, return --
# and the invariant is that they land at offset 0 of the first code hunk.
#
# Nothing enforces it.  It holds because library.c is first in BSDSOCKET_SOURCES
# and the linker keeps object order, which is a property of a list nobody is
# looking at.  AMINETXDUO_UNITY made that worse: inside a unity translation
# unit GCC chooses the emission order, so `library.c first` stops meaning
# `library.c's code first` and the file starts with whatever GCC felt like.
# The option keeps library.c out of the unity group for exactly this reason,
# and this check is what proves the reason still applies.
#
# The failure it catches is not a crash in a test.  It is a machine that
# executes an arbitrary function from the middle of a TCP stack because
# somebody typed the wrong thing at a Shell.
#
# SPDX-License-Identifier: MIT

if(NOT DEFINED LIBRARY_BINARY)
    message(FATAL_ERROR
            "check-library-entry.cmake: -DLIBRARY_BINARY=<file> required")
endif()

file(READ "${LIBRARY_BINARY}" _hex HEX)
string(LENGTH "${_hex}" _hexlen)
math(EXPR _longs "${_hexlen} / 8")

# A longword by index, as lowercase hex.
function(_lw out idx)
    math(EXPR _at "${idx} * 8")
    string(SUBSTRING "${_hex}" ${_at} 8 _v)
    set(${out} "${_v}" PARENT_SCOPE)
endfunction()

_lw(_magic 0)
if(NOT _magic STREQUAL "000003f3")
    message(FATAL_ERROR
            "${LIBRARY_BINARY}: not a HUNK executable (first longword "
            "0x${_magic}, expected 0x000003f3)")
endif()

# HUNK_HEADER: the resident library name list, terminated by a zero longword.
# Normally empty, so this walks nothing, but a name list is legal here and
# skipping it wrong would put the whole parse one field out.
set(_i 1)
while(_i LESS _longs)
    _lw(_n ${_i})
    if(_n STREQUAL "00000000")
        math(EXPR _i "${_i} + 1")
        break()
    endif()
    # Length in longwords, then that many longwords of name.
    math(EXPR _skip "0x${_n} + 1")
    math(EXPR _i "${_i} + ${_skip}")
endwhile()

# table_size, first_hunk, last_hunk, then one size longword per hunk.
_lw(_first ${_i})
math(EXPR _i "${_i} + 1")
_lw(_last ${_i})
math(EXPR _i "${_i} + 1")
_lw(_lastn ${_i})
math(EXPR _i "${_i} + 1")
math(EXPR _count "0x${_lastn} - 0x${_last} + 1")
math(EXPR _i "${_i} + ${_count}")

# The first hunk block.  It must be code: a library whose first hunk is data
# has already lost the property this checks.
_lw(_kind ${_i})
if(NOT _kind STREQUAL "000003e9")
    message(FATAL_ERROR
            "${LIBRARY_BINARY}: first hunk is 0x${_kind}, not HUNK_CODE "
            "(0x000003e9).  The executable entry stub is no longer first.")
endif()

# kind, length, then the code.
math(EXPR _i "${_i} + 2")
math(EXPR _at "${_i} * 8")
string(SUBSTRING "${_hex}" ${_at} 8 _entry)

# moveq #-1,d0 (0x70ff) ; rts (0x4e75)
if(NOT _entry STREQUAL "70ff4e75")
    get_filename_component(_name "${LIBRARY_BINARY}" NAME)
    message(FATAL_ERROR
        "${_name} does not begin with \"moveq #-1,d0 / rts\".\n"
        "First code hunk starts 0x${_entry}, expected 0x70ff4e75.\n"
        "library.c is no longer the first object on the link line, or it has\n"
        "been pulled into a unity translation unit where GCC picks the\n"
        "emission order.  Typing the library's name at a Shell now runs\n"
        "whatever function landed there instead.")
endif()
