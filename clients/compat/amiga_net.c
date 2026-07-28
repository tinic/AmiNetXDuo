/*
 * clients/compat -- the whole of "libnet" for a Roadshow-NDK client build.
 *
 * Every Unix client's build system, curl's included, expects `-lnet` on
 * AmigaOS, because bebbo's amiga-gcc ships a libnet.a holding C wrappers
 * (socket(), connect(), recv(), ...) that call through bsdsocket.library.
 * This toolchain has no libnet.a and does not need the wrappers:
 * <proto/bsdsocket.h> from the Roadshow NDK is a set of inline stubs that jump
 * straight to the library vectors.  So `-lnet` has one job left, and it is
 * this file.
 *
 * The NDK inlines all dereference a global `SocketBase`.  A client's own
 * sources define it (curl does, in lib/amigaos.c); a configure-time feature
 * test does not.  cmake's check_symbol_exists("IoctlSocket", ...) compiles a
 * five-line program, links it, and reports "not available" when the link fails
 * on an undefined SocketBase.  That is how curl's build silently loses
 * HAVE_IOCTLSOCKET_CAMEL_FIONBIO, the only way it knows how to put a socket
 * into non-blocking mode on this platform.
 *
 * Hence a weak definition, in an archive.  A member of an archive is pulled in
 * only to satisfy an undefined symbol, so during feature detection this
 * supplies SocketBase, and during the real link -- where the client has
 * already defined it -- the member is never extracted and there is no
 * duplicate.  Weak as well as archived covers the client that defines it in a
 * translation unit the linker sees later.
 *
 * SPDX-License-Identifier: MIT
 */

struct Library;

__attribute__((weak)) struct Library *SocketBase = 0;
