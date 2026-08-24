/*
 * What commit this was built from, GENERATED ON EVERY BUILD.
 * Edit include/aminetxduo/version_git.h.in.
 *
 * Separate from <aminetxduo/version.h> because these three are the only
 * version values that can change without cmake being re-run.  The product
 * version and the NetX Duo and ThreadX versions come out of files cmake
 * already watches, so a change to them re-configures; the commit does not,
 * and an incremental build after a `git checkout` used to stamp binaries with
 * whatever had been checked out when cmake last ran.
 *
 * cmake/AmiNetXDuoGitStamp.cmake writes this, and rewrites it only when the
 * contents differ, so an unchanged commit costs nothing.  <version.h> includes
 * it; nothing else should need to.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_VERSION_GIT_H
#define AMINETXDUO_VERSION_GIT_H

/*
 * Short commit, so two builds of the same release can be told apart, with
 * "-dirty" appended when tracked files differ from it.  Empty outside a
 * checkout, which is what an unpacked source tarball is.
 */
#define AMINETXDUO_VERSION_HASH     "2ab83e1-dirty"

/*
 * Commit count.  By project convention this may appear in a binary's version
 * output and nowhere else -- not in a tag, not in a release name, not in an
 * archive filename.  Empty outside a checkout.
 */
#define AMINETXDUO_VERSION_BUILD    "3239"

/* d.m.yyyy, for the $VER: strings AmigaOS Version reads.  The commit's date,
   falling back to the configure date outside a checkout. */
#define AMINETXDUO_VERSION_DATE     "24.8.2026"

#endif /* AMINETXDUO_VERSION_GIT_H */
