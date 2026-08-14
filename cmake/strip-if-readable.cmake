# Strip a binary, and do not stop the build if the toolchain cannot read it.
#
# WHY THIS EXISTS
#
# binutils 2.46's BFD amiga backend cannot read a hunk EXECUTABLE that contains
# a HUNK_DEBUG block.  2.39, which the pinned Linux toolchain carries, reads it
# and strips it out.  Measured on both, with the same objects:
#
#   2.39   strip removes HUNK_SYMBOL and HUNK_DEBUG          38632 bytes
#   2.46   strip: "file format not recognized", build stops  45732 bytes
#
# It is the file it refuses, not the operation: objcopy --strip-all gives the
# same answer, and neither -Wl,-s nor -Wl,--strip-debug stops ld emitting the
# debug hunk in the first place.  Almost every binary in this tree has no
# HUNK_DEBUG and strips fine on both; the profiler's two do have one, and what
# puts it there is not any of -Map, .bss, or an assembly object -- each was
# tested alone and none reproduces it.
#
# So the choice is between failing the build on a toolchain we intend to ship
# with, and shipping two developer binaries with their symbols on.  This takes
# the second and SAYS SO, because a silent `|| true` is how a strip that
# stopped working everywhere would go unnoticed.
#
# SPDX-License-Identifier: MIT

if(NOT EXISTS "${BINARY}")
    message(FATAL_ERROR "strip-if-readable: no ${BINARY}")
endif()

execute_process(COMMAND "${STRIP}" "${BINARY}"
                RESULT_VARIABLE rc
                OUTPUT_VARIABLE out
                ERROR_VARIABLE err)

if(NOT rc EQUAL 0 OR err MATCHES "file format not recognized")
    get_filename_component(_name "${BINARY}" NAME)
    message(WARNING
        "strip-if-readable: ${STRIP} would not read ${_name}, so it keeps its "
        "symbols.  binutils 2.46 refuses a hunk executable carrying a "
        "HUNK_DEBUG block; 2.39 strips it.  The binary is correct either way "
        "-- AmigaDOS ignores the extra hunks -- it is only larger.")
endif()
