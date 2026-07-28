/*
 * nslookup -- ask the DNS for one kind of record.
 *
 *     nslookup NAME/A,SERVER,TYPE/K,TIMEOUT/N/K
 *
 *   TYPE=A       the address                     (the default)
 *   TYPE=PTR     the name of an address
 *   TYPE=CNAME   what this name is an alias for
 *   TYPE=NS      the servers authoritative for a zone
 *   TYPE=MX      where mail for a domain goes, with its preference
 *   TYPE=TXT     the free text, which is where SPF and verification records go
 *   TYPE=SRV     a service: priority, weight, port and host
 *   TYPE=SOA     the start of authority for a zone
 *
 * A dotted quad with no TYPE is looked up backwards, as `host` does.
 *
 * This is a DNS client rather than a caller of one: a UDP socket, an RFC 1035
 * query built here and the answer section parsed here. bsdsocket.library
 * offers only gethostbyname(), so there is no way to ask for an MX, and a
 * lookup through the resolver cache diagnoses the cache.
 *
 * Sending our own datagram is also what makes SERVER possible: the machine has
 * one resolver server list, so pointing it elsewhere would move every other
 * program's lookups too. With no SERVER, the first server the stack is using:
 * the DHCP lease's, else DEVS:Internet/name_resolution's.
 *
 * A datagram from port 53 is unvalidated input and this machine has no MMU, so
 * every length in the reply is bounded against the bytes actually received.
 * Message compression (RFC 1035 4.1.4 -- a label length byte with its top two
 * bits set points to an earlier name) is followed only backwards and at most
 * sixteen times; a forward or self-referential pointer would otherwise let a
 * fourteen-byte reply loop until the machine is reset. Text out of a record is
 * filtered too, because 0x9B is the Amiga console's CSI and a TXT record can
 * carry one.
 *
 * UDP only, no retry over TCP: a truncated answer is reported as truncated
 * rather than silently shortened.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"

const char *const tool_name = "nslookup";

static const char version_tag[] __attribute__((used)) =
    "$VER: nslookup 1.0 (27.7.2026)";

#define TEMPLATE    "NAME/A,SERVER,TYPE/K,TIMEOUT/N/K"

enum
{
    ARG_NAME = 0,
    ARG_SERVER,
    ARG_TYPE,
    ARG_TIMEOUT,
    ARG_COUNT
};

#define NSL_PORT                53
#define NSL_DEFAULT_TIMEOUT     10UL    /* seconds, over all attempts       */
#define NSL_ATTEMPTS            3       /* a datagram does get lost         */

/* RFC 1035 2.3.4: 255 octets of wire name, 63 of label, 512 of message. */
#define NSL_HDR                 12
#define NSL_WIRE_NAME_MAX       255
#define NSL_LABEL_MAX           63
#define NSL_QUERY_MAX           (NSL_HDR + NSL_WIRE_NAME_MAX + 4)

/*
 * Bigger than the 512 a reply without EDNS0 may be: the bound that keeps this
 * parser safe is the bytes received, not the bytes a well-behaved server would
 * have sent.
 */
#define NSL_REPLY_MAX           1472

#define NSL_TEXT_MAX            256     /* a name, or a TXT record, as text */
#define NSL_MAX_JUMPS           16      /* compression pointers per name    */

/* The QTYPEs, and QCLASS=IN. */
#define NSL_T_A                 1
#define NSL_T_NS                2
#define NSL_T_CNAME             5
#define NSL_T_SOA               6
#define NSL_T_PTR               12
#define NSL_T_MX                15
#define NSL_T_TXT               16
#define NSL_T_SRV               33
#define NSL_C_IN                1

/* Header flags. */
#define NSL_F_QR                0x8000U /* this is a response               */
#define NSL_F_AA                0x0400U /* authoritative                    */
#define NSL_F_TC                0x0200U /* it did not fit in a datagram     */
#define NSL_F_RD                0x0100U /* please recurse                   */
#define NSL_F_RCODE             0x000fU

/* What nsl_exchange() returns instead of a length. */
#define NSL_BROKEN              (-1)
#define NSL_BREAK               (-2)

/*
 * Static, not automatic: a Shell command gets the Shell's stack, 4 KB on a
 * stock Kickstart 3.1. Same as tftp and fetch.
 */
static UBYTE nsl_query[NSL_QUERY_MAX];
static UBYTE nsl_reply[NSL_REPLY_MAX];
static char  nsl_qname[NSL_TEXT_MAX];
static char  nsl_text[NSL_TEXT_MAX];
static char  nsl_text2[NSL_TEXT_MAX];
static AmiConfig nsl_config;

/*
 * The names a user types, and the QTYPE each asks for. The help text is
 * generated from this table, so a new type always appears in the usage.
 */
typedef struct
{
    const char *nt_Name;
    UWORD       nt_Type;
} NslType;

static const NslType nsl_types[] =
{
    { "A",      NSL_T_A     },
    { "PTR",    NSL_T_PTR   },
    { "CNAME",  NSL_T_CNAME },
    { "NS",     NSL_T_NS    },
    { "MX",     NSL_T_MX    },
    { "TXT",    NSL_T_TXT   },
    { "SRV",    NSL_T_SRV   },
    { "SOA",    NSL_T_SOA   }
};

/* ------------------------------------------------------------- bytes ----- */

static UWORD nsl_get16(const UBYTE *p)
{
    return (UWORD)(((UWORD)p[0] << 8) | (UWORD)p[1]);
}

static ULONG nsl_get32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] <<  8) |  (ULONG)p[3];
}

static VOID nsl_put16(UBYTE *p, UWORD v)
{
    p[0] = (UBYTE)(v >> 8);
    p[1] = (UBYTE)(v & 0xff);
}

/*
 * One byte of a record, on its way to the screen. Below 0x20 is a control
 * character; 0x7f-0x9f are C1, and 0x9b is the Amiga console's CSI, which
 * moves the cursor, clears the window and changes the mode. Names and TXT
 * records are free text chosen by whoever runs the zone.
 */
static char nsl_safe_char(UBYTE c)
{
    if (c < 0x20 || (c >= 0x7f && c <= 0x9f))
        return '?';

    return (char)c;
}

/* --------------------------------------------------------- names, out ---- */

/*
 * A name in wire form: each label prefixed by its length, a zero label for the
 * root. Returns bytes written, or 0 when the name is not a valid one.
 *
 * "example.com." and "example.com" are the same name; "." alone is the root,
 * which is a legitimate NS or SOA question.
 */
static ULONG nsl_encode_name(const char *name, UBYTE *out, ULONG outlen)
{
    ULONG o = 0;
    ULONG i = 0;

    if (name == NULL || name[0] == '\0')
        return 0;

    if (name[0] == '.' && name[1] == '\0')
    {
        if (outlen < 1)
            return 0;
        out[0] = 0;
        return 1;
    }

    while (name[i] != '\0')
    {
        ULONG start = i;
        ULONG n;

        while (name[i] != '\0' && name[i] != '.')
            i++;

        n = i - start;

        /* An empty label is "a..b", or a leading dot: neither is a name. */
        if (n == 0 || n > (ULONG)NSL_LABEL_MAX)
            return 0;

        if (o + n + 2 > outlen || o + n + 2 > (ULONG)NSL_WIRE_NAME_MAX + 1)
            return 0;

        out[o++] = (UBYTE)n;
        while (start < i)
            out[o++] = (UBYTE)name[start++];

        if (name[i] == '.')
        {
            i++;

            /* A trailing dot ends the name; anything after it is a label. */
            if (name[i] == '\0')
                break;
        }
    }

    out[o++] = 0;

    return o;
}

/* "10.0.2.3" -> "3.2.0.10.in-addr.arpa", which is how a PTR is asked for. */
static VOID nsl_reverse_name(ULONG addr, char *out, ULONG outlen)
{
    static const char suffix[] = "in-addr.arpa";
    ULONG o = 0;
    ULONG i;

    for (i = 0; i < 4UL; i++)
    {
        ULONG v = (addr >> (i * 8U)) & 0xffUL;
        char  d[3];
        ULONG n = 0;

        if (v == 0)
            d[n++] = '0';
        while (v > 0)
        {
            d[n++] = (char)('0' + (v % 10UL));
            v /= 10UL;
        }
        while (n > 0 && o + 1 < outlen)
            out[o++] = d[--n];

        if (o + 1 < outlen)
            out[o++] = '.';
    }

    for (i = 0; suffix[i] != '\0' && o + 1 < outlen; i++)
        out[o++] = suffix[i];

    out[o] = '\0';
}

/* ---------------------------------------------------------- names, in ---- */

/*
 * A name out of a reply at `pos`, as text in `out` (NULL when only the length
 * is wanted).
 *
 * Returns the offset of the byte after the name as it appears at `pos` -- for
 * a compressed name that is two past the pointer, not the end of whatever it
 * pointed at -- or -1 when the message will not support one.
 *
 * Every exit from the loop is a bound. `len` is the bytes received, never a
 * length the message claimed. A pointer must go strictly backwards, which is
 * what makes the walk terminate; sixteen jumps is far past any real message.
 * The 0x40 and 0x80 label types are reserved, so they are rejected.
 */
static LONG nsl_decode_name(const UBYTE *msg, ULONG len, ULONG pos,
                            char *out, ULONG outlen)
{
    ULONG o     = 0;
    ULONG jumps = 0;
    LONG  end   = -1;

    if (out != NULL)
    {
        if (outlen < 2)
            return -1;
        out[0] = '\0';
    }

    for (;;)
    {
        UBYTE c;

        if (pos >= len)
            return -1;

        c = msg[pos];

        if ((c & 0xc0) == 0xc0)
        {
            ULONG target;

            if (pos + 1 >= len)
                return -1;

            target = (((ULONG)(c & 0x3f)) << 8) | (ULONG)msg[pos + 1];

            if (end < 0)
                end = (LONG)(pos + 2);

            if (target >= pos || ++jumps > (ULONG)NSL_MAX_JUMPS)
                return -1;

            pos = target;
            continue;
        }

        if ((c & 0xc0) != 0)
            return -1;

        if (c == 0)
        {
            if (end < 0)
                end = (LONG)(pos + 1);
            break;
        }

        if (pos + 1UL + (ULONG)c > len)
            return -1;

        if (out != NULL)
        {
            ULONG i;

            if (o != 0)
            {
                if (o + 1 >= outlen)
                    return -1;
                out[o++] = '.';
            }

            if (o + (ULONG)c + 1 > outlen)
                return -1;

            for (i = 0; i < (ULONG)c; i++)
                out[o++] = nsl_safe_char(msg[pos + 1 + i]);
        }

        pos += 1UL + (ULONG)c;
    }

    if (out != NULL)
    {
        if (o == 0)
            out[o++] = '.';             /* the root, which has no labels */
        out[o] = '\0';
    }

    return end;
}

/* --------------------------------------------------------- the question -- */

/* The query message, built into nsl_query. Its length, or 0. */
static ULONG nsl_build(const char *qname, UWORD type, UWORD id)
{
    ULONG n;

    nsl_put16(&nsl_query[0], id);
    nsl_put16(&nsl_query[2], NSL_F_RD);
    nsl_put16(&nsl_query[4], 1);        /* one question                     */
    nsl_put16(&nsl_query[6], 0);
    nsl_put16(&nsl_query[8], 0);
    nsl_put16(&nsl_query[10], 0);

    n = nsl_encode_name(qname, &nsl_query[NSL_HDR],
                        sizeof(nsl_query) - NSL_HDR - 4);
    if (n == 0)
        return 0;

    nsl_put16(&nsl_query[NSL_HDR + n], type);
    nsl_put16(&nsl_query[NSL_HDR + n + 2], NSL_C_IN);

    return NSL_HDR + n + 4;
}

/*
 * The identifier, checked on the way back. Not a cryptographic number: with
 * the source address it is enough for a one-shot diagnostic to ignore a stale
 * answer to the previous question and a bystander's datagram. A resolver
 * keeping a cache would need more.
 */
static UWORD nsl_id(VOID)
{
    ULONG t = ami_millis();
    ULONG a = (ULONG)FindTask(NULL);

    return (UWORD)(((t << 5) ^ (t >> 3) ^ (a >> 2) ^ (a << 7)) & 0xffffUL);
}

/* ----------------------------------------------------------- the answer -- */

/* One record of the answer section, formatted for the type it turned out to
   be, which is not always the type asked for: an A question is answered with
   the CNAME chain in front of it. */
static VOID nsl_print_record(const UBYTE *msg, ULONG len, UWORD type,
                             ULONG rdata, ULONG rdlen)
{
    char addr[16];
    LONG e1;
    LONG e2;

    switch (type)
    {
        case NSL_T_A:
            if (rdlen != 4)
                break;
            ami_config_format_ip(nsl_get32(&msg[rdata]), addr, sizeof(addr));
            tool_printf("  address    %s\n", (LONG)addr);
            break;

        case NSL_T_PTR:
            if (nsl_decode_name(msg, len, rdata, nsl_text,
                                sizeof(nsl_text)) < 0)
                break;
            tool_printf("  name       %s\n", (LONG)nsl_text);
            break;

        case NSL_T_CNAME:
            if (nsl_decode_name(msg, len, rdata, nsl_text,
                                sizeof(nsl_text)) < 0)
                break;
            tool_printf("  canonical  %s\n", (LONG)nsl_text);
            break;

        case NSL_T_NS:
            if (nsl_decode_name(msg, len, rdata, nsl_text,
                                sizeof(nsl_text)) < 0)
                break;
            tool_printf("  server     %s\n", (LONG)nsl_text);
            break;

        case NSL_T_MX:
            if (rdlen < 3)
                break;
            if (nsl_decode_name(msg, len, rdata + 2, nsl_text,
                                sizeof(nsl_text)) < 0)
                break;
            tool_printf("  mail       %-32s preference %lu\n",
                        (LONG)nsl_text, (LONG)nsl_get16(&msg[rdata]));
            break;

        case NSL_T_SRV:
            if (rdlen < 7)
                break;
            if (nsl_decode_name(msg, len, rdata + 6, nsl_text,
                                sizeof(nsl_text)) < 0)
                break;
            tool_printf("  service    %-32s port %lu, priority %lu, "
                        "weight %lu\n",
                        (LONG)nsl_text, (LONG)nsl_get16(&msg[rdata + 4]),
                        (LONG)nsl_get16(&msg[rdata]),
                        (LONG)nsl_get16(&msg[rdata + 2]));
            break;

        case NSL_T_TXT:
        {
            /*
             * A TXT record is a sequence of length-prefixed strings. SPF,
             * DKIM and verification tokens get split across them by the
             * 255-byte limit and are meant to be read joined, so join them.
             */
            ULONG p   = rdata;
            ULONG end = rdata + rdlen;
            ULONG o   = 0;

            while (p < end)
            {
                ULONG n = (ULONG)msg[p++];
                ULONG i;

                if (p + n > end)
                    break;

                for (i = 0; i < n && o + 1 < sizeof(nsl_text); i++)
                    nsl_text[o++] = nsl_safe_char(msg[p + i]);

                p += n;
            }

            nsl_text[o] = '\0';
            tool_printf("  text       \"%s\"\n", (LONG)nsl_text);
            break;
        }

        case NSL_T_SOA:
            e1 = nsl_decode_name(msg, len, rdata, nsl_text, sizeof(nsl_text));
            if (e1 < 0)
                break;
            e2 = nsl_decode_name(msg, len, (ULONG)e1, nsl_text2,
                                 sizeof(nsl_text2));
            if (e2 < 0 || (ULONG)e2 + 20UL > len)
                break;

            tool_printf("  authority  %s\n", (LONG)nsl_text);
            tool_printf("  mailbox    %s\n", (LONG)nsl_text2);
            tool_printf("  serial     %lu\n", nsl_get32(&msg[e2]));
            tool_printf("  timers     refresh %lu, retry %lu, expire %lu, "
                        "minimum %lu\n",
                        nsl_get32(&msg[e2 + 4]), nsl_get32(&msg[e2 + 8]),
                        nsl_get32(&msg[e2 + 12]), nsl_get32(&msg[e2 + 16]));
            break;

        default:
            /*
             * A record we cannot format -- an AAAA, a signature, an OPT.
             * Still reported, since "no records" would be wrong.
             */
            tool_printf("  type %-6lu %lu bytes\n", (LONG)type, (LONG)rdlen);
            break;
    }
}

/*
 * Walk the answer section. Returns records printed, or -1 for a malformed
 * reply (not the same as an empty one). The question section is walked first
 * to find where the answers start; its name may itself be compressed.
 */
static LONG nsl_print_answers(const UBYTE *msg, ULONG len)
{
    UWORD qd = nsl_get16(&msg[4]);
    UWORD an = nsl_get16(&msg[6]);
    LONG  pos = NSL_HDR;
    LONG  printed = 0;
    UWORD i;

    for (i = 0; i < qd; i++)
    {
        pos = nsl_decode_name(msg, len, (ULONG)pos, NULL, 0);
        if (pos < 0 || (ULONG)pos + 4UL > len)
            return -1;
        pos += 4;
    }

    for (i = 0; i < an; i++)
    {
        UWORD type;
        UWORD rdlen;
        ULONG rdata;

        pos = nsl_decode_name(msg, len, (ULONG)pos, NULL, 0);
        if (pos < 0 || (ULONG)pos + 10UL > len)
            return -1;

        type  = nsl_get16(&msg[pos]);           /* class and TTL, at +2 and  */
        rdlen = nsl_get16(&msg[pos + 8]);       /* +4, are not printed       */
        rdata = (ULONG)pos + 10UL;

        if (rdata + (ULONG)rdlen > len)
            return -1;

        nsl_print_record(msg, len, type, rdata, (ULONG)rdlen);
        printed++;

        pos = (LONG)(rdata + (ULONG)rdlen);
    }

    return printed;
}

/* --------------------------------------------------------- the exchange -- */

/*
 * Send the query and wait for its answer. Returns bytes received into
 * nsl_reply, 0 when nobody answered, NSL_BROKEN on a failed socket, NSL_BREAK
 * on Ctrl-C. The wait is cut into 200 ms slices so a break is noticed while
 * waiting on a dead server. An accepted datagram must come from the server
 * asked, be a response, and carry the identifier sent.
 */
static LONG nsl_exchange(struct Library *sb, LONG sock, const ToolSockAddr *srv,
                         const UBYTE *q, LONG qlen, UWORD id, ULONG secs)
{
    ULONG per   = secs / (ULONG)NSL_ATTEMPTS;
    ULONG tries;

    if (per == 0)
        per = 1;

    for (tries = 0; tries < (ULONG)NSL_ATTEMPTS; tries++)
    {
        ULONG slices = (per * 1000UL) / 200UL;
        ULONG i;

        if (slices == 0)
            slices = 1;

        if (tool_sock_sendto(sb, sock, q, qlen, srv) != qlen)
        {
            tool_error("cannot send the query: %s",
                       (LONG)tool_sock_errstr(tool_sock_errno(sb)));
            return NSL_BROKEN;
        }

        for (i = 0; i < slices; i++)
        {
            ToolSockAddr from;
            ToolFdSet    readfds;
            ToolTimeval  tv;
            LONG         ready;
            LONG         n;

            if (tool_break())
                return NSL_BREAK;

            tool_fd_zero(&readfds);
            tool_fd_add(&readfds, sock);

            tv.tv_secs  = 0;
            tv.tv_micro = 200000;

            ready = tool_sock_select(sb, sock + 1, &readfds, NULL, &tv);
            if (ready < 0)
            {
                if (tool_sock_errno(sb) == TOOL_EINTR)
                    continue;
                return NSL_BROKEN;
            }
            if (ready == 0)
                continue;

            from.sin_addr = 0;
            from.sin_port = 0;

            n = tool_sock_recvfrom(sb, sock, nsl_reply,
                                   (LONG)sizeof(nsl_reply), &from);
            if (n < 0)
            {
                if (tool_sock_errno(sb) == TOOL_EINTR)
                    continue;
                return NSL_BROKEN;
            }

            if (from.sin_addr != srv->sin_addr || from.sin_port != srv->sin_port)
                continue;                       /* not the server we asked  */

            if (n < NSL_HDR)
                continue;

            if (nsl_get16(&nsl_reply[0]) != id)
                continue;                       /* an older question's      */

            if ((nsl_get16(&nsl_reply[2]) & NSL_F_QR) == 0)
                continue;                       /* somebody else's question */

            return n;
        }
    }

    return 0;
}

/* ------------------------------------------------------------- arguments -- */

static LONG nsl_type_from_name(const char *text, UWORD *type_out)
{
    ULONG i;

    for (i = 0; i < sizeof(nsl_types) / sizeof(nsl_types[0]); i++)
    {
        if (tool_stricmp(text, nsl_types[i].nt_Name) == 0)
        {
            *type_out = nsl_types[i].nt_Type;
            return 0;
        }
    }

    return -1;
}

static VOID nsl_print_types(VOID)
{
    ULONG i;

    tool_printf("nslookup: TYPE is one of");
    for (i = 0; i < sizeof(nsl_types) / sizeof(nsl_types[0]); i++)
        tool_printf("%s%s", (i == 0) ? " " : ", ", (LONG)nsl_types[i].nt_Name);
    tool_printf("\n");
}

/*
 * The server to ask when none was named. The running stack's list first, via
 * ObtainDomainNameServerList(), since DHCP-supplied name servers appear in no
 * file on disk. DEVS:Internet/name_resolution second, for a static setup.
 */
static BOOL nsl_default_server(ULONG *out)
{
    char  servers[4][16];
    ULONG count = tool_stack_name_servers(servers, 4UL);
    ULONG i;

    for (i = 0; i < count; i++)
    {
        if (ami_config_parse_ip(servers[i], out))
            return TRUE;
    }

    if (ami_config_load(&nsl_config) == AMI_CFG_OK &&
        nsl_config.resolver.nameserver_count > 0)
    {
        *out = nsl_config.resolver.nameserver[0];
        return TRUE;
    }

    return FALSE;
}

/* The RCODE, as a sentence. Only the first six are named by RFC 1035. */
static const char *nsl_rcode_text(UWORD rcode)
{
    switch (rcode)
    {
        case 1:  return "the server did not understand the query";
        case 2:  return "the server failed to answer it";
        case 3:  return "there is no such name";
        case 4:  return "the server does not implement that kind of query";
        case 5:  return "the server refused to answer";
        default: return "the server answered with an error";
    }
}

/* ------------------------------------------------------------------ main -- */

int main(int argc, char **argv)
{
    LONG            args[ARG_COUNT];
    struct RDArgs  *rda;
    struct Library *sb;
    ToolSockAddr    srv;
    const char     *name;
    ULONG           server = 0;
    ULONG           address = 0;
    ULONG           timeout;
    ULONG           qlen;
    UWORD           type = NSL_T_A;
    UWORD           id;
    UWORD           flags;
    LONG            sock;
    LONG            n;
    LONG            printed;
    LONG            rc = RETURN_OK;
    ULONG           i;
    char            dotted[16];

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    for (i = 0; i < (ULONG)ARG_COUNT; i++)
        args[i] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("<name or address> [<server>] [TYPE <type>]",
                   "Asks the DNS for one kind of record.");
        return RETURN_ERROR;
    }

    name    = (const char *)args[ARG_NAME];
    timeout = (args[ARG_TIMEOUT] != 0)
                  ? (ULONG)(*(LONG *)args[ARG_TIMEOUT])
                  : NSL_DEFAULT_TIMEOUT;
    if (timeout == 0)
        timeout = NSL_DEFAULT_TIMEOUT;

    if (args[ARG_TYPE] != 0)
    {
        if (nsl_type_from_name((const char *)args[ARG_TYPE], &type) != 0)
        {
            tool_error("\"%s\" is not a record type I know",
                       (LONG)args[ARG_TYPE]);
            nsl_print_types();
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }
    else if (ami_config_parse_ip(name, &address))
    {
        /* Same rule as host: an address typed on its own means "who is this". */
        type = NSL_T_PTR;
    }

    /* A PTR is asked for by name, whichever way this command was told to. */
    if (type == NSL_T_PTR && ami_config_parse_ip(name, &address))
    {
        nsl_reverse_name(address, nsl_qname, sizeof(nsl_qname));
    }
    else
    {
        /*
         * tool_copy_string() truncates, and a truncated name is a different
         * name: asking about it would produce a plausible-looking answer.
         */
        for (i = 0; name[i] != '\0'; i++)
            ;
        if (i >= sizeof(nsl_qname))
        {
            tool_error("that name is too long for the DNS");
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        tool_copy_string(nsl_qname, sizeof(nsl_qname), name);
    }

    sb = tool_socket_open();
    if (sb == NULL)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (args[ARG_SERVER] != 0)
    {
        if (!tool_sock_resolve(sb, (const char *)args[ARG_SERVER], &server))
        {
            CloseLibrary(sb);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }
    else if (!nsl_default_server(&server))
    {
        tool_error("no name server is configured");
        tool_advise_blank();
        tool_advise("Name servers come from the DHCP lease, or from");
        tool_advise("DEVS:Internet/name_resolution.  Naming one on the");
        tool_advise("command line works without either:");
        tool_advise_blank();
        tool_advise("  nslookup example.com 8.8.8.8");
        CloseLibrary(sb);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    ami_config_format_ip(server, dotted, sizeof(dotted));

    id   = nsl_id();
    qlen = nsl_build(nsl_qname, type, id);
    if (qlen == 0)
    {
        tool_error("\"%s\" is not a name the DNS can be asked about",
                   (LONG)name);
        CloseLibrary(sb);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    sock = tool_sock_socket(sb, TOOL_AF_INET, TOOL_SOCK_DGRAM, 0);
    if (sock < 0)
    {
        tool_error("no socket: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(sb)));
        CloseLibrary(sb);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    tool_sock_addr(&srv, server, (UWORD)NSL_PORT);

    n = nsl_exchange(sb, sock, &srv, nsl_query, (LONG)qlen, id, timeout);

    (VOID)tool_sock_close(sb, sock);
    CloseLibrary(sb);

    if (n == NSL_BREAK)
    {
        tool_fault(ERROR_BREAK);
        FreeArgs(rda);
        return RETURN_WARN;
    }

    if (n == NSL_BROKEN)
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (n == 0)
    {
        tool_error("%s did not answer in %lu seconds", (LONG)dotted, timeout);
        tool_explain_resolve(nsl_qname, AMI_NET_ERR_TIMEOUT);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    flags = nsl_get16(&nsl_reply[2]);

    if ((flags & NSL_F_RCODE) != 0)
    {
        tool_error("%s: %s", (LONG)nsl_qname,
                   (LONG)nsl_rcode_text(flags & NSL_F_RCODE));
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    tool_printf("%s from %s%s\n", (LONG)nsl_qname, (LONG)dotted,
                (LONG)(((flags & NSL_F_AA) != 0) ? " (authoritative)" : ""));

    printed = nsl_print_answers(nsl_reply, (ULONG)n);

    if (printed < 0)
    {
        tool_error("%s sent an answer this cannot be read", (LONG)dotted);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /*
     * TC means the answer did not fit in a datagram and the rest is only
     * available over TCP, which this command does not do. Checked before the
     * empty case, because a server with too much to say commonly sets TC and
     * sends an empty answer section; reporting "no records of that type" there
     * would be wrong. Seen with `nslookup google.com TYPE TXT`, whose RRset is
     * well over 512 bytes.
     */
    if ((flags & NSL_F_TC) != 0)
    {
        tool_printf("  the answer did not fit in a datagram%s\n",
                    (LONG)((printed == 0) ? "" : " -- there is more of it"));
        rc = RETURN_WARN;
    }
    else if (printed == 0)
    {
        /*
         * Not an error: the name exists and has no record of this kind.
         * Saying that is more useful than "not found", which reads as a bad
         * name.
         */
        tool_printf("  no records of that type\n");
        rc = RETURN_WARN;
    }

    FreeArgs(rda);

    return (int)rc;
}
