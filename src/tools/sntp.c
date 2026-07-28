/*
 * sntp -- set the system clock from a time server.
 *
 *     sntp SERVER/A,TIMEOUT/N/K,SHOW/S,QUIET/S
 *
 *   tls.library skips a certificate's notBefore/notAfter entirely when the
 *   machine's clock is outside a plausible window (src/tlslib/tls_time.c).
 *   That is deliberate and it is the right call on an Amiga whose battery died
 *   in 2003 -- the alternative is a machine that cannot reach any HTTPS site at
 *   all -- but the price is exact: an EXPIRED certificate is accepted.  Put the
 *   clock right and the check comes back, which is the single biggest thing a
 *   user can do to what this stack actually verifies.  `fetch` says which of
 *   the two happened on every https: URL, so the effect is visible.
 *
 *   RFC 4330 has both.  Broadcast means sitting and waiting for a server on
 *   the LAN to announce the time whenever it feels like it -- NetX Duo's own
 *   client allows two hours between announcements -- which no Shell command
 *   can do, and it means believing whatever on the LAN claims to be a time
 *   server.  Unicast asks a server the user named and gets an answer or a
 *   timeout.  That is the only one of the two that suits a command you type.
 *
 * THE EPOCHS.  There are three of them
 *
 *   NTP counts seconds from 1900-01-01, UNIX from 1970-01-01, and AmigaOS from
 *   1978-01-01.  Only the first and last matter here, and the gap between them
 *   is 2461449600 seconds (2208988800 from 1900 to 1970, which is the number
 *   everyone knows, plus 252460800 from 1970 to 1978, which is the one
 *   src/tlslib/tls_time.c already carries).
 *
 *   NTP's 32-bit seconds field wraps in February 2036.  A plain 32-bit
 *   unsigned subtraction of the constant above gets the NEXT era right without
 *   any special case, because the wrap in the server's field and the wrap in
 *   our arithmetic are the same wrap -- (t + 2^32) - K == t - K, modulo 2^32.
 *   The AmigaOS epoch runs out in 2114, which is when this stops being true.
 *
 * LOCAL TIME, WHICH IS A Decision and not an oversight
 *
 *   NTP is UTC.  AmigaOS keeps LOCAL time: DateStamp() has no timezone concept
 *   whatever, and neither has the battery clock.  So writing the clock needs an
 *   offset, and the question is where it comes from.
 *
 *   Not from a new configuration file.  Nothing in DEVS:Internet or
 *   DEVS:NetInterfaces has ever known about time, NetSetup has never asked,
 *   and inventing an eleventh place to configure the machine to serve one
 *   command would be the wrong answer -- the machine already knows.
 *   locale.library has held loc_GMTOffset since AmigaOS 2.1, the Locale
 *   preferences editor is where a user sets it, and every other program that
 *   cares reads it there.  So this reads it there too.
 *
 *   When locale.library is absent (a bare 3.1 install may not have it) or the
 *   offset is zero, the clock is set to UTC and the report says so, because a
 *   machine three hours out is worth mentioning and silently pretending
 *   otherwise is not.  AmigaOS has no daylight-saving rules of any kind: the
 *   offset in the preferences is the whole answer, summer and winter alike.
 *
 *   Setting only the system time loses it at the next reboot, which on a
 *   machine that has just been given a correct clock is the one thing that
 *   must not happen -- SetClock SAVE exists for exactly this reason.  So both
 *   are written: timer.device's TR_SETSYSTIME for the running system, and
 *   battclock.resource for the next boot.  A machine with no real-time clock
 *   has no battclock.resource, OpenResource() returns NULL, and that half is
 *   skipped and said out loud.
 *
 *   NetX Duo vendors an SNTP client at third_party/netxduo/addons/sntp, and it
 *   is the obvious thing to use.  It cannot be used from a Shell command, for
 *   a reason that has nothing to do with SNTP:
 *
 *     A command links its OWN copy of ThreadX and NetX Duo, and that copy's
 *     kernel is not running -- the one that is runs inside bsdsocket.library.
 *     nx_sntp_client_create() calls tx_thread_create(), tx_timer_create() and
 *     tx_mutex_create(); nx_sntp_client_run_unicast() calls
 *     nx_udp_socket_bind(), which suspends the calling thread.  Every one of
 *     those touches ThreadX's scheduler globals, and in a Shell command those
 *     globals belong to a kernel that was never entered.  The same fact is why
 *     netstat and ping cannot read the live NX_IP either (netstack_weak.c), and
 *     why NetTrace reaches the capture engine through published bpf_* LVOs
 *     rather than by linking src/bpf.
 *
 *   What a command DOES have is bsdsocket.library, and SNTP over a connected
 *   UDP socket is 48 bytes out and 48 bytes back.  The checks RFC 4330 5
 *   requires of a client are all here and are named where they are made.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

#include <exec/io.h>
#include <devices/timer.h>
#include <dos/datetime.h>
#include <resources/battclock.h>
#include <libraries/locale.h>
#include <proto/battclock.h>
#include <proto/locale.h>

const char *const tool_name = "sntp";

/*
 * The library bases the two inline sets above expect to find.  Both are opened
 * and dropped inside the one function that uses them, so neither is live for
 * more than the few instructions it takes to read a number.
 */
struct Library    *BattClockBase;
struct LocaleBase *LocaleBase;

static const char version_tag[] __attribute__((used)) =
    "$VER: sntp 1.0 (26.7.2026)";

#define TEMPLATE    "SERVER/A,TIMEOUT/N/K,SHOW/S,QUIET/S"

enum
{
    ARG_SERVER = 0,
    ARG_TIMEOUT,
    ARG_SHOW,
    ARG_QUIET,
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
 * Through the LVOs, for the reason fetch.c gives: a command must not link the
 * whole socket surface to send one datagram, and the NDK inlines assume a
 * global SocketBase.  These are the six vectors this command needs; the
 * offsets are docs/RESEARCH.md 3.2's.
 */

struct SntpSockAddr
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;               /* network order == our order on m68k */
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
};

struct SntpHostEnt
{
    char   *h_name;
    char  **h_aliases;
    LONG    h_addrtype;
    LONG    h_length;
    char  **h_addr_list;
};

struct SntpTimeval
{
    LONG    tv_secs;
    LONG    tv_micro;
};

#define SNTP_AF_INET        2
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

static struct SntpHostEnt *sock_gethostbyname(struct Library *base,
                                              const char *name)
{
    register struct Library    *a6  __asm("a6") = base;
    register const char        *a0  __asm("a0") = name;
    register struct SntpHostEnt *res __asm("d0");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-210:W)"
                      : "=r" (res), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "d1", "a1", "cc", "memory");
    return res;
}

/* ------------------------------------------------------ the machine's clock */

/*
 * timer.device through exec.library alone -- a timerequest and DoIO(), with no
 * TimerBase to collide with anything else the command links.  tests/tls/
 * tls_api.c does the same thing to push the clock back to 1978 on purpose.
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
 * The battery-backed clock, which is what survives the power switch.  It holds
 * the same "seconds since 1978, local time" the system clock does; a machine
 * without a real-time chip -- a bare A500 or A1200 -- has no resource at all
 * and this returns FALSE.
 *
 * Through the NDK inlines rather than by hand.  fetch.c and toolsock.c
 * hand-code bsdsocket.library's LVOs on purpose, because the ABI is the thing
 * those commands exist to demonstrate; nothing of the kind is true of a
 * resource that has had three functions since 1985.
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
 * Minutes west of Greenwich, from the Locale preferences.  FALSE when
 * locale.library is not on the machine, in which case nothing here knows the
 * timezone and UTC is the only defensible answer.
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

/* ------------------------------------------------------------- formatting -- */

/*
 * dos.library's own date formatter, so the time reads exactly like every other
 * date on the machine -- and in the user's own Locale, which is the point of
 * having set the clock to local time in the first place.  DateToStr() is V36
 * and the floor is V40.
 */
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
 * "3 days 4 hours", "12 minutes", "6 seconds" -- the two largest units that
 * are worth saying and no more.  Printed rather than formatted into a buffer,
 * because dos.library's only formatter is VPrintf and it writes to a file.
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
 * An SNTP message is 48 bytes and every multi-byte field is big-endian, which
 * on m68k means the bytes are already in the right order -- so a cast would
 * work and is still not used.  Byte at a time costs four instructions, says on
 * its face what the wire format is, and does not quietly become wrong if any
 * of this is ever read on something little-endian.
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
 * NTP's fraction field is a binary fraction of a second; timer.device wants
 * microseconds.  The exact conversion is frac * 1000000 / 2^32, which needs 64
 * bits, and 64-bit division on a 68020 is a libgcc call for a number whose
 * bottom sixteen bits are noise anyway.  Dropping those first leaves
 * g * 15625 / 1024 with g < 65536, which is exact in 32 bits and resolves to
 * about 15 microseconds -- four orders of magnitude finer than the round trip
 * it is added to.
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
 * What RFC 4330 5 tells a client to check, and nothing more:
 *
 *   mode 4          it is a server reply and not somebody else's request
 *   LI != 3         the server says its own clock is not synchronised
 *   stratum 1..15   0 is a kiss-o'-death, 16+ is unsynchronised
 *   transmit != 0   a server that has never had the time sends zero
 *   originate       must be the timestamp we put in our request, which is
 *                   what stops a stale or spoofed reply being believed
 *
 * The source address needs no check: the socket is connect()ed, so the stack
 * has already dropped anything that did not come from the server.
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
        return "the server speaks an NTP version this does not";
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
        return "the server has never been given the time itself";

    return NULL;
}

/* ------------------------------------------------------------------ main --- */

static LONG arg_or(const LONG *args, int index, LONG fallback)
{
    const LONG *p = (const LONG *)args[index];

    return (p != NULL) ? *p : fallback;
}

/*
 * One request, one reply.  FALSE after saying what went wrong; `where` is the
 * server's address, so that a failure names the machine it was talking to
 * rather than "the time server".
 *
 * Up to SNTP_ATTEMPTS requests go out, sharing TIMEOUT between them, because a
 * lost datagram is the ordinary case on UDP and one dropped request should not
 * cost the user the whole command.
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
         * The transmit timestamp we send is our own clock, which may be
         * decades out -- that does not matter.  Its only job is to come back
         * in the originate field so a reply can be matched to a request, and
         * RFC 4330 says a client may use any unique value.  The microsecond
         * reading is used raw as the fraction for that reason: it is unique
         * and nothing on either end has to interpret it.
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
            tool_error("could not send the request to %s (errno %ld)",
                       (LONG)where, (LONG)sock_errno(sbase));
            return FALSE;
        }

        /*
         * Wait in one-second slices so Ctrl-C is noticed promptly: WaitSelect
         * is given no signal mask, because a break during a one-second wait is
         * indistinguishable from a break a moment after it.
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
                    tool_error("the time server's reply could not be read");
                    return FALSE;
                }

                bad = sntp_validate(msg, got, sent_secs, sent_frac, reply);
                if (bad != NULL)
                {
                    /*
                     * A bad reply is not a reason to give up: on a busy
                     * network it may be a stale datagram from a previous run.
                     * Say so once and go round again.
                     */
                    tool_error("%s", (LONG)bad);
                    break;
                }

                /*
                 * The round trip, by our own clock.  Both readings come from
                 * timer.device before anything is changed, so the decades the
                 * clock may be out cancel exactly.
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
    tool_advise_blank();
    tool_advise("Nothing came back on UDP port 123.  Check that the name is a");
    tool_advise("time server, that this machine can reach it -- try  ping  --");
    tool_advise("and that nothing between the two is blocking NTP.");

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
    ULONG           target = 0;
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
    char            addrtext[16];
    SntpDateText    when;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    args[ARG_SERVER]  = 0;
    args[ARG_TIMEOUT] = 0;
    args[ARG_SHOW]    = 0;
    args[ARG_QUIET]   = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("<time server>",
                   "The name or address of an SNTP server, e.g. pool.ntp.org.");
        return RETURN_ERROR;
    }

    server  = (const char *)args[ARG_SERVER];
    timeout = (ULONG)arg_or(args, ARG_TIMEOUT, (LONG)SNTP_DEFAULT_TIMEOUT);
    show    = (args[ARG_SHOW] != 0) ? TRUE : FALSE;
    quiet   = (args[ARG_QUIET] != 0) ? TRUE : FALSE;

    if (timeout == 0)
        timeout = SNTP_DEFAULT_TIMEOUT;

    /*
     * Opening bsdsocket.library is what starts the network on a machine where
     * nothing has yet -- the same contract AddNetInterface relies on -- so a
     * user who has configured an interface and not started it still gets a
     * clock rather than a lecture.
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
        tool_error("timer.device would not open, so the clock cannot be read");
        CloseLibrary(sbase);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    /* A dotted quad is used as it stands; anything else goes to the resolver. */
    if (!ami_config_parse_ip(server, &target))
    {
        struct SntpHostEnt *he = sock_gethostbyname(sbase, server);

        /*
         * h_length is checked as well as the pointers, and that is not
         * belt-and-braces: a resolver that answers with a hostent it could not
         * fill produces a plausible-looking record whose address is four bytes
         * of nothing, and the first version of this command turned that into
         * "could not send the request" -- an error about the wrong thing
         * entirely.  fetch.c checks the same three, for the same reason.
         */
        if (he == NULL || he->h_addr_list == NULL ||
            he->h_addr_list[0] == NULL || he->h_length != 4)
        {
            tool_error("cannot resolve \"%s\"", (LONG)server);
            tool_explain_resolve(server, AMI_NET_ERR_NONAME);
            rc = RETURN_ERROR;
            goto done;
        }

        target = be32((const UBYTE *)he->h_addr_list[0]);
    }

    if (target == 0)
    {
        tool_error("\"%s\" is not an address a request can be sent to",
                   (LONG)server);
        rc = RETURN_ERROR;
        goto done;
    }

    ami_config_format_ip(target, addrtext, sizeof(addrtext));

    sock = sock_socket(sbase, SNTP_AF_INET, SNTP_SOCK_DGRAM, 0);
    if (sock < 0)
    {
        tool_error("no socket was available");
        rc = RETURN_FAIL;
        goto done;
    }

    {
        struct SntpSockAddr sa;
        ULONG               i;

        sa.sin_len    = (UBYTE)sizeof(sa);
        sa.sin_family = SNTP_AF_INET;
        sa.sin_port   = (UWORD)SNTP_PORT;
        sa.sin_addr   = target;
        for (i = 0; i < 8UL; i++)
            sa.sin_zero[i] = 0;

        /*
         * connect() on a datagram socket sets the peer, which is what makes
         * send/recv usable -- and makes the stack drop every datagram that did
         * not come from the server, so no source check is needed here.
         */
        if (sock_connect(sbase, sock, &sa, (LONG)sizeof(sa)) != 0)
        {
            tool_error("could not reach %s (errno %ld)", (LONG)addrtext,
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
     * The server's transmit timestamp plus half the round trip is the time
     * NOW.  The textbook four-timestamp offset formula is deliberately not
     * used: it is for disciplining a clock that is already close, and it
     * overflows 32 bits outright when the local clock is 48 years out, which
     * is exactly the machine this command exists for.
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
     * Local time is UTC minus the offset west of Greenwich, and the Amiga
     * clock holds local time.  A machine east of Greenwich has a negative
     * offset, so this adds; the clamp is for the absurd case of a UTC value so
     * small that the subtraction would wrap, which only happens if a server
     * hands out 1978.
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
            tool_printf("The battery-backed clock was set too, so this "
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
