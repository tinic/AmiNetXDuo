# Turn compiler warnings into build failures -- for OUR sources only.
#
# Usage (no edit to any CMakeLists.txt is needed; CMake includes this file at
# the end of the top-level project() call):
#
#   cmake -S . -B build -DCMAKE_PROJECT_INCLUDE=cmake/ci-warnings.cmake
#
# Override the flag set with -DAMINETXDUO_WARNING_FLAGS="-Wall;-Wextra", and
# turn the whole thing off again with -DAMINETXDUO_WERROR=OFF.
#
# WHY IT IS NOT JUST add_compile_options(-Wall -Wextra -Werror)
#
#   Most of what this project compiles is not this project: ThreadX, NetX Duo,
#   nx_crypto and nx_secure are vendored verbatim as submodules and are not
#   warning-clean under -Wextra.  A global flag would therefore fail the build
#   on code we have a standing rule never to modify.
#
#   Filtering by target does not work either -- `threadx`, `netxduo`,
#   `netxduo_addons` and `crypto68k_ref` are declared in OUR CMakeLists.txt
#   files but compile vendored sources.  So the filter is per SOURCE FILE:
#   every source whose path contains /third_party/ is left alone, everything
#   else gets the flags.  Nothing has to be kept in a list, so a new component
#   is covered the day it is added.
#
# The work is deferred to the end of the top-level directory because targets do
# not exist yet at the point this file is included -- add_subdirectory() has
# not run.  set_property(SOURCE ... TARGET_DIRECTORY ...) is what makes it
# legal to reach into a target declared in another directory.
#
# SPDX-License-Identifier: MIT

option(AMINETXDUO_WERROR "Fail the build on any warning in our own sources" ON)

set(AMINETXDUO_WARNING_FLAGS "-Wall;-Wextra" CACHE STRING
    "Warning flags applied to sources outside third_party/")

function(_aminetxduo_warnings_apply_dir dir)

    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)

    set(_flags ${AMINETXDUO_WARNING_FLAGS})
    if(AMINETXDUO_WERROR)
        list(APPEND _flags "-Werror")
    endif()

    foreach(_t IN LISTS _targets)

        # INTERFACE and UTILITY targets have no sources to compile.
        get_target_property(_type ${_t} TYPE)
        if(_type STREQUAL "INTERFACE_LIBRARY" OR _type STREQUAL "UTILITY")
            continue()
        endif()

        get_target_property(_srcs ${_t} SOURCES)
        get_target_property(_sdir ${_t} SOURCE_DIR)
        if(NOT _srcs)
            continue()
        endif()

        set(_ours "")
        foreach(_s IN LISTS _srcs)
            if(NOT IS_ABSOLUTE "${_s}")
                set(_s "${_sdir}/${_s}")
            endif()
            # Generator expressions and generated sources are skipped: they are
            # not ours to police and cannot be path-matched reliably.
            if(_s MATCHES "\\$<")
                continue()
            endif()
            if(_s MATCHES "/third_party/")
                continue()
            endif()
            list(APPEND _ours "${_s}")
        endforeach()

        if(_ours)
            # APPEND, not set_source_files_properties(): src/crypto68k already
            # puts COMPILE_OPTIONS on its two .S files and overwriting them
            # would drop the -m68020 the assembler needs.
            set_property(SOURCE ${_ours} TARGET_DIRECTORY ${_t}
                         APPEND PROPERTY COMPILE_OPTIONS ${_flags})
        endif()

    endforeach()

    foreach(_d IN LISTS _subdirs)
        _aminetxduo_warnings_apply_dir("${_d}")
    endforeach()

endfunction()

function(_aminetxduo_warnings_apply)
    _aminetxduo_warnings_apply_dir("${CMAKE_SOURCE_DIR}")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
               CALL _aminetxduo_warnings_apply)

string(REPLACE ";" " " _aminetxduo_wflags "${AMINETXDUO_WARNING_FLAGS}")
message(STATUS "AmiNetXDuo warnings gate: ${_aminetxduo_wflags}"
               " (-Werror ${AMINETXDUO_WERROR})")
