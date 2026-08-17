# Where the commit in every $VER: string comes from.  Script mode, `cmake -P`.
#
# WHY THIS IS NOT IN AmiNetXDuoVersion.cmake
#
# It used to be.  cmake read the hash while it configured, baked it into
# version.h, and never looked again, so an incremental `cmake --build` after a
# `git checkout` stamped the binaries with the commit that had been checked out
# the last time somebody ran cmake.  On the ClassicWB rig a guest reported
# 1e99fb7 while running a later branch, and then reported 8806430 while running
# a merge built on top of that.  The bytes were right both times; the label was
# wrong, and tools/classicwb.sh's version gate exists to prove exactly the
# thing that label was lying about.
#
# So the git query moved to a build step, and this file is what it runs.  It is
# invoked once at configure time as well, so the header exists before anything
# reads it.
#
# HOW IT AVOIDS REBUILDING THE WORLD
#
# It runs on every build, and a stamp that rewrote its header every build would
# be worse than the bug it fixes: <aminetxduo/version.h> is included by every
# command in the tree.  So the header is written to a scratch file and copied
# over the real one with configure_file(COPYONLY), which leaves the destination
# untouched -- same bytes, same mtime -- when the two already match.  An
# unchanged commit therefore compiles and links nothing.
#
# Expects, all -D:
#   GIT_EXECUTABLE   git, or empty when there is none
#   SRC_DIR          the source tree to ask about
#   TEMPLATE         include/aminetxduo/version_git.h.in
#   OUTPUT           the header to write
#   FALLBACK_DATE    d.m.yyyy to use when there is no commit to date
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)

set(AMINETXDUO_VERSION_HASH "")
set(AMINETXDUO_VERSION_DATE "")
set(AMINETXDUO_VERSION_BUILD "")

# EXISTS and not IS_DIRECTORY: in a `git worktree` .git is a FILE holding the
# path to the real git dir, and every agent on this project works in one.
if(GIT_EXECUTABLE AND EXISTS "${SRC_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
        WORKING_DIRECTORY "${SRC_DIR}"
        OUTPUT_VARIABLE AMINETXDUO_VERSION_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    # The date is the commit's, not the clock's, so that two builds of one
    # source say the same thing.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" log -1 --format=%cd --date=format:%-d.%-m.%Y
        WORKING_DIRECTORY "${SRC_DIR}"
        OUTPUT_VARIABLE AMINETXDUO_VERSION_DATE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-list --count HEAD
        WORKING_DIRECTORY "${SRC_DIR}"
        OUTPUT_VARIABLE AMINETXDUO_VERSION_BUILD
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    # -dirty, because a commit named on a binary built from edited sources is
    # the same lie in a smaller size.
    #
    # `status --porcelain`, not `diff-index`: diff-index compares the index's
    # stat cache, so `touch` on an unmodified file reads as a modification and
    # every touch would flip the stamp and recompile everything that carries
    # it.  status re-reads the content and a touch says nothing.
    #
    # --untracked-files=no keeps build directories and scratch files out of it;
    # --ignore-submodules=dirty keeps the walk off the ~20k files in
    # third_party/ while still reporting a submodule moved to another commit,
    # which is a real difference in what got compiled.
    if(NOT AMINETXDUO_VERSION_HASH STREQUAL "")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" status --porcelain
                    --untracked-files=no --ignore-submodules=dirty
            WORKING_DIRECTORY "${SRC_DIR}"
            OUTPUT_VARIABLE _anx_status
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(NOT _anx_status STREQUAL "")
            string(APPEND AMINETXDUO_VERSION_HASH "-dirty")
        endif()
    endif()
endif()

# No commit to take a date from is what an unpacked source tarball looks like.
# The fallback is fixed at configure time rather than read from the clock here,
# so that a tarball build does not rewrite this header, and recompile with it,
# every time the day rolls over.
if(AMINETXDUO_VERSION_DATE STREQUAL "")
    set(AMINETXDUO_VERSION_DATE "${FALLBACK_DATE}")
endif()

configure_file("${TEMPLATE}" "${OUTPUT}.tmp" @ONLY)
configure_file("${OUTPUT}.tmp" "${OUTPUT}" COPYONLY)
file(REMOVE "${OUTPUT}.tmp")
