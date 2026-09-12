# Turn compiler warnings into build failures, for OUR sources only.
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
#   Filtering by target does not work either, `threadx`, `netxduo`,
#   `netxduo_addons` and `crypto68k_ref` are declared in OUR CMakeLists.txt
#   files but compile vendored sources.  So the filter is per SOURCE FILE:
#   every source whose path contains /third_party/ is left alone, everything
#   else gets the flags.  Nothing has to be kept in a list, so a new component
#   is covered the day it is added.
#
# The work is deferred to the end of the top-level directory because targets do
# not exist yet at the point this file is included, add_subdirectory() has
# not run.  set_property(SOURCE ... TARGET_DIRECTORY ...) is what makes it
# legal to reach into a target declared in another directory.
#
# SPDX-License-Identifier: MIT

option(AMINETXDUO_WERROR "Fail the build on any warning in our own sources" ON)

# -Wmissing-prototypes is here so a function that is not static has to say what
# it is somewhere a caller can see.  It found 34: eight that were only ever used
# in their own file and are static now, one dead accessor, two asm-facing entry
# points nothing declared, a C fallback a macro renamed out from under its own
# prototype, and a handful of files that simply did not include the header
# already declaring what they defined -- config_advice.c defined ami_cfg_advice()
# without including the header that tells callers its shape.
#
# It is not a size lever.  Measured across the whole change: bsdsocket.library
# 339,468 -> 339,476 bytes, +8.  Single-unit LTO already saw everything, so
# static unlocked no internalization the linker did not have.  What it buys is
# the compiler checking every definition against what callers were told.
set(AMINETXDUO_WARNING_FLAGS "-Wall;-Wextra" CACHE STRING
    "Warning flags applied to sources outside third_party/")

# SHIPPING SOURCES ONLY -- src/ and port/, not tests/.
#
# -Wmissing-prototypes says a function that is not static has to declare itself
# somewhere a caller can see.  That is exactly right for code that ships, and
# it found real things: a definition whose header was never included
# (config_advice.c defined ami_cfg_advice() without including config.h, so
# nothing checked it against what every caller is told), a C fallback a macro
# had renamed out from under its own prototype, and eight functions never used
# outside their own file.
#
# It is NOT right for tests.  tests/fuzz/fuzz_dns.c DEFINES
# _tx_thread_system_suspend() to stand in for ThreadX's, and there is no header
# it could be declared in that would not be a forgery of upstream's.  A test
# that impersonates a symbol on purpose is not the bug this flag looks for, and
# the findings there are per-configuration noise: each CI arm compiles a
# different set of stubs.
set(AMINETXDUO_SHIPPING_WARNING_FLAGS "-Wmissing-prototypes" CACHE STRING
    "Warning flags applied to src/ and port/ only")

# Per-file escapes, as <path fragment> <extra flags> pairs.  Every entry is a
# bug someone has to fix, so each one says what it is; this list should shrink.
#
# IT IS EMPTY, and the last entry to leave is worth recording, because it did
# not leave for the reason its own note gave.
#
# src/config/test/test_config.c held -Wno-error=address for CHECK_STR's
# `(got) ? (got) : "(null)"` printf argument, which is -Waddress when `got` is
# an array.  That form was replaced by an or_null() helper at some point after
# the escape was written, and nobody took the escape back out: measured on gcc
# 14.2 with this file's own flags, the ternary form gives 7 -Werror=address and
# the or_null form gives none.  SO THE ESCAPE HAD BEEN DEAD, and an escape that
# is dead is worse than one that is needed -- it is a hole nothing is watching,
# ready for the next real warning in that file to fall through silently.
#
# CHECK_STR is a function now, so the null guard is expressed once instead of
# at 132 expansion sites.  That does not make the guard fire for array callers;
# nothing can, an array is not null.  It means the compiler is not asked to
# prove the same tautology 132 times, which is the thing that needed an escape.
#
# Adding an entry here is fine.  Leaving a dead one is not: check that the
# warning still fires before assuming an entry is load-bearing.
set(AMINETXDUO_WARNING_EXEMPT)

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
        set(_shipping "")
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
            # tests/atf/ holds FreeBSD's tests/sys/netinet sources byte for
            # byte, under their own BSD-2-Clause header, so the same rule
            # applies: not ours, not modifiable, not warning-clean against the
            # Roadshow NDK's prototypes (sendto() takes APTR where FreeBSD's
            # takes const void *).  The shim beside them -- atf-c.h,
            # atf_main.c, atf-prelude.h -- is ours and is not exempt.
            if(_s MATCHES "/tests/atf/" AND NOT _s MATCHES "/tests/atf/atf")
                continue()
            endif()
            list(APPEND _ours "${_s}")
            # src/<component>/test/ holds host tests, which stub Exec --
            # Forbid(), Disable(), AddSemaphore() -- and there is no header
            # those could be declared in that is not a forgery of exec.library.
            # They live under src/ but they do not ship.
            if((_s MATCHES "/src/" OR _s MATCHES "/port/")
               AND NOT _s MATCHES "/test/")
                list(APPEND _shipping "${_s}")
            endif()
        endforeach()

        if(_ours)
            # APPEND, not set_source_files_properties(): src/crypto68k already
            # puts COMPILE_OPTIONS on its two .S files and overwriting them
            # would drop the -m68020 the assembler needs.
            set_property(SOURCE ${_ours} TARGET_DIRECTORY ${_t}
                         APPEND PROPERTY COMPILE_OPTIONS ${_flags})

            # The shipping-only half: src/ and port/, never tests/.
            if(_shipping AND AMINETXDUO_SHIPPING_WARNING_FLAGS)
                set_property(SOURCE ${_shipping} TARGET_DIRECTORY ${_t}
                             APPEND PROPERTY COMPILE_OPTIONS
                             ${AMINETXDUO_SHIPPING_WARNING_FLAGS})
            endif()

            # ... then the escapes, which have to come after to win.
            list(LENGTH AMINETXDUO_WARNING_EXEMPT _n)
            math(EXPR _last "${_n} / 2 - 1")
            if(_n GREATER 0)
                foreach(_i RANGE ${_last})
                    math(EXPR _pi "${_i} * 2")
                    math(EXPR _fi "${_i} * 2 + 1")
                    list(GET AMINETXDUO_WARNING_EXEMPT ${_pi} _pat)
                    list(GET AMINETXDUO_WARNING_EXEMPT ${_fi} _extra)
                    foreach(_s IN LISTS _ours)
                        if(_s MATCHES "${_pat}$")
                            set_property(SOURCE "${_s}" TARGET_DIRECTORY ${_t}
                                         APPEND PROPERTY COMPILE_OPTIONS ${_extra})
                        endif()
                    endforeach()
                endforeach()
            endif()
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
