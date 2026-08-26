/*
 * clients/compat, the whole of "libnet" for a Roadshow-NDK client build.
 *
 * The NDK's <proto/bsdsocket.h> inlines all dereference a global SocketBase,
 * which a configure-time feature test does not define, so its link fails and
 * the feature is reported missing.  Hence a WEAK definition, in an ARCHIVE: it
 * is extracted only when nothing else defines SocketBase, so the real link,
 * where the client defines its own, never sees a duplicate.
 *
 * SPDX-License-Identifier: MIT
 */

struct Library;

__attribute__((weak)) struct Library *SocketBase = 0;
