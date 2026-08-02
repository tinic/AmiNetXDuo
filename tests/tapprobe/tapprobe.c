/*
 * tapprobe -- point a foreign TCP/IP stack at our synthetic SANA-II device and
 * write down what it does with it.
 *
 *   tapprobe [anxd|roadshow] [command-to-bring-the-interface-up]
 *
 * The device has to be in ExecBase's device list before the stack looks for
 * it, and it lives in this program's address space, so this program installs
 * it and then starts the stack itself rather than the other way round.  For
 * Roadshow that means Execute()ing AddNetInterface; for our own library it
 * means opening it, because it configures its interfaces on open.
 *
 * What is measured, and why the shape of each measurement:
 *
 *   alignment   The device chooses where the frame sits, so &frame[14] --
 *               the pointer S2_CopyToBuff receives -- can be put on any of
 *               the four residues.  Eight of nine real Amiga drivers hand
 *               over 2 mod 4.  Each residue is exercised with an ICMP echo
 *               request whose payload is a known pattern; the echo reply
 *               comes back through CMD_WRITE and is compared byte for byte,
 *               so "it coped" means the bytes arrived, not that nothing
 *               crashed.
 *
 *   depth       Raising this task above the stack's own stops it re-arming
 *               while frames are being pushed in, so frames can be injected
 *               until the device runs out of queued CMD_READs.  The count is
 *               then the real queue depth rather than a race with the
 *               reader.  The frames used carry a deliberately wrong IPv4
 *               header checksum: they consume a read and produce no reply,
 *               so the stack does no work that would distort the next one.
 *
 *   re-arm      The event ring records BeginIO(CMD_READ) and every reply with
 *               the IORequest pointer, so a replacement read is identifiable
 *               as the same request coming back, and its position relative to
 *               the CMD_WRITE carrying the answer says whether the stack
 *               re-arms before or after it processes the frame.
 *
 *   copy cost   The E-Clock is read either side of the stack's own hook.  A
 *               plain longword copy of the same length, timed here with the
 *               same clock, is the scale: a hook that only moves bytes cannot
 *               cost much more than that, and one that folds a checksum in
 *               must cost more.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdarg.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "sana2_device.h"
#include "sana2_r3_tags.h"

#include "probedev.h"

/* ------------------------------------------------------------- the wire --- */

#define ETH_HDR         14

static const UBYTE mac_amiga[6] = { 0x02, 0x00, 0x00, 0x0A, 0x09, 0x01 };
static const UBYTE mac_peer[6]  = { 0x02, 0x00, 0x00, 0x0A, 0x09, 0x02 };

/* 10.9.9.1 is the Amiga, 10.9.9.2 the peer -- see DEVS:NetInterfaces/tap0. */
static const UBYTE ip_amiga[4]  = { 10, 9, 9, 1 };
static const UBYTE ip_peer[4]   = { 10, 9, 9, 2 };

#define ET_IP           0x0800
#define ET_ARP          0x0806

#define ECHO_PAYLOAD    1024        /* big enough for the copy cost to show */
#define SMALL_PAYLOAD   64          /* the other end of the two-point fit   */
#define REPS            8           /* injections per alignment             */

/* ------------------------------------------------------------- reporting -- */

static BPTR out_file;

static VOID emit(const char *s, ULONG n)
{
    if (out_file != (BPTR)0)
    {
        Write(out_file, (APTR)s, (LONG)n);
        Flush(out_file);
    }
    Write(Output(), (APTR)s, (LONG)n);
}

static VOID fmt_num(char **p, ULONG v, UWORD base, UWORD width, BOOL sign)
{
    char  tmp[12];
    UWORD n = 0;
    char *o = *p;

    if (sign && (LONG)v < 0)
    {
        *o++ = '-';
        v = (ULONG)(-(LONG)v);
    }

    do
    {
        UWORD d = (UWORD)(v % base);
        tmp[n++] = (char)((d < 10) ? ('0' + d) : ('a' + d - 10));
        v /= base;
    } while (v != 0);

    while (n < width)
        tmp[n++] = '0';

    while (n-- != 0)
        *o++ = tmp[n];

    *p = o;
}

/* %s %ld %lu %lx %02x %04x %08x.  newlib's printf would pull in
   mathieeedoubbas.library on a bare boot disk. */
static VOID say(const char *fmt, ...)
{
    static char line[400];
    char       *o = line;
    const char *f = fmt;
    va_list     ap;

    va_start(ap, fmt);

    while (*f != '\0' && (ULONG)(o - line) < sizeof(line) - 16)
    {
        if (*f != '%')
        {
            *o++ = *f++;
            continue;
        }

        f++;
        {
            UWORD width = 0;

            if (*f == '-')                  /* left-align: parsed, ignored */
                f++;
            while (*f >= '0' && *f <= '9')
                width = (UWORD)(width * 10 + (UWORD)(*f++ - '0'));
            if (*f == 'l')
                f++;

            switch (*f)
            {
            case 's':
            {
                const char *s = va_arg(ap, const char *);
                while (*s != '\0' && (ULONG)(o - line) < sizeof(line) - 2)
                    *o++ = *s++;
                break;
            }
            case 'd':
                fmt_num(&o, (ULONG)va_arg(ap, LONG), 10, width, TRUE);
                break;
            case 'u':
                fmt_num(&o, va_arg(ap, ULONG), 10, width, FALSE);
                break;
            case 'x':
                fmt_num(&o, va_arg(ap, ULONG), 16, width, FALSE);
                break;
            case '%':
                *o++ = '%';
                break;
            default:
                break;
            }
            f++;
        }
    }

    va_end(ap);

    *o++ = '\n';
    emit(line, (ULONG)(o - line));
}

/* --------------------------------------------------------------- packets -- */

static VOID put16(UBYTE *p, UWORD v)
{
    p[0] = (UBYTE)(v >> 8);
    p[1] = (UBYTE)v;
}

static UWORD get16(const UBYTE *p)
{
    return (UWORD)(((UWORD)p[0] << 8) | p[1]);
}

static UWORD inet_sum(const UBYTE *p, ULONG n)
{
    ULONG sum = 0;

    while (n > 1)
    {
        sum += get16(p);
        p   += 2;
        n   -= 2;
    }
    if (n != 0)
        sum += (ULONG)p[0] << 8;

    while ((sum >> 16) != 0)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (UWORD)~sum;
}

static VOID eth_head(UBYTE *f, const UBYTE *dst, const UBYTE *src, UWORD type)
{
    UWORD i;

    for (i = 0; i < 6; i++)
    {
        f[i]     = dst[i];
        f[6 + i] = src[i];
    }
    put16(&f[12], type);
}

static ULONG build_arp(UBYTE *f, UWORD op, const UBYTE *tha)
{
    UBYTE *a = &f[ETH_HDR];
    UWORD  i;

    eth_head(f, (op == 1) ? (const UBYTE *)"\xFF\xFF\xFF\xFF\xFF\xFF"
                          : mac_amiga,
             mac_peer, ET_ARP);

    put16(&a[0], 1);
    put16(&a[2], ET_IP);
    a[4] = 6;
    a[5] = 4;
    put16(&a[6], op);
    for (i = 0; i < 6; i++) a[8 + i] = mac_peer[i];
    for (i = 0; i < 4; i++) a[14 + i] = ip_peer[i];
    for (i = 0; i < 6; i++) a[18 + i] = (tha != NULL) ? tha[i] : 0;
    for (i = 0; i < 4; i++) a[24 + i] = ip_amiga[i];

    return ETH_HDR + 28;
}

/*
 * An ICMP echo request whose payload is `seed`-derived, so the reply can be
 * checked byte for byte.  `bad_sum` corrupts the IPv4 header checksum, which
 * makes the stack drop the frame after it has copied it -- one read consumed,
 * nothing sent back.
 */
static ULONG build_echo(UBYTE *f, UWORD seq, UBYTE seed, ULONG payload,
                        BOOL bad_sum)
{
    UBYTE *ip   = &f[ETH_HDR];
    UBYTE *icmp = &ip[20];
    ULONG  i;

    eth_head(f, mac_amiga, mac_peer, ET_IP);

    ip[0] = 0x45;
    ip[1] = 0;
    put16(&ip[2], (UWORD)(20 + 8 + payload));
    put16(&ip[4], seq);
    put16(&ip[6], 0);
    ip[8] = 64;
    ip[9] = 1;                          /* ICMP */
    put16(&ip[10], 0);
    for (i = 0; i < 4; i++) ip[12 + i] = ip_peer[i];
    for (i = 0; i < 4; i++) ip[16 + i] = ip_amiga[i];
    put16(&ip[10], inet_sum(ip, 20));
    if (bad_sum)
        ip[10] ^= 0xFF;

    icmp[0] = 8;                        /* echo request */
    icmp[1] = 0;
    put16(&icmp[2], 0);
    put16(&icmp[4], 0x1234);
    put16(&icmp[6], seq);
    for (i = 0; i < payload; i++)
        icmp[8 + i] = (UBYTE)(seed + (UBYTE)(i * 7));
    put16(&icmp[2], inet_sum(icmp, 8 + payload));

    return ETH_HDR + 20 + 8 + payload;
}

/* ---------------------------------------------------------------- arena --- */

/*
 * One frame buffer, placed so that &frame[14] lands on a chosen residue.
 * The allocation is longword aligned, so k = (align + 2) mod 4 puts the
 * payload where we want it.
 */
static UBYTE *arena;

static UBYTE *frame_at(UWORD align)
{
    return arena + ((align + 2) & 3);
}

/* ------------------------------------------------------------- the pump --- */

static UBYTE  txbuf[PROBE_FRAME_MAX];
static ULONG  arp_answered;

static BOOL echo_reply_ok(ULONG n, UWORD seq, UBYTE seed, ULONG payload,
                          ULONG *bad_out);

/*
 * Collect one transmitted frame of `want` type, answering any ARP request the
 * stack makes for the peer on the way -- otherwise the first echo request
 * stalls behind an address resolution nobody replies to.
 * `ticks` are 50ths of a second.  Returns the length, or 0 on timeout.
 */
static ULONG wait_tx(UWORD want, ULONG ticks)
{
    ULONG waited;

    for (waited = 0; waited <= ticks; waited++)
    {
        ULONG n;

        while ((n = probe_tx_get(txbuf, sizeof(txbuf))) != 0)
        {
            UWORD type = get16(&txbuf[12]);

            if (type == ET_ARP && n >= ETH_HDR + 28 &&
                get16(&txbuf[ETH_HDR + 6]) == 1)
            {
                UBYTE *reply = frame_at(2);
                ULONG  rlen  = build_arp(reply, 2, mac_amiga);

                (VOID)probe_rx_put(reply, rlen);
                arp_answered++;
                if (want == ET_ARP)
                    return n;
                continue;
            }

            if (want == 0 || type == want)
                return n;
        }

        Delay(1);
    }

    return 0;
}

/*
 * Wait for the echo reply belonging to `seq`, skipping anything else the stack
 * emits in the meantime.  Matching on the sequence rather than taking the next
 * frame off the ring: one straggler from an earlier step otherwise shifts every
 * later comparison by one and reports the whole run as corrupt.
 */
static BOOL wait_echo(UWORD seq, UBYTE seed, ULONG payload, ULONG ticks,
                      ULONG *bad_out)
{
    ULONG waited;

    *bad_out = payload;

    for (waited = 0; waited <= ticks; waited++)
    {
        ULONG n;

        while ((n = probe_tx_get(txbuf, sizeof(txbuf))) != 0)
        {
            UWORD type = get16(&txbuf[12]);

            if (type == ET_ARP && n >= ETH_HDR + 28 &&
                get16(&txbuf[ETH_HDR + 6]) == 1)
            {
                UBYTE *reply = frame_at(2);

                (VOID)probe_rx_put(reply, build_arp(reply, 2, mac_amiga));
                arp_answered++;
                continue;
            }
            if (type != ET_IP || n < ETH_HDR + 28)
                continue;
            if (txbuf[ETH_HDR + 9] != 1 || txbuf[ETH_HDR + 20] != 0)
                continue;                       /* not an echo reply */
            if (get16(&txbuf[ETH_HDR + 26]) != seq)
                continue;                       /* an older one */

            return echo_reply_ok(n, seq, seed, payload, bad_out);
        }

        Delay(1);
    }

    return FALSE;
}

/* Is txbuf an ICMP echo reply carrying the pattern `seed` for `payload`? */
static BOOL echo_reply_ok(ULONG n, UWORD seq, UBYTE seed, ULONG payload,
                          ULONG *bad_out)
{
    const UBYTE *ip   = &txbuf[ETH_HDR];
    const UBYTE *icmp = &ip[20];
    ULONG        i, bad = 0;

    *bad_out = 0;

    if (n < ETH_HDR + 20 + 8 + payload)
        return FALSE;
    if (get16(&txbuf[12]) != ET_IP || ip[9] != 1 || icmp[0] != 0)
        return FALSE;
    if (get16(&icmp[6]) != seq)
        return FALSE;

    for (i = 0; i < payload; i++)
        if (icmp[8 + i] != (UBYTE)(seed + (UBYTE)(i * 7)))
            bad++;

    *bad_out = bad;
    return (bad == 0) ? TRUE : FALSE;
}

/* ------------------------------------------------------ the instrument --- */

/*
 * A copy is timed as ReadEClock, hook, ReadEClock, so one whole ReadEClock
 * sits inside every measurement -- and it is not small: it goes through
 * timer.device and reads CIA registers that are E-Clock synchronised.  Left
 * in, it swamps a short frame.  Measured once here and subtracted, so what is
 * reported is the hook.
 */
static ULONG ecl_overhead;

static ULONG measure_overhead(VOID)
{
    ULONG best = 0xFFFFFFFFUL;
    ULONG i;

    for (i = 0; i < 64; i++)
    {
        ULONG a = probe_eclock_now();
        ULONG b = probe_eclock_now();

        if (b - a < best)
            best = b - a;
    }
    return (best == 0xFFFFFFFFUL) ? 0 : best;
}

static ULONG ns_per_byte(ULONG ticks, ULONG len)
{
    ULONG rate = probe_eclock_rate();

    if (rate == 0 || len == 0 || ticks <= ecl_overhead)
        return 0;

    return ((ticks - ecl_overhead) * 1000000UL) / ((rate / 1000UL) * len);
}

/* --------------------------------------------------- a copy for the scale - */

/* The cheapest thing the hook could possibly be doing, timed the same way. */
static VOID copy_lw(UBYTE *to, const UBYTE *from, ULONG len)
{
    ULONG *d = (ULONG *)to;
    const ULONG *s = (const ULONG *)from;
    ULONG  n = len >> 2;

    while (n-- != 0)
        *d++ = *s++;
}

/* ---------------------------------------------------------------- events -- */

static ProbeEvent evbuf[PROBE_EVENTS];

static const char *ev_name(UBYTE k)
{
    switch (k)
    {
    case PEV_OPEN:       return "OPEN";
    case PEV_READ_IN:    return "READ+";
    case PEV_READ_REPLY: return "READ-";
    case PEV_COPY:       return "COPY";
    case PEV_WRITE:      return "WRITE";
    case PEV_NOREADER:   return "DROP";
    case PEV_CMD:        return "CMD";
    case PEV_ONLINE:     return "ONLINE";
    case PEV_OFFLINE:    return "OFFLINE";
    case PEV_CLOSE:      return "CLOSE";
    case PEV_ABORT:      return "ABORT";
    }
    return "?";
}

static VOID dump_events(ULONG last)
{
    ULONG n = probe_events(evbuf, PROBE_EVENTS);
    ULONG i = (n > last) ? (n - last) : 0;
    ULONG t0 = (n != 0) ? evbuf[i].t : 0;

    say("");
    say("-- event ring, last %u of %u (t in E-Clock ticks from the first) --",
        n - i, n);
    say("   %10s  %-7s %5s  %8s  %8s  %s", "t", "kind", "type", "a", "b",
        "aux");

    for (; i < n; i++)
    {
        ProbeEvent *e = &evbuf[i];

        say("   %10u  %-7s %04x  %08x  %8u  %u",
            e->t - t0, ev_name(e->kind), (ULONG)e->type, e->a, e->b,
            (ULONG)e->aux);
    }
}

/* ------------------------------------------------------------------ tags -- */

static const char *tag_name(ULONG tag)
{
    switch (tag)
    {
    case S2_CopyToBuff:     return "S2_CopyToBuff";
    case S2_CopyFromBuff:   return "S2_CopyFromBuff";
    case S2_PacketFilter:   return "S2_PacketFilter";
    case S2_CopyToBuff16:   return "S2_CopyToBuff16";
    case S2_CopyFromBuff16: return "S2_CopyFromBuff16";
    case S2_CopyToBuff32:   return "S2_CopyToBuff32";
    case S2_CopyFromBuff32: return "S2_CopyFromBuff32";
    case S2_DMACopyToBuff32:   return "S2_DMACopyToBuff32";
    case S2_DMACopyFromBuff32: return "S2_DMACopyFromBuff32";
    }
    return "(unknown)";
}

/* ---------------------------------------------------------------- main ---- */

static ULONG min_ticks(const ULONG *v, ULONG n)
{
    ULONG best = 0xFFFFFFFFUL;
    ULONG i;

    for (i = 0; i < n; i++)
        if (v[i] < best)
            best = v[i];

    return (best == 0xFFFFFFFFUL) ? 0 : best;
}

/* The COPY event most recently recorded, so a measurement can be attributed
   to the injection that caused it. */
static BOOL last_copy(ULONG *ticks, ULONG *len, UBYTE *align)
{
    ULONG n = probe_events(evbuf, PROBE_EVENTS);

    while (n-- != 0)
    {
        if (evbuf[n].kind == PEV_COPY)
        {
            *ticks = evbuf[n].a;
            *len   = evbuf[n].b;
            *align = evbuf[n].aux;
            return TRUE;
        }
    }
    return FALSE;
}

/*
 * One alignment, one payload size: inject REPS echo requests, check every
 * reply byte for byte, and report the cheapest hook time seen.  The cheapest
 * rather than the mean because everything that perturbs it -- a task switch,
 * an interrupt -- only ever adds.
 */
static ULONG sweep_align(UWORD align, ULONG payload, ULONG *seq)
{
    ULONG ticks[REPS];
    ULONG replies = 0, badtotal = 0, used = 0;
    ULONG rep, mt;

    for (rep = 0; rep < REPS; rep++)
    {
        UBYTE *f    = frame_at(align);
        UBYTE  seed = (UBYTE)(0x40 + align * 16 + rep);
        ULONG  flen = build_echo(f, (UWORD)*seq, seed, payload, FALSE);
        ULONG  bad;
        ULONG  ct, cl;
        UBYTE  ca;

        if (probe_rx_put(f, flen) != 0)
        {
            (*seq)++;
            continue;
        }

        if (last_copy(&ct, &cl, &ca) && ca == (UBYTE)align && cl == payload + 28)
            ticks[used++] = ct;

        if (wait_echo((UWORD)*seq, seed, payload, 100, &bad))
            replies++;
        else
            badtotal += bad;

        (*seq)++;
    }

    mt = (used != 0) ? min_ticks(ticks, used) : 0;
    say("   %5u  %8u  %8u  %8u  %10u  %10u",
        (ULONG)align, payload, replies, badtotal, mt,
        ns_per_byte(mt, payload));

    return mt;
}

/*
 * DH0:mode.txt, written by the runner: the stack name on the first line and
 * the command that brings its interface up on the second.  A file rather than
 * an argument because argv does not survive the boot shell here -- the
 * Startup-Sequence carries the word and the program still sees argc == 1.
 */
static char mode_buf[256];
static char up_buf[256];

static VOID read_mode(VOID)
{
    BPTR  fh = Open((STRPTR)"DH0:mode.txt", MODE_OLDFILE);
    char  raw[512];
    LONG  n, i, w = 0;
    BOOL  second = FALSE;

    if (fh == (BPTR)0)
        return;

    n = Read(fh, raw, (LONG)sizeof(raw) - 1);
    Close(fh);
    if (n <= 0)
        return;
    raw[n] = '\0';

    for (i = 0; i < n; i++)
    {
        char c = raw[i];

        if (c == '\n' || c == '\r')
        {
            if (!second)
            {
                mode_buf[w] = '\0';
                second = TRUE;
                w = 0;
                continue;
            }
            break;
        }
        if (!second)
        {
            if (w < (LONG)sizeof(mode_buf) - 1)
                mode_buf[w++] = c;
        }
        else
        {
            if (w < (LONG)sizeof(up_buf) - 1)
                up_buf[w++] = c;
        }
    }

    if (!second)
        mode_buf[w] = '\0';
    else
        up_buf[w] = '\0';
}

static BOOL streq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;

        if (ca != cb)
            return FALSE;
        a++;
        b++;
    }
    return (*a == *b) ? TRUE : FALSE;
}

int main(int argc, char **argv)
{
    struct Library *sock = NULL;
    const char     *mode;
    const char     *up;
    ProbeStats      st;
    ULONG          *tags, *tagdata;
    ULONG           ntags, i, seq = 1;
    UWORD           align;
    ULONG           depth = 0;
    struct Task    *self;
    BYTE            oldpri;

    out_file = Open((STRPTR)"DH0:tapprobe.txt", MODE_NEWFILE);

    mode_buf[0] = 'a'; mode_buf[1] = 'n'; mode_buf[2] = 'x';
    mode_buf[3] = 'd'; mode_buf[4] = '\0';
    read_mode();
    mode = mode_buf;
    up   = (up_buf[0] != '\0') ? up_buf : NULL;

    (VOID)argc;
    (VOID)argv;

    say("==== tapprobe: %s ====", mode);

    arena = (UBYTE *)AllocMem(PROBE_FRAME_MAX + 8, MEMF_PUBLIC | MEMF_CLEAR);
    if (arena == NULL)
    {
        say("!! no memory for the frame arena");
        goto done;
    }

    /* Our own stack drives S2_ONLINE itself; Roadshow does not. */
    if (probe_install(mac_amiga, streq(mode, "anxd") ? FALSE : TRUE) != 0)
    {
        say("!! the device would not install");
        goto done;
    }
    ecl_overhead = measure_overhead();
    say("device installed as " PROBE_DEVICE_NAME
        ", E-Clock %u Hz, ReadEClock %u ticks, arena %08x",
        probe_eclock_rate(), ecl_overhead, (ULONG)arena);

    /* ------------------------------------------------ bring the stack up -- */

    if (streq(mode, "anxd"))
    {
        sock = OpenLibrary((STRPTR)"bsdsocket.library", 4);
        if (sock == NULL)
        {
            say("!! bsdsocket.library would not open");
            goto done;
        }
        say("bsdsocket.library %u.%u opened",
            (ULONG)sock->lib_Version, (ULONG)sock->lib_Revision);
    }
    else
    {
        LONG rc;

        if (up == NULL)
            up = "DH0:rs/AddNetInterface DEVS:NetInterfaces/tap0";
        say("executing: %s", up);
        rc = Execute((STRPTR)up, (BPTR)0, Output());
        say("Execute() returned %d", rc);
    }

    for (i = 0; i < 200 && !probe_is_online(); i++)
        Delay(2);

    /*
     * AddNetInterface adds and configures; on a device it does not recognise
     * it may stop short of S2_ONLINE.  Ask explicitly before giving up, and
     * print the stack's own view of the interface either way.
     */
    if (!probe_is_online() && !streq(mode, "anxd"))
    {
        say("not online after the bring-up command; trying Online");
        say("Online returned %d",
            Execute((STRPTR)"DH0:rs/Online tap0", (BPTR)0, Output()));
        for (i = 0; i < 250 && !probe_is_online(); i++)
            Delay(2);
        (VOID)Execute((STRPTR)"DH0:rs/ShowNetStatus INTERFACES", (BPTR)0,
                      Output());
    }

    if (!probe_is_online())
    {
        say("!! the interface never came online");
        probe_get_stats(&st);
        say("   opens=%u reads=%u online=%u",
            st.opens, st.reads_total, st.online_count);
        dump_events(200);
        goto done;
    }

    /* Let the stack finish arming before anything is counted. */
    Delay(50);

    /* --------------------------------------------------------- what we saw */

    probe_get_stats(&st);
    say("");
    say("-- open --");
    say("   opens                %u", st.opens);
    say("   buffer-management tags:");
    ntags = probe_tags(&tags, &tagdata);
    for (i = 0; i < ntags; i++)
        say("     %08x  %08x  %s", tags[i], tagdata[i], tag_name(tags[i]));
    say("   CopyToBuff   %08x", (ULONG)probe_hook_to());
    say("   CopyFromBuff %08x", (ULONG)probe_hook_from());
    say("   CopyToBuff16 %08x", (ULONG)probe_hook_to16());
    say("   CopyFrom16   %08x", (ULONG)probe_hook_from16());
    say("   PacketFilter %08x", (ULONG)probe_hook_filter());

    say("");
    say("-- outstanding CMD_READs, settled --");
    say("   total          %u", probe_reads_for(0));
    say("   IPv4 (0800)    %u", probe_reads_for(ET_IP));
    say("   ARP  (0806)    %u", probe_reads_for(ET_ARP));
    say("   ever posted    %u", st.reads_total);
    say("   high water     %u", st.reads_max);
    say("   raw (SANA2IOF_RAW) %u", st.raw_reads);

    /* ------------------------------------------------------------- ARP --- */

    {
        UBYTE *f    = frame_at(2);
        ULONG  flen = build_arp(f, 1, NULL);
        ULONG  n;

        say("");
        say("-- ARP request in at src align 2 --");
        if (probe_rx_put(f, flen) != 0)
            say("   !! no reader took it");
        n = wait_tx(ET_ARP, 100);
        if (n == 0)
            say("   !! no ARP reply within 2 s");
        else
            say("   ARP reply, %u bytes, op %u", n,
                (ULONG)get16(&txbuf[ETH_HDR + 6]));
    }

    /* ------------------------------------------------- alignment sweep --- */

    {
        ULONG big[4], small[4];

        say("");
        say("-- ICMP echo, %u reps per source alignment, payload byte for "
            "byte --", (ULONG)REPS);
        say("   %5s  %8s  %8s  %8s  %10s  %10s", "align", "bytes", "replies",
            "badbytes", "min ticks", "ns/byte");

        for (align = 0; align < 4; align++)
            big[align] = sweep_align(align, ECHO_PAYLOAD, &seq);
        for (align = 0; align < 4; align++)
            small[align] = sweep_align(align, SMALL_PAYLOAD, &seq);

        /*
         * Two payload sizes, so the per-byte cost falls out of the difference
         * and neither ReadEClock nor any fixed cost inside the hook is in it.
         */
        say("");
        say("   per-byte cost from the two sizes (no subtraction needed):");
        for (align = 0; align < 4; align++)
        {
            ULONG d = (big[align] > small[align])
                    ? (big[align] - small[align]) : 0;
            ULONG rate = probe_eclock_rate();
            ULONG nsb  = (rate != 0 && d != 0)
                       ? (d * 1000000UL) /
                         ((rate / 1000UL) * (ECHO_PAYLOAD - SMALL_PAYLOAD))
                       : 0;

            say("     align %u: %u ticks over %u bytes = %u ns/byte",
                (ULONG)align, d, (ULONG)(ECHO_PAYLOAD - SMALL_PAYLOAD), nsb);
        }
    }

    /* ------------------------------------------------- the scale for it --- */

    {
        ULONG t[REPS];
        ULONG rep, mt;
        UBYTE *dst = (UBYTE *)AllocMem(ECHO_PAYLOAD + 8,
                                       MEMF_PUBLIC | MEMF_CLEAR);

        if (dst != NULL)
        {
            for (rep = 0; rep < REPS; rep++)
            {
                ULONG a = probe_eclock_now();
                copy_lw(dst, arena, ECHO_PAYLOAD);
                t[rep] = probe_eclock_now() - a;
            }
            mt = min_ticks(t, REPS);
            say("   %5s  %8s  %8s  %10u  %10u   plain C longword loop",
                "-", "-", "-", mt, ns_per_byte(mt, ECHO_PAYLOAD));
            FreeMem(dst, ECHO_PAYLOAD + 8);
        }
        say("   ReadEClock costs %u ticks and is already subtracted above",
            ecl_overhead);
    }

    /* ---------------------------------------------------------- poison --- */

    say("");
    say("-- source buffer scribbled on the instant the hook returns --");
    probe_set_poison(TRUE);
    for (align = 0; align < 4; align++)
    {
        UBYTE *f = frame_at(align);
        UBYTE  seed = (UBYTE)(0xA0 + align);
        ULONG  flen = build_echo(f, (UWORD)seq, seed, ECHO_PAYLOAD, FALSE);
        ULONG  bad = 0;

        if (probe_rx_put(f, flen) == 0)
        {
            if (wait_echo((UWORD)seq, seed, ECHO_PAYLOAD, 100, &bad))
                say("   align %u: reply intact", (ULONG)align);
            else
                say("   align %u: no intact reply (%u bad bytes)",
                    (ULONG)align, bad);
        }
        else
        {
            say("   align %u: no reader", (ULONG)align);
        }
        seq++;
    }
    probe_set_poison(FALSE);

    /* ----------------------------------------------------- queue depth --- */

    say("");
    say("-- queue depth, this task above the stack --");

    self   = FindTask(NULL);
    oldpri = self->tc_Node.ln_Pri;
    (VOID)SetTaskPri(self, 40);

    {
        ULONG before = probe_reads_for(ET_IP);
        ULONG n;

        for (n = 0; n < 400; n++)
        {
            UBYTE *f    = frame_at(2);
            ULONG  flen = build_echo(f, (UWORD)seq, 0x11, 64, TRUE);

            seq++;
            if (probe_rx_put(f, flen) != 0)
                break;
            depth++;
        }

        (VOID)SetTaskPri(self, oldpri);

        say("   IPv4 reads before   %u", before);
        say("   frames accepted     %u", depth);
        say("   IPv4 reads after    %u", probe_reads_for(ET_IP));
    }

    /* How long the stack takes to put them all back. */
    {
        ULONG t0 = probe_eclock_now();
        ULONG back = 0;

        for (i = 0; i < 250; i++)
        {
            back = probe_reads_for(ET_IP);
            if (back >= depth)
                break;
            Delay(1);
        }
        say("   refilled to         %u after %u E-Clock ticks", back,
            probe_eclock_now() - t0);
    }

    /* ------------------------------------------------- re-arm ordering --- */

    say("");
    say("-- one echo request, drained queue, to read the re-arm order --");
    {
        UBYTE *f    = frame_at(2);
        ULONG  flen = build_echo(f, (UWORD)seq, 0x77, 256, FALSE);
        ULONG  bad;

        if (probe_rx_put(f, flen) == 0)
            say("   reply %s", wait_echo((UWORD)seq, 0x77, 256, 100, &bad)
                               ? "intact" : "missing");
        seq++;
    }

    /* --------------------------------------------- the DMA offer, if any -- */

    say("");
    say("-- S2_DMACopyToBuff32 --");
    if (probe_hook_dma_to() == NULL)
    {
        say("   not offered");
    }
    else
    {
        UBYTE *f    = frame_at(2);
        ULONG  flen = build_echo(f, (UWORD)seq, 0x55, SMALL_PAYLOAD, FALSE);
        APTR   buf  = NULL;
        ULONG  bad;
        LONG   rc   = probe_rx_put_dma(f, flen, &buf);

        say("   buffer offered  %08x  (align %u mod 4)", (ULONG)buf,
            (ULONG)buf & 3);
        if (rc == 0)
            say("   delivered through it, reply %s",
                wait_echo((UWORD)seq, 0x55, SMALL_PAYLOAD, 100, &bad)
                ? "intact" : "missing");
        else
            say("   declined");
        seq++;
    }

    probe_get_stats(&st);
    say("");
    say("-- totals --");
    say("   tx frames        %u", st.tx_frames);
    say("   rx delivered     %u", st.rx_delivered);
    say("   rx no reader     %u", st.rx_no_reader);
    say("   rx copy failed   %u", st.rx_copy_failed);
    say("   reads posted     %u", st.reads_total);
    say("   reads now        %u", st.reads_now);
    say("   read high water  %u", st.reads_max);
    say("   arp answered     %u", arp_answered);
    say("   events lost      %u", st.events_lost);
    say("   device open cnt  %u", probe_open_count());

    dump_events(260);

done:
    say("");
    if (sock != NULL)
        CloseLibrary(sock);

    if (probe_remove_safe())
        say("device removed");
    else
        say("device still open -- staying resident so nothing jumps into "
            "freed memory");

    /*
     * The harness watches for DH0:.done.  Writing it here rather than letting
     * the Startup-Sequence do it means the emulator is killed while this
     * process is still alive, so a stack that cannot be shut down never gets
     * its device unloaded underneath it.
     */
    {
        BPTR d = Open((STRPTR)"DH0:.done", MODE_NEWFILE);

        if (d != (BPTR)0)
        {
            Write(d, (APTR)"0\n", 2);
            Close(d);
        }
    }

    if (out_file != (BPTR)0)
    {
        Flush(out_file);
        Close(out_file);
        out_file = (BPTR)0;
    }

    if (probe_open_count() != 0)
    {
        for (;;)
            Delay(50);
    }

    if (arena != NULL)
        FreeMem(arena, PROBE_FRAME_MAX + 8);

    return 0;
}
