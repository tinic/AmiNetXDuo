/*
 * toolsock, bsdsocket.library through its published vectors, for the
 * commands that are network applications rather than parts of the stack.
 *
 * nc and telnet must run on an Amiga with Roadshow or AmiTCP underneath, so
 * they link no part of src/netstack or src/bsdsocket. They call the library by
 * hand at the LVOs docs/RESEARCH.md 3.2 lists, as fetch.c does. The NDK inlines
 * are not used because they assume a global SocketBase and hide the ABI these
 * programs demonstrate.
 *
 * fetch open-codes the eight vectors it needs. These three need nineteen
 * between them, including the server half, bind, listen, accept, that no
 * client-only tool touches, so they are written once here.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TOOLSOCK_H
#define AMINETXDUO_TOOLSOCK_H

#include "tools.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ the shapes,
 *
 * Open-coded rather than included: <sys/socket.h> in this toolchain is the
 * socket world's, tools.h has already pulled in NetX Duo's <sys/types.h>, and
 * the two disagree. These four structures are the whole ABI surface these
 * commands need, unchanged since 4.2BSD.
 */

typedef struct ToolSockAddr
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;               /* network order == our order on m68k   */
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
} ToolSockAddr;

/*
 * The AF_INET6 shape, as this NDK spells it: no sin6_len, so byte 0 is the
 * family. src/bsdsocket/in6.c pins every offset with _Static_assert and
 * bsd_sa_family() reads byte 0 plus the declared length, which is why the two
 * structs can share the union below without ambiguity. A sockaddr_in's byte 0
 * is its length, 16, and never 23.
 */
typedef struct ToolSockAddr6
{
    UBYTE   sin6_family;
    UBYTE   sin6_pad;
    UWORD   sin6_port;
    ULONG   sin6_flowinfo;
    UBYTE   sin6_addr[16];
    ULONG   sin6_scope_id;
} ToolSockAddr6;

typedef union ToolSockAddrAny
{
    ToolSockAddr    in;
    ToolSockAddr6   in6;
} ToolSockAddrAny;

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

/*
 * struct addrinfo, from the NDK's netdb.h. Open-coded for the same reason the
 * sockaddrs above are: tools.h has already pulled in NetX Duo's headers and
 * including netdb.h here starts a fight over socklen_t.
 */
typedef struct ToolAddrInfo
{
    LONG                    ai_flags;
    LONG                    ai_family;
    LONG                    ai_socktype;
    LONG                    ai_protocol;
    LONG                    ai_addrlen;
    ToolSockAddrAny        *ai_addr;
    char                   *ai_canonname;
    struct ToolAddrInfo    *ai_next;
} ToolAddrInfo;

#define TOOL_AI_NUMERICHOST 4

/*
 * The two getaddrinfo() verdicts this file acts on, from netdb.h:189.
 * EAI_NONAME is worth a second question.  EAI_AGAIN means the lookup was never
 * answered, and asking twice only doubles the wait.
 */
#define TOOL_EAI_NONAME    -2
#define TOOL_EAI_AGAIN     -3

#define TOOL_AF_UNSPEC      0
#define TOOL_AF_INET        2
#define TOOL_AF_INET6      23
#define TOOL_SOCK_STREAM    1
#define TOOL_SOCK_DGRAM     2
#define TOOL_SOCK_RAW       3

#define TOOL_SOL_SOCKET     0xffff
#define TOOL_SO_REUSEADDR   0x0004
#define TOOL_SO_BROADCAST   0x0020
#define TOOL_SO_ERROR       0x1007

/*
 * <netinet/in.h>'s and <sys/socket.h>'s, 4.4BSD's, and Roadshow's, not
 * NetX Duo's addons/BSD layer, which numbers IPPROTO_IP 2 and IP_TTL 26 and is
 * not what this library speaks.  Shared by `ping` and `traceroute`.
 */
#define TOOL_IPPROTO_IP     0
#define TOOL_IPPROTO_ICMP   1
#define TOOL_IP_TOS         3
#define TOOL_IP_TTL         4

#define TOOL_SHUT_RD        0
#define TOOL_SHUT_WR        1
#define TOOL_SHUT_RDWR      2

/* _IOW('f', 126, int), non-blocking mode. */
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
#define TOOL_EISCONN        56
#define TOOL_ETIMEDOUT      60
#define TOOL_ECONNREFUSED   61
#define TOOL_EHOSTUNREACH   65

/* IPv6, for the commands that build their own ICMPv6. */
#define TOOL_IPPROTO_ICMPV6     58
#define TOOL_IPPROTO_IPV6       41
/* Both numberings are accepted by this library.  See bsdsocket_internal.h. */
#define TOOL_IPV6_UNICAST_HOPS   4
#define TOOL_IPV6_TCLASS        61

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
 * Open bsdsocket.library, which starts the network because the library is
 * self-starting, and print an explanation when it does not open.  NULL on
 * failure, nothing printed on success.
 *
 * Not tool_require_stack(): these commands exist to use the network, so they
 * bring it up.
 */
struct Library *tool_socket_open(VOID);

/* ------------------------------------------------------------- vectors ---- */

LONG  tool_sock_socket(struct Library *base, LONG domain, LONG type, LONG proto);
LONG  tool_sock_bind(struct Library *base, LONG s, const ToolSockAddrAny *sa);
LONG  tool_sock_listen(struct Library *base, LONG s, LONG backlog);
LONG  tool_sock_accept(struct Library *base, LONG s, ToolSockAddrAny *from);
LONG  tool_sock_connect(struct Library *base, LONG s, const ToolSockAddrAny *sa);
LONG  tool_sock_send(struct Library *base, LONG s, const void *buf, LONG len);
LONG  tool_sock_send_full(struct Library *base, LONG s, const void *buf,
                          LONG len);
LONG  tool_sock_sendto(struct Library *base, LONG s, const void *buf, LONG len,
                       const ToolSockAddrAny *to);
LONG  tool_sock_recv(struct Library *base, LONG s, void *buf, LONG len);
LONG  tool_sock_recvfrom(struct Library *base, LONG s, void *buf, LONG len,
                         ToolSockAddrAny *from);
LONG  tool_sock_shutdown(struct Library *base, LONG s, LONG how);
LONG  tool_sock_setsockopt(struct Library *base, LONG s, LONG level, LONG name,
                           const void *val, LONG len);
LONG  tool_sock_getsockopt(struct Library *base, LONG s, LONG level, LONG name,
                           void *val, LONG *len);
LONG  tool_sock_getsockname(struct Library *base, LONG s, ToolSockAddrAny *sa);
LONG  tool_sock_ioctl(struct Library *base, LONG s, ULONG req, void *argp);
LONG  tool_sock_close(struct Library *base, LONG s);
LONG  tool_sock_errno(struct Library *base);

/*
 * WaitSelect() with the full set of masks.  Any of the three sets can be
 * NULL, and so can the timeout, which then means no ceiling at all.  Returns
 * the number of ready descriptors, 0 on timeout, -1 on error.
 */
LONG  tool_sock_select(struct Library *base, LONG nfds, ToolFdSet *readfds,
                       ToolFdSet *writefds, ToolTimeval *tv);

/*
 * The same, with WaitSelect()'s signal mask -- in and out, and the signals it
 * reports have been cleared from the task.  For a caller that has to watch a
 * MsgPort as well as its sockets: the port's signal belongs in the same wait,
 * or the two end up polled against each other.
 */
LONG  tool_sock_select_sigs(struct Library *base, LONG nfds, ToolFdSet *readfds,
                            ToolFdSet *writefds, ToolTimeval *tv,
                            ULONG *sigmask);

ToolHostEnt *tool_sock_gethostbyname(struct Library *base, const char *name);
ToolServEnt *tool_sock_getservbyname(struct Library *base, const char *name,
                                     const char *proto);

/*
 * getaddrinfo() and friends, at the far end of the vector table (-0x324
 * upwards). Not every bsdsocket.library has them, so tool_sock_have_lvo()
 * below is asked first and the answer decides whether tool_sock_resolve()
 * uses them or falls back to gethostbyname().
 */
LONG  tool_sock_getaddrinfo(struct Library *base, const char *node,
                            const char *service, const ToolAddrInfo *hints,
                            ToolAddrInfo **res);
VOID  tool_sock_freeaddrinfo(struct Library *base, ToolAddrInfo *ai);
char *tool_sock_ntop(struct Library *base, LONG af, const void *src,
                     char *dst, LONG size);

/*
 * TRUE when the library's vector table reaches `lvo`, given as the positive
 * magnitude of the negative offset (0x32a for getaddrinfo). A library that
 * stops short has (APTR)-1 there, and jumping to it is a guru.
 */
BOOL  tool_sock_have_lvo(struct Library *base, ULONG lvo);

/* TRUE when this library will make an AF_INET6 socket. */
BOOL  tool_sock_have_ipv6(struct Library *base);

/* ------------------------------------------------------------- helpers ---- */

/*
 * A resolved endpoint, either family. The v4 address is in host order because
 * that is what ami_config_format_ip() and the rest of the tools use.  The v6
 * address is the sixteen wire bytes, because nothing here interprets them.
 */
typedef struct ToolAddr
{
    UWORD   ta_Family;              /* TOOL_AF_INET or TOOL_AF_INET6        */
    UWORD   ta_Pad;
    ULONG   ta_V4;
    UBYTE   ta_V6[16];
    ULONG   ta_ScopeId;
} ToolAddr;

/* "fe80::280:10ff:fe32:3334" and a NUL, with room for a scope suffix. */
#define TOOL_ADDR_STRLEN    64

#define TOOL_ADDR_IS6(a)    ((a)->ta_Family == (UWORD)TOOL_AF_INET6)

VOID tool_addr_v4(ToolAddr *addr, ULONG v4);
VOID tool_addr_any(ToolAddr *addr, LONG family);

/*
 * Fill in a sockaddr and return the length to hand the library, which is 16
 * for AF_INET and 28 for AF_INET6.  `port` is in host order and byte-swapped
 * here if the host ever stops being big-endian.
 */
LONG tool_sock_addr(ToolSockAddrAny *sa, const ToolAddr *addr, UWORD port);
LONG tool_sock_addr_v4(ToolSockAddrAny *sa, ULONG v4, UWORD port);

BOOL tool_addr_same(const ToolAddr *a, const ToolAddr *b);

/* The other direction, for what accept() and recvfrom() write back. */
BOOL  tool_sock_addr_get(const ToolSockAddrAny *sa, ToolAddr *out);
UWORD tool_sock_addr_port(const ToolSockAddrAny *sa);
BOOL  tool_sock_addr_same(const ToolSockAddrAny *a, const ToolSockAddrAny *b);

/* Printable, both families.  Never fails, and writes "?" if it cannot. */
VOID tool_addr_text(struct Library *base, const ToolAddr *addr,
                    char *buf, ULONG buflen);
VOID tool_sock_addr_text(struct Library *base, const ToolSockAddrAny *sa,
                         char *buf, ULONG buflen);

/*
 * A literal of either family as itself, anything else through the library's
 * own getaddrinfo() with AF_UNSPEC, so an IPv6 answer is preferred when the
 * machine has IPv6 and the name has an AAAA.  Prints the failure and returns
 * FALSE.  On success nothing is printed.
 *
 * `want` is TOOL_AF_UNSPEC, or one family to insist on.  Insisting says which
 * of the two failures happened: "has no IPv6 address" when the name resolves
 * in the other family, "cannot resolve" when it resolves in neither.  That
 * costs a second lookup, made only on the failure path.
 */
BOOL tool_sock_resolve_af(struct Library *base, const char *host, LONG want,
                          ToolAddr *out);
BOOL tool_sock_resolve(struct Library *base, const char *host, ToolAddr *out);

/*
 * Four, which covers a dual-stack name with a pair of records in each family.
 * A longer answer is truncated: the point is to have a second address to try
 * when the first one is unreachable, not to walk a whole round robin.
 */
#define TOOL_ADDR_TRIES     4

typedef struct ToolAddrList
{
    ULONG       count;
    ToolAddr    addr[TOOL_ADDR_TRIES];
} ToolAddrList;

/*
 * The same lookup, keeping every address instead of the first.  The order is
 * the resolver's, IPv6 ahead of IPv4, and `want` still pins absolutely: -6
 * yields AAAAs and nothing else.  Failures are worded and printed exactly as
 * tool_sock_resolve_af() words them, which is the same code.
 */
BOOL tool_sock_resolve_list(struct Library *base, const char *host, LONG want,
                            ToolAddrList *out);

/*
 * `IPV4=-4/S,IPV6=-6/S` out of a ReadArgs array and into a `want`.  Every
 * client command carries the pair, so the refusal when both are given is
 * worded once here rather than ten times.  FALSE after printing it.
 */
BOOL tool_arg_family(LONG four, LONG six, LONG *want);

/*
 * TRUE when `host` has no address of `want` but does have one of the other
 * family, so a caller that did its own getaddrinfo() can tell a missing AAAA
 * from a missing name.  Costs a lookup, so ask only after one has already
 * failed, and only when it failed with a verdict rather than a timeout.
 */
BOOL tool_sock_family_absent(struct Library *base, const char *host, LONG want);

/* "<host> has no IPv6 address, and -6 was given", worded once. */
VOID tool_sock_say_no_family(const char *host, LONG want);

/*
 * Strip one layer of brackets from an IPv6 literal written the URL way.
 * "[::1]" becomes "::1", and anything else is returned unchanged.  The result
 * points either into `host` or into `buf`.
 */
const char *tool_host_unbracket(const char *host, char *buf, ULONG buflen);

/*
 * A port number, a service name out of DEVS:Internet/services, or 0 after
 * printing why not.  `proto` is "tcp" or "udp".
 */
UWORD tool_sock_port(struct Library *base, const char *text, const char *proto);

/* A sentence, not an errno.  Always returns something printable. */
const char *tool_sock_errstr(LONG err);

/*
 * "cannot connect to 10.0.2.2 port 21: connection refused", the standard
 * failure line, so all three commands word it the same way.
 *
 * `what` is the whole verb phrase and carries its own preposition: "connect
 * to", "reach".  It is pasted straight into "cannot <what> <address> port
 * <n>: <reason>", so "connect" on its own prints "cannot connect 10.0.2.2
 * port 21", which is not a sentence.
 */
VOID tool_sock_fail(struct Library *base, const char *what,
                    const ToolAddr *addr, UWORD port);

/*
 * The same sentence with the errno given rather than read back.  A caller
 * that has closed the failed socket already cannot use the one above: the
 * close is itself a library call and overwrites what it would report.
 */
VOID tool_sock_fail_why(struct Library *base, const char *what,
                        const ToolAddr *addr, UWORD port, LONG err);

/* ---------------------------------------------------------- connecting --- */

/*
 * connect(), with a ceiling on how long it can take.
 *
 * The plain call blocks for the stack's whole SYN schedule, 191 s on ours,
 * and a command with a timeout of its own cannot cut that short.  With
 * `timeout` the socket goes non-blocking, the connect returns EINPROGRESS,
 * and WaitSelect() watches the write set.  SO_ERROR then says whether the
 * handshake finished or failed.  0 means the ceiling is the stack's.
 *
 * 0 on success, -1 on failure with the errno in *why, TOOL_CONNECT_TIMEDOUT
 * when the ceiling was reached.  Nothing is printed.
 */
#define TOOL_CONNECT_TIMEDOUT   (-2)

LONG tool_sock_connect_timed(struct Library *base, LONG s,
                             const ToolSockAddrAny *sa, ULONG timeout,
                             LONG *why);

/* What tool_sock_connect_host() is being asked for. */
typedef struct ToolConnect
{
    LONG    family;         /* TOOL_AF_UNSPEC, or the one -4/-6 pinned  */
    LONG    socktype;       /* TOOL_SOCK_STREAM or TOOL_SOCK_DGRAM      */
    UWORD   port;
    UWORD   localport;      /* 0, or a local port to bind first         */
    ULONG   timeout;        /* seconds, 0 for the stack's own ceiling   */
    BOOL    announce;       /* "Trying <address> port <n>..." per try   */
} ToolConnect;

#define TOOL_CONNECT_NORESOLVE  (-3)    /* the resolver has already said why */
#define TOOL_CONNECT_NOSOCKET   (-4)
#define TOOL_CONNECT_NOBIND     (-5)
#define TOOL_CONNECT_FAILED     (-6)

/*
 * Resolve `host` and connect to the first of its addresses that answers.
 *
 * A name with an AAAA and an A gets both tried, so a machine on a network
 * whose IPv6 goes nowhere still reaches the service.  Every address but the
 * last is given a short trial first; the last gets what remains.
 *
 * `timeout` bounds the call and not each address in it: what the short trials
 * did not use goes to the last address, and if the list then still has budget
 * to spare, the addresses that were cut short are tried again out of it, so a
 * slow path is not abandoned because a later address failed fast.  A caller
 * that set no timeout has nothing to divide and gets no second round.
 *
 * The socket on success, else one of the negative codes above, with *chosen
 * the address that failed and *why its errno.  Only the resolver prints.
 */
LONG tool_sock_connect_host(struct Library *base, const char *host,
                            const ToolConnect *how, ToolAddr *chosen,
                            LONG *why);

/* ------------------------------------------------------------- console ---- */

/*
 * Standard input, polled.  Watching a socket and a keyboard at once means
 * polling the socket with a short timeout and the input side between polls,
 * because WaitSelect() knows nothing about DOS handles:
 *
 *   * an interactive stream (a Shell window) is put in RAW mode and read one
 *     keystroke at a time behind WaitForChar(), the only way to get
 *     character-at-a-time out of the Amiga console,
 *   * anything else, a file, NIL:, a redirection, is read in blocks, since
 *     a file read returns immediately.
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
