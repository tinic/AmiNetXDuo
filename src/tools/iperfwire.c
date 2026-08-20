/*
 * The iperf 2 wire format and the rate arithmetic.  See iperfwire.h for what
 * this speaks and what it does not.
 *
 * SPDX-License-Identifier: MIT
 */

#include "iperfwire.h"

#include <stddef.h>

/*
 * unsigned long is 32 bits on the m68k and 64 on the host that runs the tests,
 * and this file is compiled both ways.  Anything that depends on a value
 * wrapping at 32 bits -- the sign of a datagram id, the carry out of the byte
 * counter -- has to say so, because on the host it does not wrap at all.
 * Both of those were wrong until the host test said so.
 */
#define U32(x)  ((unsigned long)(x) & 0xffffffffUL)

/* ------------------------------------------------------------ byte order - */

static void wire_put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)((v >> 24) & 0xffUL);
    p[1] = (unsigned char)((v >> 16) & 0xffUL);
    p[2] = (unsigned char)((v >> 8) & 0xffUL);
    p[3] = (unsigned char)(v & 0xffUL);
}

static unsigned long wire_get32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16)
         | ((unsigned long)p[2] << 8)  | (unsigned long)p[3];
}

/* ---------------------------------------------------------------- payload - */

void iperf_pattern_fill(unsigned char *buf, unsigned long len,
                        unsigned long offset)
{
    unsigned long i;
    unsigned long d = offset % 10UL;

    for (i = 0; i < len; i++)
    {
        buf[i] = (unsigned char)('0' + d);
        if (++d == 10UL)
            d = 0;
    }
}

/* --------------------------------------------------------------- datagram - */

void iperf_dg_put(unsigned char *buf, long id,
                  unsigned long sec, unsigned long usec)
{
    unsigned long i;

    wire_put32(buf, (unsigned long)id);
    wire_put32(buf + 4, sec);
    wire_put32(buf + 8, usec);

    for (i = IPERF_DG_LEN; i < (unsigned long)IPERF_DG_TOTAL; i++)
        buf[i] = 0;
}

long iperf_dg_id(const unsigned char *buf)
{
    unsigned long raw = wire_get32(buf);

    /* Two's complement by hand: a plain cast of a value above LONG_MAX is
       implementation-defined, and this has to mean the same thing on the host
       tests as on the 68000.  The mask is what makes it 32-bit on both. */
    if (raw & 0x80000000UL)
        return -(long)U32(~raw + 1UL);

    return (long)raw;
}

int iperf_dg_get(const unsigned char *buf, unsigned long len, long *id)
{
    if (len < IPERF_DG_LEN || id == NULL)
        return -1;

    *id = iperf_dg_id(buf);
    return 0;
}

/* ----------------------------------------------------------------- report - */

void iperf_report_put(unsigned char *buf, const IperfWireReport *rep)
{
    unsigned long i;

    for (i = 0; i < (unsigned long)IPERF_REPORT_LEN; i++)
        buf[i] = 0;

    wire_put32(buf + IPERF_REPORT_OFF +  0, rep->flags);
    wire_put32(buf + IPERF_REPORT_OFF +  4, rep->bytes_hi);
    wire_put32(buf + IPERF_REPORT_OFF +  8, rep->bytes_lo);
    wire_put32(buf + IPERF_REPORT_OFF + 12, rep->stop_sec);
    wire_put32(buf + IPERF_REPORT_OFF + 16, rep->stop_usec);
    wire_put32(buf + IPERF_REPORT_OFF + 20, rep->lost);
    wire_put32(buf + IPERF_REPORT_OFF + 24, rep->outoforder);
    wire_put32(buf + IPERF_REPORT_OFF + 28, rep->datagrams);
    wire_put32(buf + IPERF_REPORT_OFF + 32, rep->jitter_sec);
    wire_put32(buf + IPERF_REPORT_OFF + 36, rep->jitter_usec);
}

int iperf_report_get(const unsigned char *buf, unsigned long len,
                     IperfWireReport *rep)
{
    if (len < (unsigned long)(IPERF_REPORT_OFF + 40))
        return -1;

    rep->flags = wire_get32(buf + IPERF_REPORT_OFF + 0);

    if ((rep->flags & IPERF_HEADER_VERSION1) == 0)
        return -1;

    rep->bytes_hi    = wire_get32(buf + IPERF_REPORT_OFF +  4);
    rep->bytes_lo    = wire_get32(buf + IPERF_REPORT_OFF +  8);
    rep->stop_sec    = wire_get32(buf + IPERF_REPORT_OFF + 12);
    rep->stop_usec   = wire_get32(buf + IPERF_REPORT_OFF + 16);
    rep->lost        = wire_get32(buf + IPERF_REPORT_OFF + 20);
    rep->outoforder  = wire_get32(buf + IPERF_REPORT_OFF + 24);
    rep->datagrams   = wire_get32(buf + IPERF_REPORT_OFF + 28);
    rep->jitter_sec  = wire_get32(buf + IPERF_REPORT_OFF + 32);
    rep->jitter_usec = wire_get32(buf + IPERF_REPORT_OFF + 36);
    return 0;
}

/* ------------------------------------------------------------- arithmetic - */

void iperf_add64(unsigned long *hi, unsigned long *lo, unsigned long add)
{
    unsigned long sum = U32(*lo + add);

    if (sum < *lo)
        *hi = U32(*hi + 1UL);

    *lo = sum;
}

const char *iperf_limits_check(unsigned long seconds, unsigned long kbytes,
                               unsigned long buflen, unsigned long port)
{
    if (port == 0 || port > 65535UL)
        return "that is not a port";

    if (seconds == 0 && kbytes == 0)
        return "a test needs either a duration or a size";

    if (seconds != 0 && kbytes != 0)
        return "a duration and a size cannot both decide when to stop";

    if (seconds > IPERF_MAX_SECONDS)
        return "a run longer than an hour is not a measurement";

    /* A byte target is bounded too: at the rate the fastest Amiga card
       reaches, 4 GB is already several hours. */
    if (kbytes > (4UL * 1024UL * 1024UL))
        return "a transfer over 4 GB is not a measurement";

    if (buflen < (unsigned long)IPERF_BUF_MIN
        || buflen > (unsigned long)IPERF_BUF_MAX)
        return "the buffer size is outside what this will send";

    return NULL;
}

unsigned long iperf_slice_budget(unsigned long seconds, unsigned long kbytes,
                                 unsigned long buflen)
{
    /*
     * A slice either does up to 64 sends or waits up to 20 ms, so a second of
     * a real run is a few hundred slices at the very most.  10,000 a second is
     * more than an order of magnitude of headroom, and an hour's run is then
     * 36 million, which still cannot be reached by anything but a stopped
     * clock.
     */
    unsigned long budget;

    if (seconds != 0)
        budget = seconds * 10000UL;
    else
    {
        /* A byte target: one slice per send, plus the same headroom. */
        unsigned long sends;

        if (buflen != 0)
        {
            /* Split before multiplying: the accepted 4 GB ceiling is
               4194304 KB, whose byte count is exactly one past ULONG on the
               target.  The remainder product is at most 8191 * 1024. */
            sends = (kbytes / buflen) * 1024UL;
            sends += ((kbytes % buflen) * 1024UL) / buflen;
            sends++;
        }
        else
            sends = 1UL;

        budget = (sends > 0xFFFFFFFFUL / 100UL)
                     ? 0xFFFFFFFFUL : sends * 100UL;
    }

    /* A floor, so that a one-second run still has room for a handshake and a
       peer that takes its time. */
    if (budget < 20000UL)
        budget = 20000UL;

    return budget;
}

unsigned long iperf_bits_per_sec(unsigned long bytes_hi, unsigned long bytes_lo,
                                 unsigned long ms)
{
    unsigned long long bytes;
    unsigned long long bits;

    if (ms == 0)
        return 0;

    bytes = ((unsigned long long)bytes_hi << 32) | (unsigned long long)bytes_lo;
    bits  = bytes * 8000ULL;

    /* Round to nearest rather than down: at the low end a truncating divide
       is what turns 999,999 bit/s into 999 Kbit/s twice over. */
    bits += (unsigned long long)ms / 2ULL;
    bits /= (unsigned long long)ms;

    if (bits > 0xffffffffULL)
        return 0xffffffffUL;

    return (unsigned long)bits;
}

/*
 * Two decimals of `value / scale`, appended to buf.  Written out rather than
 * done in floating point: these commands run on a 68000 with no FPU, and
 * pulling in the soft-float printf for one number costs more than the whole
 * command.
 */
static unsigned long fmt_fixed2(char *buf, unsigned long buflen,
                                unsigned long pos,
                                unsigned long long value, unsigned long long scale,
                                const char *unit)
{
    unsigned long long whole = value / scale;
    unsigned long long frac  = ((value % scale) * 100ULL + scale / 2ULL) / scale;
    char               digits[24];
    unsigned long      n = 0;
    unsigned long      i;

    if (frac >= 100ULL)
    {
        whole++;
        frac -= 100ULL;
    }

    if (whole == 0ULL)
    {
        digits[n++] = '0';
    }
    else
    {
        char rev[24];
        unsigned long r = 0;

        while (whole > 0ULL && r < sizeof(rev))
        {
            rev[r++] = (char)('0' + (unsigned)(whole % 10ULL));
            whole /= 10ULL;
        }
        while (r > 0)
            digits[n++] = rev[--r];
    }

    digits[n++] = '.';
    digits[n++] = (char)('0' + (unsigned)(frac / 10ULL));
    digits[n++] = (char)('0' + (unsigned)(frac % 10ULL));

    for (i = 0; i < n && pos + 1 < buflen; i++)
        buf[pos++] = digits[i];

    if (pos + 1 < buflen)
        buf[pos++] = ' ';

    for (i = 0; unit[i] != '\0' && pos + 1 < buflen; i++)
        buf[pos++] = unit[i];

    buf[pos] = '\0';
    return pos;
}

void iperf_format_rate(char *buf, unsigned long buflen, unsigned long bits)
{
    if (buflen == 0)
        return;

    buf[0] = '\0';

    if (bits >= 1000000UL)
        (void)fmt_fixed2(buf, buflen, 0, (unsigned long long)bits,
                         1000000ULL, "Mbit/s");
    else if (bits >= 1000UL)
        (void)fmt_fixed2(buf, buflen, 0, (unsigned long long)bits,
                         1000ULL, "Kbit/s");
    else
        (void)fmt_fixed2(buf, buflen, 0, (unsigned long long)bits,
                         1ULL, "bit/s");
}

void iperf_format_bytes(char *buf, unsigned long buflen,
                        unsigned long hi, unsigned long lo)
{
    unsigned long long bytes;

    if (buflen == 0)
        return;

    buf[0] = '\0';
    bytes = ((unsigned long long)hi << 32) | (unsigned long long)lo;

    if (bytes >= (1ULL << 20))
        (void)fmt_fixed2(buf, buflen, 0, bytes, 1ULL << 20, "MBytes");
    else if (bytes >= (1ULL << 10))
        (void)fmt_fixed2(buf, buflen, 0, bytes, 1ULL << 10, "KBytes");
    else
        (void)fmt_fixed2(buf, buflen, 0, bytes, 1ULL, "Bytes");
}
