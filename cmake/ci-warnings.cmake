# Turn compiler warnings into build failures, for OUR sources only.
#
# Included at the end of the top-level project() call.  Every preset sets
# CMAKE_PROJECT_INCLUDE to this file (CMakePresets.json, hidden `amiga` base),
# so there is no way to configure one of this project's own drawers without the
# gate.  tools/ci.sh passes the same option explicitly; that is redundant and
# kept only because the ci.sh host arm configures a directory no preset
# describes.
#
# Override with -DAMINETXDUO_WARNING_FLAGS="-Wall;-Wextra", turn the whole
# thing off with -DAMINETXDUO_WERROR=OFF.
#
# Filtering is per SOURCE FILE, not per target: most of what this project
# compiles is not this project (the ThreadX, NetX Duo, nx_crypto and nx_secure
# submodules are vendored verbatim and are not warning-clean under -Wextra),
# while targets like `threadx` and `netxduo` are declared in OUR CMakeLists.txt
# and compile vendored sources.  Paths containing /third_party/ are left alone;
# nothing has to be kept in a list.
#
# The work is deferred to the end of the top-level directory because targets do
# not exist yet when this file is included.  set_property(SOURCE ...
# TARGET_DIRECTORY ...) is what makes reaching into another directory legal.
#
# SPDX-License-Identifier: MIT

option(AMINETXDUO_WERROR "Fail the build on any warning in our own sources" ON)

# -Wmissing-prototypes: a function that is not static has to say what it is
# where a caller can see it.  Found 34.  Not a size lever -- single-unit LTO
# already saw everything, so static unlocked nothing the linker did not have;
# what it buys is the compiler checking every definition against what callers
# were told.
#
# -Wstrict-prototypes is NOT here, deliberately, and what replaced it is a hard
# error in the language rather than a warning here.
#
# It was added for the mixing hazard -mregparm introduces: `VOID f();' declares
# no prototype, so `f(a, b)' travels on the stack while a definition compiled
# for -mregparm=3 reads registers, and the callee returns a wrong number.  Two
# things make the flag the wrong tool.  It cannot fire where the hazard exists:
# the m68k compiler is GCC 16 and compiles as C23 (__STDC_VERSION__ 202311L with
# no -std given), where `()' MEANS `(void)' and `f(a, b)' is "too many arguments
# to function" -- a hard error for a direct call and for a call through a
# function pointer alike.  And where it does fire it cannot be satisfied: a full
# host build (gcc 14, C17) reports 26 diagnostics from just three sites, none of
# them ours to change -- the NDK's own `VOID (*putChProc)()' in exec_protos.h,
# which forces the cast at src/bsdsocket/loghook.c:163; the NDK-impersonating
# src/netdev/test/shim/exec/interrupts.h; and the vendored
# third_party/netxduo/nx_secure/inc/nx_secure_tls_api.h.
#
# The guarantee is therefore asserted where it is relied on:
# include/aminetxduo/asm_main.h #errors unless the m68k build is C23 or later.
# That covers a hand-written compile line too, which no flag here would.
#
# -Werror=implicit-function-declaration: C23 makes it an error anyway; naming it
# stops a future -std from taking that away quietly.
#
# MEASURED 2026-09-30, full host build with -Werror on (gcc 14.2, C17):
# 0 -Wmissing-prototypes from shipping sources, 0 -Wcast-function-type, 0
# implicit-function-declaration.  Everything -Wmissing-prototypes finds is a
# test, which is why it is off for them below.
set(AMINETXDUO_WARNING_FLAGS
    "-Wall;-Wextra;-Werror=implicit-function-declaration;-Wmissing-prototypes"
    CACHE STRING "Warning flags applied to sources outside third_party/")

# NON-SHIPPING -- anything not under src/ or port/, plus the host tests under
# src/<component>/test/.
#
# -Wmissing-prototypes is right for code that ships and wrong for tests.  A test
# that impersonates a symbol on purpose (tests/fuzz/fuzz_dns.c DEFINES
# _tx_thread_system_suspend()) is not the bug the warning looks for, and the
# findings are per-configuration noise: each CI arm compiles a different set of
# stubs.  One rule, not an entry per file.
#
# -Wno-cast-function-type used to be here, for the five tests that install a Hook
# by casting one -- the NDK declares `h_Entry' as ULONG (*)(VOID) while a Hook
# function is really ULONG (*)(struct Hook *, APTR, APTR).  It is gone because
# it has no subject left: main replaced all five casts with union punning
# (4607a0c6, test_netmon_host.c and its siblings now assign through a `HookEntry'
# union), no shipping source ever cast h_Entry -- src/ only READS it -- and a
# full host build with the escape removed reports 0 -Wcast-function-type.  An
# escape nothing can trip is a hole nothing is watching; see the note on
# AMINETXDUO_WARNING_EXEMPT below.
set(AMINETXDUO_NONSHIPPING_WARNING_FLAGS
    "-Wno-missing-prototypes"
    CACHE STRING "Warning flags applied to sources that do not ship")

# Per-file escapes, as <path fragment> <extra flags> pairs.  Each entry is a bug
# somebody has to fix, so it says what it is; this list should shrink.  IT IS
# EMPTY: the last entry, -Wno-error=address for src/config/test/test_config.c,
# had been dead -- the ternary it was written for was replaced by an or_null()
# helper and nobody took the escape out.  A dead escape is worse than a needed
# one, a hole nothing is watching.  Check a warning still fires before assuming
# an entry is load-bearing.
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
        set(_nonshipping "")
        foreach(_s IN LISTS _srcs)
            if(NOT IS_ABSOLUTE "${_s}")
                set(_s "${_sdir}/${_s}")
            endif()
            # Generator expressions and generated sources cannot be
            # path-matched reliably, so they are not policed.
            if(_s MATCHES "\\$<")
                continue()
            endif()
            if(_s MATCHES "/third_party/")
                continue()
            endif()
            # tests/atf/ holds FreeBSD's tests/sys/netinet sources byte for
            # byte under their own BSD-2-Clause header, so the same rule
            # applies: not ours, not modifiable, not warning-clean against the
            # Roadshow NDK's prototypes (sendto() takes APTR where FreeBSD's
            # takes const void *).  That is tcp_socket.c and whatever lands
            # beside it, so the exemption names the files that are OURS
            # instead of guessing from a filename prefix.  The shim is atf-c.h,
            # atf-prelude.h and atf_main.c, and it is not exempt.
            if(_s MATCHES "/tests/atf/"
               AND NOT _s MATCHES "/tests/atf/(atf-c\\.h|atf-prelude\\.h|atf_main\\.c)$")
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
            else()
                list(APPEND _nonshipping "${_s}")
            endif()
        endforeach()

        if(_ours)
            # APPEND, not set_source_files_properties(): src/crypto68k already
            # puts COMPILE_OPTIONS on its two .S files and overwriting those
            # would drop the -m68020 the assembler needs.
            set_property(SOURCE ${_ours} TARGET_DIRECTORY ${_t}
                         APPEND PROPERTY COMPILE_OPTIONS ${_flags})

            # Everything that does not ship gets the two flags that only make
            # sense for shipping code turned back off.  Set after the main list
            # so the -Wno- wins.
            if(_nonshipping AND AMINETXDUO_NONSHIPPING_WARNING_FLAGS)
                set_property(SOURCE ${_nonshipping} TARGET_DIRECTORY ${_t}
                             APPEND PROPERTY COMPILE_OPTIONS
                             ${AMINETXDUO_NONSHIPPING_WARNING_FLAGS})
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
