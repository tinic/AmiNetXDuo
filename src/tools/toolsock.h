/*
 * toolsock -- bsdsocket.library through its published vectors, for the
 * commands that are ordinary network applications rather than parts of the
 * stack.
 *
 *   nc and telnet are worked examples as much as they are commands: a
 *   program on somebody else's Amiga will have Roadshow or AmiTCP underneath
 *   it, so none of them may link one line of src/netstack or src/bsdsocket.
 *   They call the library by hand at the LVOs docs/RESEARCH.md 3.2 lists,
 *   exactly as fetch.c does -- and for the same reason the NDK inlines are
 *   not used: those assume a global SocketBase and hide the ABI these
 *   programs exist to demonstrate.
 *
 *   fetch needed eight vectors and open-coded them.  These three need
 *   nineteen between them, including the whole server half -- bind, listen,
 *   accept -- that no client-only tool ever touches, so they are written once
 *   here.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TOOLSOCK_H
#define AMINETXDUO_TOOLSOCK_H

#include "tools.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ the shapes --
 *
 * Open-coded rather than included.  <sys/socket.h> in this toolchain is the
 * socket world's, tools.h has already pulled in NetX Duo's <sys/types.h>, and
 * the two disagree; these four structures are the whole of the ABI surface
 * these commands need and they have not changed since 4.2BSD.
 */

typedef struct ToolSockAddr
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;               /* network order == our order on m68k   */
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
} ToolSockAddr;

typedef struct ToolHostEnt
{
    char   *h_name;
    char  **h_aliases;
    LONG    h_addrtype;
    LONG    h_length;
    char  **h_addr_list;
} ToolHostEnt;

typedef struct ToolServEnt
{
    char   *s_name;
    char  **s_aliases;
    LONG    s_port;                 /* network order, as the ABI says       */
    char   *s_proto;
} ToolServEnt;

typedef struct ToolTimeval
{
    LONG    tv_secs;
    LONG    tv_micro;
} ToolTimeval;

#define TOOL_AF_INET        2
#define TOOL_SOCK_STREAM    1
#define TOOL_SOCK_DGRAM     2
#define TOOL_SOCK_RAW       3

#define TOOL_SOL_SOCKET     0xffff
#define TOOL_SO_REUSEADDR   0x0004
#define TOOL_SO_BROADCAST   0x0020
#define TOOL_SO_ERROR       0x1007

/*
 * <netinet/in.h>'s and <sys/socket.h>'s -- 4.4BSD's, and Roadshow's -- and NOT
 * NetX Duo's addons/BSD layer, which numbers IPPROTO_IP 2 and IP_TTL 26 and is
 * not what this library speaks.  Shared because `ping` and `traceroute` both
 * want them and each had grown its own copy.
 */
#define TOOL_IPPROTO_IP     0
#define TOOL_IPPROTO_ICMP   1
#define TOOL_IP_TOS         3
#define TOOL_IP_TTL         4

#define TOOL_SHUT_RD        0
#define TOOL_SHUT_WR        1
#define TOOL_SHUT_RDWR      2

/* _IOW('f', 126, int) -- non-blocking mode. */
#define TOOL_FIONBIO        0x8004667EUL

/* The errno numbers these commands name.  4.4BSD's, which is what every
   Amiga bsdsocket.library reports (src/bsdsocket/bsdsocket_internal.h). */
#define TOOL_EINTR          4
#define TOOL_EPIPE          32
#define TOOL_EWOULDBLOCK    35
#define TOOL_EINPROGRESS    36
#define TOOL_EADDRINUSE     48
#define TOOL_ENETUNREACH    51
#define TOOL_ECONNRESET     54
#define TOOL_ETIMEDOUT      60
#define TOOL_ECONNREFUSED   61
#define TOOL_EHOSTUNREACH   65

/* What a stack without SOCK_RAW answers socket(AF_INET, SOCK_RAW, ...) with. */
#define TOOL_EPROTONOSUPPORT 43
#define TOOL_ESOCKTNOSUPPORT 44
#define TOOL_EOPNOTSUPP      45
#define TOOL_EAFNOSUPPORT    47

/* ------------------------------------------------------------- fd_set ---- */

/* 256 descriptors, the published maximum of getdtablesize(). */
#define TOOL_FD_WORDS       8

typedef struct ToolFdSet
{
    ULONG w[TOOL_FD_WORDS];
} ToolFdSet;

VOID tool_fd_zero(ToolFdSet *set);
VOID tool_fd_add(ToolFdSet *set, LONG fd);
BOOL tool_fd_isset(const ToolFdSet *set, LONG fd);

/* ---------------------------------------------------------- the library --- */

/*
 * Open bsdsocket.library, which is what starts the network -- the library is
 * self-starting -- and prints a legible explanation when it will not.  NULL
 * on failure, nothing printed on success.
 *
 * A command whose whole job is to use the network has no business refusing to
 * bring it up, which is why this is not tool_require_stack().
 */
struct Library *tool_socket_open(VOID);

/* ------------------------------------------------------------- vectors ---- */

LONG  tool_sock_socket(struct Library *base, LONG domain, LONG type, LONG proto);
LONG  tool_sock_bind(struct Library *base, LONG s, const ToolSockAddr *sa);
LONG  tool_sock_listen(struct Library *base, LONG s, LONG backlog);
LONG  tool_sock_accept(struct Library *base, LONG s, ToolSockAddr *from);
LONG  tool_sock_connect(struct Library *base, LONG s, const ToolSockAddr *sa);
LONG  tool_sock_send(struct Library *base, LONG s, const void *buf, LONG len);
LONG  tool_sock_sendto(struct Library *base, LONG s, const void *buf, LONG len,
                       const ToolSockAddr *to);
LONG  tool_sock_recv(struct Library *base, LONG s, void *buf, LONG len);
LONG  tool_sock_recvfrom(struct Library *base, LONG s, void *buf, LONG len,
                         ToolSockAddr *from);
LONG  tool_sock_shutdown(struct Library *base, LONG s, LONG how);
LONG  tool_sock_setsockopt(struct Library *base, LONG s, LONG level, LONG name,
                           const void *val, LONG len);
LONG  tool_sock_getsockopt(struct Library *base, LONG s, LONG level, LONG name,
                           void *val, LONG *len);
LONG  tool_sock_getsockname(struct Library *base, LONG s, ToolSockAddr *sa);
LONG  tool_sock_ioctl(struct Library *base, LONG s, ULONG req, void *argp);
LONG  tool_sock_close(struct Library *base, LONG s);
LONG  tool_sock_errno(struct Library *base);

/*
 * WaitSelect() with the full set of masks.  Any of the three sets may be
 * NULL, and so may the timeout, which then means "wait forever".  Returns the
 * number of ready descriptors, 0 on timeout, -1 on error.
 */
LONG  tool_sock_select(struct Library *base, LONG nfds, ToolFdSet *readfds,
                       ToolFdSet *writefds, ToolTimeval *tv);

ToolHostEnt *tool_sock_gethostbyname(struct Library *base, const char *name);
ToolServEnt *tool_sock_getservbyname(struct Library *base, const char *name,
                                     const char *proto);

/* ------------------------------------------------------------- helpers ---- */

/* Fill in an address.  `port` is in host order and byte-swapped here if the
   host ever stops being big-endian. */
VOID tool_sock_addr(ToolSockAddr *sa, ULONG address, UWORD port);

/*
 * A dotted quad as itself, anything else through gethostbyname().  Prints the
 * failure and returns FALSE; on success nothing is printed.
 */
BOOL tool_sock_resolve(struct Library *base, const char *host, ULONG *out);

/*
 * A port number, a service name out of DEVS:Internet/services, or 0 after
 * printing why not.  `proto` is "tcp" or "udp".
 */
UWORD tool_sock_port(struct Library *base, const char *text, const char *proto);

/* A sentence, not an errno.  Always returns something printable. */
const char *tool_sock_errstr(LONG err);

/*
 * "cannot connect to 10.0.2.2 port 21: connection refused" -- the standard
 * failure line, so all three commands word it the same way.
 */
VOID tool_sock_fail(struct Library *base, const char *what, ULONG address,
                    UWORD port);

/* ------------------------------------------------------------- console ---- */

/*
 * Standard input, polled.
 *
 * A byte shovel has to watch a socket and a keyboard at once, and WaitSelect()
 * knows nothing about DOS handles.  So the socket is polled with a short
 * timeout and the input side is polled between polls:
 *
 *   * an interactive stream (a Shell window) is put in RAW mode and read one
 *     keystroke at a time behind WaitForChar(), which is the only way to get
 *     character-at-a-time out of the Amiga console;
 *   * anything else -- a file, NIL:, a redirection -- is read in blocks,
 *     because a file read returns immediately and there is nothing to
 *     multiplex with.
 *
 * tool_input_open() must be paired with tool_input_close(): RAW mode outlives
 * the process and a Shell left in it is unusable.
 */
typedef struct ToolInput
{
    BPTR    fh;
    BOOL    interactive;
    BOOL    raw;
    BOOL    eof;
} ToolInput;

VOID tool_input_open(ToolInput *in, BOOL want_raw);
VOID tool_input_close(ToolInput *in);

/*
 * Bytes read, 0 at end of input, -1 when nothing is available yet (only ever
 * on an interactive stream).  Waits at most `micros` microseconds.
 */
LONG tool_input_read(ToolInput *in, UBYTE *buf, LONG len, ULONG micros);

/* Write straight to standard output, unbuffered. -1 on failure. */
LONG tool_output_write(const UBYTE *buf, LONG len);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_TOOLSOCK_H */
