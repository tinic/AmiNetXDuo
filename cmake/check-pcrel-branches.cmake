# Fail the build unless every 32-bit PC-relative relocation in the linked image
# holds exactly target + addend - field address, and every other byte of every
# input code section reached the image unchanged.  tools/check-pcrel-exact.py
# reads the input objects (archive members and LTO ltrans objects included)
# through the linker map; exit 2, something it could not check, also fails.
#
# Inputs: PYTHON3, TOOL, BINARY, MAPFILE, DIRS (where relative map paths
# resolve, the link directory first), KEEP_TEMPS.  The LTO temporaries next to
# BINARY are removed after a pass unless KEEP_TEMPS, and kept after a failure.
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)

foreach(_v PYTHON3 TOOL BINARY MAPFILE)
    if(NOT ${_v})
        message(FATAL_ERROR "check-pcrel-branches: ${_v} not set")
    endif()
endforeach()

string(REPLACE "|" ";" _dirs "${DIRS}")
execute_process(COMMAND "${PYTHON3}" "${TOOL}" "${BINARY}" "${MAPFILE}" ${_dirs}
                OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
string(STRIP "${_out}${_err}" _all)
get_filename_component(_name "${BINARY}" NAME)

if(NOT _rc EQUAL 0)
    message("${_all}")
    if(_rc EQUAL 1)
        message(FATAL_ERROR
            "${_name}: the linker wrote a PC-relative field that does not hold "
            "target + addend - address, or changed a byte of an input code "
            "section.  This is a toolchain defect; see docs/RESEARCH.md §25.")
    endif()
    message(FATAL_ERROR
        "${_name}: tools/check-pcrel-exact.py could not check every relocation "
        "(exit ${_rc}); an unchecked image is refused.")
endif()

message("${_all}")

if(KEEP_TEMPS)
    return()
endif()
file(GLOB _temps "${BINARY}.ltrans*" "${BINARY}.wpa.*" "${BINARY}.res"
                 "${BINARY}.lto_wrapper_args")
if(_temps)
    file(REMOVE ${_temps})
endif()
