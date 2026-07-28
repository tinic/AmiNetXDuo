/*
 * toolsock -- bsdsocket.library through its published vectors.
 *
 * See toolsock.h for why these are called by hand rather than through the NDK
 * inlines.  Every stub below is written the same way: the arguments go into
 * the registers the ABI names, a6 holds the library base, and the call is a
 * `jsr a6@(-LVO:W)`.  The LVOs are docs/RESEARCH.md 3.2's and the register
 * assignments are src/bsdsocket/bsdsocket_vectors.h's; a disagreement between
 * the two is a bug, and shows up here.
 *
 * Hazard: every stub must declare d1/a0/a1 written.  An AmigaOS library call
 * clobbers d0, d1, a0 and a1, and GCC may assume an input-only operand is left
 * alone, so a stub passing an argument in d1 without declaring d1 written lets
 * GCC keep a value there across the `jsr` and then reuse or spill whatever the
 * library left behind.  This turned IoctlSocket(FIONBIO) into a call with a
 * garbage request code and wedged a test for a day (docs/RESEARCH.md 42).  The
 * `_clob_*` dummies bound to those registers and listed as outputs are the
 * NDK's own idiom, from inline/macros.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"

/* ------------------------------------------------------------- fd_set ---- */

VOID tool_fd_zero(ToolFdSet *set)
{
    ULONG i;

    for (i = 0; i < (ULONG)TOOL_FD_WORDS; i++)
        set->w[i] = 0;
}

VOID tool_fd_add(ToolFdSet *set, LONG fd)
{
    if (fd < 0 || fd >= (LONG)(TOOL_FD_WORDS * 32))
        return;

    set->w[(ULONG)fd / 32UL] |= 1UL << ((ULONG)fd % 32UL);
}

BOOL tool_fd_isset(const ToolFdSet *set, LONG fd)
{
    if (fd < 0 || fd >= (LONG)(TOOL_FD_WORDS * 32))
        return FALSE;

    return (set->w[(ULONG)fd / 32UL] & (1UL << ((ULONG)fd % 32UL))) != 0
               ? TRUE : FALSE;
}

/* ---------------------------------------------------------- the library --- */

struct Library *tool_socket_open(VOID)
{
    struct Library *base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);

    if (base == NULL)
    {
        if (tool_stack_installed())
            tool_error("the network would not start");
        else
            tool_error("there is no bsdsocket.library on this machine");
        tool_explain_no_stack();
    }

    return base;
}

/* ------------------------------------------------------------- vectors ---- */

/* LVO -0x01e */
LONG tool_sock_socket(struct Library *base, LONG domain, LONG type, LONG proto)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = domain;
    register LONG            d1  __asm("d1") = type;
    register LONG            d2  __asm("d2") = proto;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-30:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2)
                      : "a0", "a1", "cc", "memory");
    return res;
}

/* LVO -0x024 */
LONG tool_sock_bind(struct Library *base, LONG s, const ToolSockAddr *sa)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)sa;
    register LONG            d1  __asm("d1") = (LONG)sizeof(*sa);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-36:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

/* LVO -0x02a */
LONG tool_sock_listen(struct Library *base, LONG s, LONG backlog)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            d1  __asm("d1") = backlog;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-42:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1)
                      : "a0", "a1", "cc", "memory");
    return res;
}

/* LVO -0x030 */
LONG tool_sock_accept(struct Library *base, LONG s, ToolSockAddr *from)
{
    LONG namelen = (LONG)sizeof(*from);

    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = (APTR)from;
    register APTR            a1  __asm("a1") = (APTR)&namelen;
    register LONG            res __asm("d0");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-48:W)"
                      : "=r" (res), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "d1", "cc", "memory");
    return res;
}

/* LVO -0x036 */
LONG tool_sock_connect(struct Library *base, LONG s, const ToolSockAddr *sa)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)sa;
    register LONG            d1  __asm("d1") = (LONG)sizeof(*sa);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-54:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

/* LVO -0x03c */
LONG tool_sock_sendto(struct Library *base, LONG s, const void *buf, LONG len,
                      const ToolSockAddr *to)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = 0;
    register CONST_APTR      a1  __asm("a1") = (CONST_APTR)to;
    register LONG            d3  __asm("d3") = (LONG)sizeof(*to);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-60:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2),
                        "r" (a1), "r" (d3)
                      : "cc", "memory");
    return res;
}

/* LVO -0x042 */
LONG tool_sock_send(struct Library *base, LONG s, const void *buf, LONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = 0;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-66:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

/* LVO -0x048 */
LONG tool_sock_recvfrom(struct Library *base, LONG s, void *buf, LONG len,
                        ToolSockAddr *from)
{
    LONG namelen = (LONG)sizeof(*from);

    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = (APTR)buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = 0;
    register APTR            a1  __asm("a1") = (APTR)from;
    register APTR            a2  __asm("a2") = (APTR)&namelen;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-72:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2),
                        "r" (a1), "r" (a2)
                      : "cc", "memory");
    return res;
}

/* LVO -0x04e */
LONG tool_sock_recv(struct Library *base, LONG s, void *buf, LONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = (APTR)buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = 0;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-78:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

/* LVO -0x054 */
LONG tool_sock_shutdown(struct Library *base, LONG s, LONG how)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            d1  __asm("d1") = how;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-84:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1)
                      : "a0", "a1", "cc", "memory");
    return res;
}

/* LVO -0x05a */
LONG tool_sock_setsockopt(struct Library *base, LONG s, LONG level, LONG name,
                          const void *val, LONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            d1  __asm("d1") = level;
    register LONG            d2  __asm("d2") = name;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)val;
    register LONG            d3  __asm("d3") = len;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-90:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2), "r" (a0),
                        "r" (d3)
                      : "a1", "cc", "memory");
    return res;
}

/* LVO -0x060 */
LONG tool_sock_getsockopt(struct Library *base, LONG s, LONG level, LONG name,
                          void *val, LONG *len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            d1  __asm("d1") = level;
    register LONG            d2  __asm("d2") = name;
    register APTR            a0  __asm("a0") = (APTR)val;
    register APTR            a1  __asm("a1") = (APTR)len;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-96:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2), "r" (a0),
                        "r" (a1)
                      : "cc", "memory");
    return res;
}

/* LVO -0x066 */
LONG tool_sock_getsockname(struct Library *base, LONG s, ToolSockAddr *sa)
{
    LONG namelen = (LONG)sizeof(*sa);

    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = (APTR)sa;
    register APTR            a1  __asm("a1") = (APTR)&namelen;
    register LONG            res __asm("d0");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-102:W)"
                      : "=r" (res), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "d1", "cc", "memory");
    return res;
}

/* LVO -0x072 */
LONG tool_sock_ioctl(struct Library *base, LONG s, ULONG req, void *argp)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register ULONG           d1  __asm("d1") = req;
    register APTR            a0  __asm("a0") = (APTR)argp;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-114:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (a0)
                      : "a1", "cc", "memory");
    return res;
}

/* LVO -0x078 */
LONG tool_sock_close(struct Library *base, LONG s)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-120:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

/* LVO -0x07e */
LONG tool_sock_select(struct Library *base, LONG nfds, ToolFdSet *readfds,
                      ToolFdSet *writefds, ToolTimeval *tv)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = nfds;
    register APTR            a0  __asm("a0") = (APTR)readfds;
    register APTR            a1  __asm("a1") = (APTR)writefds;
    register APTR            a2  __asm("a2") = NULL;
    register APTR            a3  __asm("a3") = (APTR)tv;
    register ULONG          *d1  __asm("d1") = NULL;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-126:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1), "r" (a2),
                        "r" (a3), "r" (d1)
                      : "cc", "memory");
    return res;
}

/* LVO -0x0a2 */
LONG tool_sock_errno(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)"
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

/* LVO -0x0d2 */
ToolHostEnt *tool_sock_gethostbyname(struct Library *base, const char *name)
{
    register struct Library *a6  __asm("a6") = base;
    register const char     *a0  __asm("a0") = name;
    register ToolHostEnt    *res __asm("d0");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-210:W)"
                      : "=r" (res), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "d1", "a1", "cc", "memory");
    return res;
}

/* LVO -0x0ea */
ToolServEnt *tool_sock_getservbyname(struct Library *base, const char *name,
                                     const char *proto)
{
    register struct Library *a6  __asm("a6") = base;
    register const char     *a0  __asm("a0") = name;
    register const char     *a1  __asm("a1") = proto;
    register ToolServEnt    *res __asm("d0");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-234:W)"
                      : "=r" (res), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (a0), "r" (a1)
                      : "d1", "cc", "memory");
    return res;
}

/* ------------------------------------------------------------- helpers ---- */

VOID tool_sock_addr(ToolSockAddr *sa, ULONG address, UWORD port)
{
    ULONG i;

    for (i = 0; i < (ULONG)sizeof(sa->sin_zero); i++)
        sa->sin_zero[i] = 0;

    sa->sin_len    = (UBYTE)sizeof(*sa);
    sa->sin_family = (UBYTE)TOOL_AF_INET;
    sa->sin_port   = port;              /* big-endian host: already network */
    sa->sin_addr   = address;
}

BOOL tool_sock_resolve(struct Library *base, const char *host, ULONG *out)
{
    ToolHostEnt *he;
    ULONG        address = 0;
    ULONG        i;

    if (ami_config_parse_ip(host, &address))
    {
        *out = address;
        return TRUE;
    }

    he = tool_sock_gethostbyname(base, host);
    if (he == NULL || he->h_addr_list == NULL ||
        he->h_addr_list[0] == NULL || he->h_length != 4)
    {
        tool_error("cannot resolve \"%s\"", (LONG)host);
        tool_explain_resolve(host, AMI_NET_ERR_NONAME);
        return FALSE;
    }

    for (i = 0; i < 4UL; i++)
        address = (address << 8) | (ULONG)(UBYTE)he->h_addr_list[0][i];

    *out = address;
    return TRUE;
}

UWORD tool_sock_port(struct Library *base, const char *text, const char *proto)
{
    ToolServEnt *se;
    ULONG        value = 0;
    ULONG        i;

    if (text == NULL || text[0] == '\0')
    {
        tool_error("no port given");
        return 0;
    }

    if (text[0] >= '0' && text[0] <= '9')
    {
        for (i = 0; text[i] != '\0'; i++)
        {
            if (text[i] < '0' || text[i] > '9')
            {
                tool_error("\"%s\" is not a port number", (LONG)text);
                return 0;
            }
            value = (value * 10UL) + (ULONG)(text[i] - '0');
            if (value > 65535UL)
            {
                tool_error("port %s is out of range", (LONG)text);
                return 0;
            }
        }

        if (value == 0)
        {
            tool_error("port 0 is not a port anything listens on");
            return 0;
        }

        return (UWORD)value;
    }

    /*
     * A service name, through the library's own getservbyname(), so the answer
     * comes from DEVS:Internet/services like every other program's.
     */
    se = tool_sock_getservbyname(base, text, proto);
    if (se == NULL)
    {
        tool_error("there is no %s service called \"%s\"",
                   (LONG)proto, (LONG)text);
        tool_advise_blank();
        tool_advise("Service names come from DEVS:Internet/services.  "
                    "A number always works.");
        return 0;
    }

    return (UWORD)se->s_port;           /* network order == host order here */
}

/*
 * The unnamed case gets the number.  The errnos that turn up in bug reports are
 * the ones not in the list below, so printing the number beats a generic "the
 * network refused".
 */
static char tool_sock_errbuf[64];

static const char *tool_sock_unnamed(LONG err)
{
    static const char prefix[] = "the stack reported error ";
    char  digits[12];
    ULONG n = 0;
    ULONG o = 0;
    ULONG v;

    while (prefix[o] != '\0')
    {
        tool_sock_errbuf[o] = prefix[o];
        o++;
    }

    v = (err < 0) ? (ULONG)(-err) : (ULONG)err;
    if (err < 0)
        tool_sock_errbuf[o++] = '-';

    if (v == 0)
        digits[n++] = '0';
    while (v > 0)
    {
        digits[n++] = (char)('0' + (v % 10UL));
        v /= 10UL;
    }
    while (n > 0)
        tool_sock_errbuf[o++] = digits[--n];

    tool_sock_errbuf[o] = '\0';
    return tool_sock_errbuf;
}

const char *tool_sock_errstr(LONG err)
{
    switch (err)
    {
        case 0:                  return "no error";
        case TOOL_EINTR:         return "interrupted";
        case TOOL_EPIPE:         return "the other end has gone";
        case TOOL_EWOULDBLOCK:   return "nothing to do yet";
        case TOOL_EINPROGRESS:   return "still connecting";
        case TOOL_EADDRINUSE:    return "that port is already in use";
        case TOOL_ENETUNREACH:   return "there is no route to that network";
        case TOOL_ECONNRESET:    return "the other end reset the connection";
        case TOOL_ETIMEDOUT:     return "the other end never answered";
        case TOOL_ECONNREFUSED:  return "connection refused";
        case TOOL_EHOSTUNREACH:  return "there is no route to that host";
        default:                 return tool_sock_unnamed(err);
    }
}

VOID tool_sock_fail(struct Library *base, const char *what, ULONG address,
                    UWORD port)
{
    LONG err = tool_sock_errno(base);
    char dotted[16];

    ami_config_format_ip(address, dotted, sizeof(dotted));

    tool_error("cannot %s %s port %ld: %s", (LONG)what, (LONG)dotted,
               (LONG)port, (LONG)tool_sock_errstr(err));
}

/* ------------------------------------------------------------- console ---- */

VOID tool_input_open(ToolInput *in, BOOL want_raw)
{
    in->fh          = Input();
    in->interactive = (IsInteractive(in->fh) != 0) ? TRUE : FALSE;
    in->raw         = FALSE;
    in->eof         = FALSE;

    if (in->interactive && want_raw)
    {
        /*
         * SetMode(fh, 1) is the console's RAW mode: no line editing, no local
         * echo, every keystroke available as it is pressed.  It is a property
         * of the console, not of this process, so every caller must reach
         * tool_input_close() on every path including Ctrl-C or the user's Shell
         * is left unusable.
         */
        if (SetMode(in->fh, 1L) != 0)
            in->raw = TRUE;
    }
}

VOID tool_input_close(ToolInput *in)
{
    if (in->raw)
    {
        (VOID)SetMode(in->fh, 0L);
        in->raw = FALSE;
    }
}

LONG tool_input_read(ToolInput *in, UBYTE *buf, LONG len, ULONG micros)
{
    LONG n;

    if (in->eof)
        return 0;

    if (in->interactive)
    {
        if (WaitForChar(in->fh, (LONG)micros) == 0)
            return -1;              /* nothing pressed yet */
    }

    n = Read(in->fh, (APTR)buf, len);
    if (n <= 0)
    {
        in->eof = TRUE;
        return 0;
    }

    return n;
}

/*
 * Straight to standard output, past dos.library's buffer.
 *
 * These commands print two kinds of thing to the same stream: their own lines
 * through VPrintf(), which dos.library buffers, and socket bytes through
 * Write(), which it does not.  Without the Flush() the second overtakes the
 * first and the transcript comes out interleaved.  Observed.
 */
LONG tool_output_write(const UBYTE *buf, LONG len)
{
    BPTR out = Output();

    if (len <= 0)
        return 0;

    (VOID)Flush(out);

    return Write(out, (APTR)buf, len);
}
