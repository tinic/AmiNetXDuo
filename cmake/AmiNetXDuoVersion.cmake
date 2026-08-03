# AmiNetXDuo version: our own, compounded with the stack we are built on.
#
# There is one product version -- the VERSION argument to project() in the
# root CMakeLists.txt -- and nothing else in the tree may spell one out.  This
# module turns that number, plus the NetX Duo and ThreadX versions READ OUT OF
# the submodule headers, into the strings the build, the distribution archive
# and CI all quote.
#
# Nothing here touches the AmigaOS library ABI.  bsdsocket.library's
# lib_Version is 4 because 4 is the AmiTCP/Roadshow API level that
# OpenLibrary("bsdsocket.library", 4) asks for; it is not a product version
# and it is not this module's business.  Same for usergroup.library.
#
# WHY THE SUBMODULE VERSIONS ARE READ AND NOT WRITTEN
#
# An artefact called "0.1.0+nx6.5.1" is a promise about what is inside it.  If
# third_party/netxduo is bumped and the compound string keeps saying 6.5.1,
# every .lha and every CI artefact from that point on is lying about its own
# contents, and nothing in the build notices.  So the versions come from
# nx_api.h and tx_api.h, and the pins below are checked against them: a
# submodule bump fails the configure step with the two lines to edit rather
# than shipping a stale label.
#
# SPDX-License-Identifier: MIT

# ------------------------------------------------------------------ pins ----
#
# What third_party/ is expected to contain.  These are the ONLY hardcoded
# upstream version numbers in the tree.  Bumping a submodule means editing the
# matching line here, in the same commit, on purpose.
set(AMINETXDUO_NETXDUO_VERSION_PIN "6.5.1")
set(AMINETXDUO_THREADX_VERSION_PIN "6.5.1")

# --------------------------------------------------------------- reading ----

# Pull MAJOR/MINOR/PATCH out of a vendored api header and join them.
#
#   _aminetxduo_read_version(<out> <header> <prefix>)
#
# <prefix> is the macro stem: NETXDUO for NETXDUO_MAJOR_VERSION and friends,
# THREADX for THREADX_MAJOR_VERSION.  A header that does not define all three
# is a hard error -- upstream renaming the macros must not silently degrade
# into an empty version string.
function(_aminetxduo_read_version out header prefix)
    if(NOT EXISTS "${header}")
        message(FATAL_ERROR
            "AmiNetXDuo version: ${header} is missing.\n"
            "  Run: git submodule update --init --recursive")
    endif()

    set(parts "")
    foreach(field MAJOR MINOR PATCH)
        file(STRINGS "${header}" line
             REGEX "^[ \t]*#define[ \t]+${prefix}_${field}_VERSION[ \t]+[0-9]+")
        # Checked before list(GET), which errors out on an empty list with a
        # cmake diagnostic instead of the one below.
        if(NOT line)
            message(FATAL_ERROR
                "AmiNetXDuo version: ${prefix}_${field}_VERSION not found in\n"
                "  ${header}\n"
                "Upstream has renamed its version macros; teach\n"
                "  cmake/AmiNetXDuoVersion.cmake the new spelling.")
        endif()
        list(GET line 0 line)       # first match wins; there is only one
        string(REGEX REPLACE
               "^[ \t]*#define[ \t]+${prefix}_${field}_VERSION[ \t]+([0-9]+).*$"
               "\\1" value "${line}")
        list(APPEND parts "${value}")
    endforeach()

    list(JOIN parts "." joined)
    set(${out} "${joined}" PARENT_SCOPE)
endfunction()

# A header that changes must re-run configure, or the pin check only fires on
# a clean build and a `git submodule update` followed by `cmake --build` walks
# straight past it.
function(_aminetxduo_watch header)
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}"
                 APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${header}")
endfunction()

# The check itself.  Loud, and it names the line to edit.
function(_aminetxduo_check_pin what found pinned header)
    if(NOT found STREQUAL pinned)
        message(FATAL_ERROR
            "AmiNetXDuo version: ${what} has moved.\n"
            "  recorded : ${pinned}   (AMINETXDUO_${what}_VERSION_PIN)\n"
            "  submodule: ${found}   (${header})\n"
            "\n"
            "The compound version this build stamps on its libraries, its\n"
            ".lha and its CI artefacts would name a ${what} that is not the\n"
            "one being compiled.  Set the pin in\n"
            "  cmake/AmiNetXDuoVersion.cmake\n"
            "to ${found} in the same commit as the submodule bump.")
    endif()
endfunction()

set(_nx_api "${AMINETXDUO_NETXDUO}/common/inc/nx_api.h")
set(_tx_api "${AMINETXDUO_THREADX}/common/inc/tx_api.h")

_aminetxduo_watch("${_nx_api}")
_aminetxduo_watch("${_tx_api}")

_aminetxduo_read_version(AMINETXDUO_NETXDUO_VERSION "${_nx_api}" NETXDUO)
_aminetxduo_read_version(AMINETXDUO_THREADX_VERSION "${_tx_api}" THREADX)

_aminetxduo_check_pin(NETXDUO "${AMINETXDUO_NETXDUO_VERSION}"
                              "${AMINETXDUO_NETXDUO_VERSION_PIN}" "${_nx_api}")
_aminetxduo_check_pin(THREADX "${AMINETXDUO_THREADX_VERSION}"
                              "${AMINETXDUO_THREADX_VERSION_PIN}" "${_tx_api}")

# ------------------------------------------------------------- compounds ----

set(AMINETXDUO_VERSION "${PROJECT_VERSION}")

# Machine-facing: goes in filenames, artefact names and tags-adjacent places.
# SemVer build metadata, so "0.1.0+nx6.5.1" sorts and compares as 0.1.0.
set(AMINETXDUO_VERSION_COMPOUND
    "${AMINETXDUO_VERSION}+nx${AMINETXDUO_NETXDUO_VERSION}")

# Human-facing: what a person reads in a .readme header or a --version.
set(AMINETXDUO_VERSION_LONG
    "AmiNetXDuo ${AMINETXDUO_VERSION} (NetX Duo ${AMINETXDUO_NETXDUO_VERSION}, ThreadX ${AMINETXDUO_THREADX_VERSION})")

# The short commit, for the $VER: strings. A release tag pins the version, but
# a build from a working tree does not -- and "which 0.16.1 is this" is exactly
# the question a user tracking an installation asks. Empty outside a checkout.
set(AMINETXDUO_VERSION_HASH "")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE AMINETXDUO_VERSION_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()

# AmigaOS $VER: wants a d.m.yyyy date after the version, and `Version` prints
# it. Taken from the last commit rather than the clock so two builds of the
# same source say the same thing; falls back to the configure date outside a
# checkout, which is what an unpacked source tarball is.
set(AMINETXDUO_VERSION_DATE "")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" log -1 --format=%cd --date=format:%-d.%-m.%Y
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE AMINETXDUO_VERSION_DATE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
if(AMINETXDUO_VERSION_DATE STREQUAL "")
    string(TIMESTAMP AMINETXDUO_VERSION_DATE "%d.%m.%Y")
endif()

# The commit count, by the project's convention, appears in a binary's version
# output and nowhere else -- not in the tag, not in the release name, not in
# the archive filename.  Snapshot at configure time; empty outside a checkout,
# which is what an unpacked source tarball looks like.
set(AMINETXDUO_VERSION_BUILD "")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-list --count HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE AMINETXDUO_VERSION_BUILD
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()

# --------------------------------------------------------------- header -----

configure_file(
    "${CMAKE_SOURCE_DIR}/include/aminetxduo/version.h.in"
    "${CMAKE_BINARY_DIR}/include/aminetxduo/version.h"
    @ONLY)
