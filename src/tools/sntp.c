/*
 * sntp, set the system clock from a time server. Unicast only (RFC 4330).
 * The NTP-to-Amiga epoch gap is 2461449600 seconds; a plain 32-bit unsigned
 * subtraction gets the 2036 era right, and holds until the Amiga epoch ends in
 * 2114. NetX Duo's vendored SNTP client cannot be used: its ThreadX calls
 * belong to a kernel a Shell command never entered.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"

#include <exec/io.h>
#include <devices/timer.h>
#include <dos/datetime.h>
#include <resources/battclock.h>
#include <libraries/locale.h>
#include <proto/battclock.h>
#include <proto/locale.h>
#include "aminetxduo/version.h"

const char *const tool_name = "sntp";

/* The library bases the two inline sets above expect to find. Each is opened
   and closed inside the single function that uses it. */
struct Library    *BattClockBase;
struct LocaleBase *LocaleBase;

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("sntp");

#define TEMPLATE    "SERVER/A,TIMEOUT/N/K,SHOW/S,QUIET/S,IPV4=-4/S,IPV6=-6/S"

enum
{
    ARG_SERVER = 0,
    ARG_TIMEOUT,
    ARG_SHOW,
    ARG_QUIET,
    ARG_IPV4,
    ARG_IPV6,
    ARG_COUNT
};

#define SNTP_DEFAULT_TIMEOUT    10UL    /* seconds, for the whole exchange */
#define SNTP_ATTEMPTS           3UL
#define SNTP_PORT               123
#define SNTP_MSG_SIZE           48

/* 1900-01-01 to 1978-01-01, in seconds.  See the epoch note above. */
#define SNTP_NTP_TO_AMIGA       2461449600UL

/* --------------------------------------------------- bsdsocket, by hand --- */

/*
 * Through the LVOs, for the reason fetch.c gives: the NDK inlines assume a
 * global SocketBase. Offsets from docs/RESEARCH.md 3.2.
 */

struct SntpTimeval
{
    LONG    tv_secs;
    LONG    tv_micro;
};

#define SNTP_SOCK_DGRAM     2

static LONG sock_socket(struct Library *base, LONG domain, LONG type, LONG proto)
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

static LONG sock_connect(struct Library *base, LONG s, APTR name, LONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = name;
    register LONG            d1  __asm("d1") = len;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-54:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static LONG sock_send(struct Library *base, LONG s, CONST_APTR buf, LONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = buf;
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

static LONG sock_recv(struct Library *base, LONG s, APTR buf, LONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = buf;
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

static LONG sock_close(struct Library *base, LONG s)
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

static LONG sock_waitselect(struct Library *base, LONG nfds, APTR readfds,
                            struct SntpTimeval *tv)
{
    register struct Library    *a6  __asm("a6") = base;
    register LONG               d0  __asm("d0") = nfds;
    register APTR               a0  __asm("a0") = readfds;
    register APTR               a1  __asm("a1") = NULL;
    register APTR               a2  __asm("a2") = NULL;
    register struct SntpTimeval *a3 __asm("a3") = tv;
    register ULONG             *d1  __asm("d1") = NULL;
    register LONG               res __asm("d0");
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

static LONG sock_errno(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)"
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

/* ------------------------------------------------------ the machine's clock */

/*
 * timer.device through exec.library alone, a timerequest and DoIO(), with no
 * TimerBase to collide with anything else the command links.
 */
static struct MsgPort     *clock_port;
static struct timerequest *clock_req;

static BOOL clock_open(VOID)
{
    clock_port = CreateMsgPort();
    if (clock_port == NULL)
        return FALSE;

    clock_req = (struct timerequest *)
                CreateIORequest(clock_port, (ULONG)sizeof(*clock_req));
    if (clock_req == NULL)
    {
        DeleteMsgPort(clock_port);
        clock_port = NULL;
        return FALSE;
    }

    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK,
                   (struct IORequest *)clock_req, 0) != 0)
    {
        DeleteIORequest((struct IORequest *)clock_req);
        DeleteMsgPort(clock_port);
        clock_req  = NULL;
        clock_port = NULL;
        return FALSE;
    }

    return TRUE;
}

static VOID clock_close(VOID)
{
    if (clock_req != NULL)
    {
        CloseDevice((struct IORequest *)clock_req);
        DeleteIORequest((struct IORequest *)clock_req);
        clock_req = NULL;
    }
    if (clock_port != NULL)
    {
        DeleteMsgPort(clock_port);
        clock_port = NULL;
    }
}

/* Seconds and microseconds since 1978-01-01, local time, as the machine has it. */
static VOID clock_get(ULONG *secs, ULONG *micro)
{
    clock_req->tr_node.io_Command = TR_GETSYSTIME;
    (VOID)DoIO((struct IORequest *)clock_req);

    *secs  = (ULONG)clock_req->tr_time.tv_secs;
    *micro = (ULONG)clock_req->tr_time.tv_micro;
}

static VOID clock_set(ULONG secs, ULONG micro)
{
    clock_req->tr_node.io_Command  = TR_SETSYSTIME;
    clock_req->tr_time.tv_secs     = secs;
    clock_req->tr_time.tv_micro    = micro;
    (VOID)DoIO((struct IORequest *)clock_req);
}

/*
 * The battery-backed clock, which survives the power switch. It holds the same
 * seconds since 1978, local time, as the system clock. A machine without a
 * real-time chip has no resource at all, and this returns FALSE.
 */
static BOOL battclock_write(ULONG secs)
{
    BattClockBase = (struct Library *)OpenResource((CONST_STRPTR)BATTCLOCKNAME);

    if (BattClockBase == NULL)
        return FALSE;

    WriteBattClock(secs);

    /* A resource is never closed: it is not opened either, only looked up. */
    return TRUE;
}

/*
 * Minutes west of Greenwich, from the Locale preferences. FALSE when
 * locale.library is not on the machine; the caller then falls back to UTC.
 */
static BOOL locale_gmt_offset(LONG *minutes_west)
{
    struct Locale *locale;
    BOOL           got = FALSE;

    LocaleBase = (struct LocaleBase *)
                 OpenLibrary((CONST_STRPTR)"locale.library", 38UL);
    if (LocaleBase == NULL)
        return FALSE;

    locale = OpenLocale(NULL);          /* NULL: the current preferences */
    if (locale != NULL)
    {
        *minutes_west = locale->loc_GMTOffset;
        got           = TRUE;
        CloseLocale(locale);
    }

    CloseLibrary((struct Library *)LocaleBase);
    LocaleBase = NULL;

    return got;
}

/* ------------------------------------------------------------- formatting, */

/* dos.library's date formatter, so the time reads like every other date on the
   machine and in the user's Locale. DateToStr() is V36 and the floor is V40. */
typedef struct SntpDateText
{
    char day[LEN_DATSTRING];
    char date[LEN_DATSTRING];
    char time[LEN_DATSTRING];
} SntpDateText;

static VOID format_amiga_time(ULONG secs, SntpDateText *out)
{
    struct DateTime dt;

    out->day[0] = out->date[0] = out->time[0] = '\0';

    dt.dat_Stamp.ds_Days   = (LONG)(secs / 86400UL);
    dt.dat_Stamp.ds_Minute = (LONG)((secs % 86400UL) / 60UL);
    dt.dat_Stamp.ds_Tick   = (LONG)((secs % 60UL) * TICKS_PER_SECOND);
    dt.dat_Format          = FORMAT_DOS;
    dt.dat_Flags           = 0;
    dt.dat_StrDay          = (STRPTR)out->day;
    dt.dat_StrDate         = (STRPTR)out->date;
    dt.dat_StrTime         = (STRPTR)out->time;

    if (!DateToStr(&dt))
    {
        tool_copy_string(out->day,  sizeof(out->day),  "");
        tool_copy_string(out->date, sizeof(out->date), "(a date this machine");
        tool_copy_string(out->time, sizeof(out->time), "cannot print)");
    }
}

/*
 * "3 days 4 hours", the two largest useful units. Printed rather than formatted
 * into a buffer: dos.library's only formatter is VPrintf and it writes to a file.
 */
static VOID print_span(ULONG secs)
{
    ULONG d = secs / 86400UL;
    ULONG h = (secs % 86400UL) / 3600UL;
    ULONG m = (secs % 3600UL) / 60UL;
    ULONG s = secs % 60UL;

    if (d > 0)
        tool_printf("%lu day%s %lu hour%s", d, (LONG)(d == 1 ? "" : "s"),
                    h, (LONG)(h == 1 ? "" : "s"));
    else if (h > 0)
        tool_printf("%lu hour%s %lu minute%s", h, (LONG)(h == 1 ? "" : "s"),
                    m, (LONG)(m == 1 ? "" : "s"));
    else if (m > 0)
        tool_printf("%lu minute%s %lu second%s", m, (LONG)(m == 1 ? "" : "s"),
                    s, (LONG)(s == 1 ? "" : "s"));
    else
        tool_printf("%lu second%s", s, (LONG)(s == 1 ? "" : "s"));
}

/* -------------------------------------------------------------- the wire --- */

/*
 * An SNTP message is 48 bytes and every multi-byte field is big-endian. Byte at
 * a time, so this stays correct if ever read little-endian.
 */
static ULONG be32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8)  |  (ULONG)p[3];
}

static VOID put_be32(UBYTE *p, ULONG v)
{
    p[0] = (UBYTE)(v >> 24);
    p[1] = (UBYTE)(v >> 16);
    p[2] = (UBYTE)(v >> 8);
    p[3] = (UBYTE)v;
}

/*
 * NTP's fraction field is a binary fraction of a second. The exact conversion
 * needs 64 bits; dropping the noisy bottom sixteen leaves g * 15625 / 1024 with
 * g < 65536, exact in 32 bits and resolving to about 15 microseconds.
 */
static ULONG frac_to_micro(ULONG frac)
{
    return (((frac >> 16) & 0xffffUL) * 15625UL) >> 10;
}

typedef struct SntpReply
{
    ULONG   transmit_secs;      /* NTP seconds, the server's idea of now      */
    ULONG   transmit_frac;
    UWORD   stratum;
    ULONG   round_trip_us;      /* measured here, by our own clock            */
} SntpReply;

/*
 * What RFC 4330 5 tells a client to check: mode 4, LI != 3, stratum 1..15,
 * transmit != 0, and originate matching the request that was sent. The source
 * address needs no check -- the socket is connect()ed.
 */
static const char *sntp_validate(const UBYTE *msg, LONG len,
                                 ULONG sent_secs, ULONG sent_frac,
                                 SntpReply *out)
{
    UWORD li;
    UWORD mode;
    UWORD version;

    if (len < SNTP_MSG_SIZE)
        return "the reply was too short to be an SNTP message";

    li      = (UWORD)((msg[0] >> 6) & 0x03);
    version = (UWORD)((msg[0] >> 3) & 0x07);
    mode    = (UWORD)(msg[0] & 0x07);

    if (mode != 4)
        return "the reply was not a server message";
    if (version < 1 || version > 4)
        return "the server uses an NTP version this command does not support";
    if (li == 3)
        return "the server says its own clock is not synchronised";

    out->stratum = msg[1];
    if (out->stratum == 0)
        return "the server refused to serve this machine";
    if (out->stratum > 15)
        return "the server's own clock is not synchronised";

    if (be32(&msg[24]) != sent_secs || be32(&msg[28]) != sent_frac)
        return "the reply did not answer the request that was sent";

    out->transmit_secs = be32(&msg[40]);
    out->transmit_frac = be32(&msg[44]);

    if (out->transmit_secs == 0)
        return "the server does not have the time itself";

    return NULL;
}

/* ------------------------------------------------------------------ main --- */

static LONG arg_or(const LONG *args, int index, LONG fallback)
{
    const LONG *p = (const LONG *)args[index];

    return (p != NULL) ? *p : fallback;
}

/*
 * One request, one reply. FALSE after reporting what went wrong. Up to
 * SNTP_ATTEMPTS requests go out, sharing TIMEOUT between them.
 */
static BOOL sntp_exchange(struct Library *sbase, LONG sock, ULONG timeout,
                          const char *where, SntpReply *reply)
{
    UBYTE msg[SNTP_MSG_SIZE];
    ULONG attempt;
    ULONG i;
    ULONG per_attempt = (timeout + SNTP_ATTEMPTS - 1) / SNTP_ATTEMPTS;

    if (per_attempt == 0)
        per_attempt = 1;

    for (attempt = 0; attempt < SNTP_ATTEMPTS; attempt++)
    {
        ULONG sent_secs;
        ULONG sent_micro;
        ULONG sent_frac;
        ULONG waited = 0;

        if (tool_break())
            return FALSE;

        /*
         * The transmit timestamp is our own clock, which can be decades out.
         * Its only job is to come back in the originate field, and RFC 4330
         * lets a client use any unique value.
         */
        clock_get(&sent_secs, &sent_micro);
        sent_secs += SNTP_NTP_TO_AMIGA;
        sent_frac  = sent_micro;

        for (i = 0; i < (ULONG)SNTP_MSG_SIZE; i++)
            msg[i] = 0;
        msg[0] = (UBYTE)((0 << 6) | (4 << 3) | 3);  /* LI 0, version 4, client */
        put_be32(&msg[40], sent_secs);
        put_be32(&msg[44], sent_frac);

        if (sock_send(sbase, sock, msg, (LONG)sizeof(msg)) != (LONG)sizeof(msg))
        {
            tool_error("cannot send the request to %s (errno %ld)",
                       (LONG)where, (LONG)sock_errno(sbase));
            return FALSE;
        }

        /*
         * Wait in one-second slices so Ctrl-C is noticed promptly. WaitSelect
         * gets no signal mask.
         */
        while (waited < per_attempt)
        {
            struct SntpTimeval tv;
            ULONG              fds = 1UL << (ULONG)sock;
            LONG               ready;

            tv.tv_secs  = 1;
            tv.tv_micro = 0;

            ready = sock_waitselect(sbase, sock + 1, &fds, &tv);

            if (tool_break())
                return FALSE;

            if (ready > 0)
            {
                LONG  got = sock_recv(sbase, sock, msg, (LONG)sizeof(msg));
                ULONG now_secs;
                ULONG now_micro;
                const char *bad;

                clock_get(&now_secs, &now_micro);

                if (got < 0)
                {
                    tool_error("the time server's reply cannot be read");
                    return FALSE;
                }

                bad = sntp_validate(msg, got, sent_secs, sent_frac, reply);
                if (bad != NULL)
                {
                    /*
                     * A bad reply is not a reason to give up: it can be a
                     * stale datagram from a previous run.
                     */
                    tool_error("%s", (LONG)bad);
                    break;
                }

                /*
                 * The round trip by our own clock. Both readings come from
                 * timer.device before anything is changed, so however far out
                 * the clock is cancels exactly.
                 */
                reply->round_trip_us =
                    (now_secs - (sent_secs - SNTP_NTP_TO_AMIGA)) * 1000000UL
                    + now_micro - sent_micro;

                return TRUE;
            }

            waited++;
        }
    }

    tool_error("the time server did not answer");

    return FALSE;
}

int main(int argc, char **argv)
{
    LONG            args[ARG_COUNT];
    struct RDArgs  *rda;
    struct Library *sbase = NULL;
    const char     *server;
    ULONG           timeout;
    BOOL            show;
    BOOL            quiet;
    LONG            family;
    ToolAddr        target;
    LONG            sock = -1;
    SntpReply       reply;
    LONG            gmt_west = 0;
    BOOL            have_locale;
    ULONG           old_secs = 0;
    ULONG           old_micro = 0;
    ULONG           new_utc;
    ULONG           new_local;
    ULONG           new_micro;
    LONG            rc = RETURN_OK;
    char            addrtext[TOOL_ADDR_STRLEN];
    SntpDateText    when;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    args[ARG_SERVER]  = 0;
    args[ARG_TIMEOUT] = 0;
    args[ARG_SHOW]    = 0;
    args[ARG_QUIET]   = 0;
    args[ARG_IPV4]    = 0;
    args[ARG_IPV6]    = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("[-4|-6] <time server>",
                   "The name or address of an SNTP server, for example "
                   "pool.ntp.org.");
        return RETURN_ERROR;
    }

    server  = (const char *)args[ARG_SERVER];

    if (args[ARG_TIMEOUT] != 0 && *(LONG *)args[ARG_TIMEOUT] < 0)
    {
        tool_error("TIMEOUT cannot be negative");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    timeout = (ULONG)arg_or(args, ARG_TIMEOUT, (LONG)SNTP_DEFAULT_TIMEOUT);
    show    = (args[ARG_SHOW] != 0) ? TRUE : FALSE;
    quiet   = (args[ARG_QUIET] != 0) ? TRUE : FALSE;

    if (timeout == 0)
        timeout = SNTP_DEFAULT_TIMEOUT;

    if (!tool_arg_family(args[ARG_IPV4], args[ARG_IPV6], &family))
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /*
     * Opening bsdsocket.library starts the network on a machine where nothing
     * has started it yet, so a configured but unstarted interface still yields
     * a clock.
     */
    sbase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (sbase == NULL)
    {
        tool_error("the network is not available");
        tool_explain_no_stack();
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (!clock_open())
    {
        tool_error("timer.device did not open, so the clock cannot be read");
        CloseLibrary(sbase);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (!tool_sock_resolve_af(sbase, server, family, &target))
    {
        rc = RETURN_ERROR;
        goto done;
    }

    tool_addr_text(sbase, &target, addrtext, sizeof(addrtext));

    sock = sock_socket(sbase, (LONG)target.ta_Family, SNTP_SOCK_DGRAM, 0);
    if (sock < 0)
    {
        tool_error("no socket was available");
        rc = RETURN_FAIL;
        goto done;
    }

    {
        ToolSockAddrAny sa;
        LONG            salen = tool_sock_addr(&sa, &target, (UWORD)SNTP_PORT);

        /*
         * connect() on a datagram socket sets the peer, so the stack drops
         * every datagram that did not come from the server.
         */
        if (sock_connect(sbase, sock, &sa, salen) != 0)
        {
            tool_error("cannot reach %s (errno %ld)", (LONG)addrtext,
                       (LONG)sock_errno(sbase));
            rc = RETURN_ERROR;
            goto done;
        }
    }

    reply.stratum       = 0;
    reply.transmit_secs = 0;
    reply.transmit_frac = 0;
    reply.round_trip_us = 0;

    clock_get(&old_secs, &old_micro);

    if (!sntp_exchange(sbase, sock, timeout, addrtext, &reply))
    {
        rc = tool_break() ? RETURN_WARN : RETURN_ERROR;
        goto done;
    }

    /*
     * The server's transmit timestamp plus half the round trip is the time now.
     * The four-timestamp offset formula overflows 32 bits when the local clock
     * is 48 years out, which is the case this command is for.
     */
    new_utc   = reply.transmit_secs - SNTP_NTP_TO_AMIGA;
    new_micro = frac_to_micro(reply.transmit_frac) + reply.round_trip_us / 2UL;
    while (new_micro >= 1000000UL)
    {
        new_micro -= 1000000UL;
        new_utc++;
    }

    have_locale = locale_gmt_offset(&gmt_west);

    /*
     * Local time is UTC minus the offset west of Greenwich, and the Amiga clock
     * holds local time. The clamp covers a UTC value small enough for the
     * subtraction to wrap.
     */
    if (have_locale && gmt_west != 0)
    {
        LONG shifted = (LONG)new_utc - (gmt_west * 60L);

        new_local = (shifted > 0) ? (ULONG)shifted : new_utc;
    }
    else
    {
        new_local = new_utc;
    }

    if (!quiet)
    {
        tool_printf("%s (%s): stratum %lu, round trip %lu ms\n",
                    (LONG)server, (LONG)addrtext, (ULONG)reply.stratum,
                    reply.round_trip_us / 1000UL);
    }

    /* How far out this machine was, before anything is changed. */
    {
        ULONG behind;
        BOOL  slow;

        if (new_local >= old_secs)
        {
            behind = new_local - old_secs;
            slow   = TRUE;
        }
        else
        {
            behind = old_secs - new_local;
            slow   = FALSE;
        }

        if (!quiet)
        {
            if (behind < 2)
            {
                tool_printf("This machine's clock was already right.\n");
            }
            else
            {
                tool_printf("This machine's clock was ");
                print_span(behind);
                tool_printf(" %s.\n", (LONG)(slow ? "slow" : "fast"));
            }
        }
    }

    format_amiga_time(new_local, &when);

    if (show)
    {
        tool_printf("The time is %s %s %s.\n",
                    (LONG)when.day, (LONG)when.date, (LONG)when.time);
        goto done;
    }

    clock_set(new_local, new_micro);

    if (!quiet)
    {
        if (!have_locale || gmt_west == 0)
        {
            tool_printf("Clock set to %s %s %s, UTC.\n",
                        (LONG)when.day, (LONG)when.date, (LONG)when.time);

            if (!have_locale)
                tool_printf("This machine has no locale.library, so nothing "
                            "here knows its timezone.\n");
        }
        else
        {
            LONG mins = (gmt_west < 0) ? -gmt_west : gmt_west;

            tool_printf("Clock set to %s %s %s, local time (GMT%s%ld:%02ld).\n",
                        (LONG)when.day, (LONG)when.date, (LONG)when.time,
                        (LONG)(gmt_west < 0 ? "+" : "-"),
                        (LONG)(mins / 60L), (LONG)(mins % 60L));
        }
    }

    if (battclock_write(new_local))
    {
        if (!quiet)
            tool_printf("The battery-backed clock was set too, so the time "
                        "survives a reboot.\n");
    }
    else if (!quiet)
    {
        tool_printf("This machine has no battery-backed clock, so the time "
                    "will be\nlost at the next reboot.\n");
    }

done:
    if (sock >= 0)
        (VOID)sock_close(sbase, sock);

    clock_close();
    CloseLibrary(sbase);
    FreeArgs(rda);

    if (rc == RETURN_OK && tool_break())
    {
        tool_fault(ERROR_BREAK);
        return RETURN_WARN;
    }

    return (int)rc;
}
