/*
 * RsGapRx: the receive client of docs/plans/roadshow-rx-gap.md.
 *
 * One binary for both arms.  It opens bsdsocket.library by name and uses
 * socket/connect/recv/getsockname/CloseSocket/Errno and nothing else: no
 * SocketBaseTags, no IoctlSocket, no helper of ours.  The stack under test is
 * whatever LIBS: resolves, so Roadshow and AmiNetXDuo run this file unchanged.
 *
 *   RsGapRx HOST=192.168.1.160 PORT=17810 BYTES=100000000 TO=RAM:rsgap.dat
 *           [READSIZE=16384] [TAG=1] [CONNECTWAIT=60]
 *
 * The stream is written to TO as it arrives.  The clock is timer.device's
 * EClock, started when connect() returns and stopped after the last Write()
 * following recv() == 0.  kb = 1000 bytes, as in the report being tested.
 *
 * Output is one key=value line.  RC 0 when BYTES arrived exactly, 10 on a
 * short or long stream or any socket error, 20 when nothing could start.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <sys/types.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <devices/timer.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <proto/bsdsocket.h>

#include <stdarg.h>
#include <string.h>

static const char version_tag[] __attribute__((used)) =
    "$VER: RsGapRx 1.0 (24.9.2026)";

#define TEMPLATE \
    "HOST/A,PORT/K/N,BYTES/K/N,TO/K,READSIZE/K/N,TAG/K,CONNECTWAIT/K/N"

enum { ARG_HOST, ARG_PORT, ARG_BYTES, ARG_TO, ARG_READSIZE, ARG_TAG,
       ARG_CONNECTWAIT, ARG_COUNT };

#define DEF_PORT        17810
#define DEF_READSIZE    16384
#define DEF_CONNECTWAIT 60

struct Library *SocketBase;
struct Device  *TimerBase;

static struct MsgPort     *rx_port;
static struct timerequest *rx_treq;
static ULONG               rx_rate;

static VOID rx_printf(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    VPrintf((CONST_STRPTR)fmt, (APTR)args);
    va_end(args);
}

static BOOL rx_timer_open(VOID)
{
    struct EClockVal ev;

    rx_port = CreateMsgPort();
    if (rx_port == NULL)
        return FALSE;
    rx_treq = (struct timerequest *)CreateIORequest(rx_port,
                                                    sizeof(*rx_treq));
    if (rx_treq == NULL)
        return FALSE;
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_ECLOCK,
                   (struct IORequest *)rx_treq, 0) != 0)
        return FALSE;
    TimerBase = rx_treq->tr_node.io_Device;
    rx_rate = ReadEClock(&ev);
    return rx_rate != 0UL;
}

static VOID rx_timer_close(VOID)
{
    if (TimerBase != NULL)
        CloseDevice((struct IORequest *)rx_treq);
    if (rx_treq != NULL)
        DeleteIORequest((struct IORequest *)rx_treq);
    if (rx_port != NULL)
        DeleteMsgPort(rx_port);
}

/* Milliseconds between two EClock readings; 32 bits of ticks is 100 minutes. */
static ULONG rx_ms(const struct EClockVal *a, const struct EClockVal *b)
{
    ULONG ticks = b->ev_lo - a->ev_lo;

    return (ticks / rx_rate) * 1000UL + ((ticks % rx_rate) * 1000UL) / rx_rate;
}

/* Dotted quad to a network-order address; 0 when it is not one. */
static ULONG rx_parse_ip(const char *s)
{
    ULONG addr = 0, part = 0;
    int   dots = 0, digits = 0;

    for (;; s++)
    {
        if (*s >= '0' && *s <= '9')
        {
            part = part * 10UL + (ULONG)(*s - '0');
            if (part > 255UL || ++digits > 3)
                return 0;
        }
        else if (*s == '.' || *s == '\0')
        {
            if (digits == 0)
                return 0;
            addr = (addr << 8) | part;
            part = 0;
            digits = 0;
            if (*s == '\0')
                break;
            if (++dots > 3)
                return 0;
        }
        else
            return 0;
    }
    return dots == 3 ? addr : 0;
}

static LONG rx_connect(ULONG addr, UWORD port, LONG waitsecs, LONG *tries,
                       LONG *lasterr)
{
    struct sockaddr_in sin;
    LONG               s;
    LONG               waited = 0;

    for (;;)
    {
        (*tries)++;
        s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0)
        {
            *lasterr = Errno();
            return -1;
        }
        memset(&sin, 0, sizeof(sin));
        sin.sin_len = sizeof(sin);
        sin.sin_family = AF_INET;
        sin.sin_port = port;
        sin.sin_addr.s_addr = addr;
        if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) == 0)
            return s;
        *lasterr = Errno();
        CloseSocket(s);
        if (waited >= waitsecs || (SetSignal(0, 0) & SIGBREAKF_CTRL_C))
            return -1;
        Delay(50);
        waited++;
    }
}

int main(VOID)
{
    LONG               args[ARG_COUNT];
    struct RDArgs     *rda;
    const char        *host, *to, *tag;
    ULONG              addr, want, readsize, got = 0;
    LONG               port, waitsecs, tries = 0, lasterr = 0;
    LONG               s = -1, n;
    BPTR               fh = 0;
    UBYTE             *buf = NULL;
    struct EClockVal   t0, t1;
    struct sockaddr_in me;
    socklen_t          melen = sizeof(me);
    ULONG              ms, q, frac, la, lp;
    const char        *result = "ok";
    int                rc = 0;

    memset(args, 0, sizeof(args));
    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        rx_printf("rec=xfer result=badargs rc=20\n");
        return 20;
    }
    host = (const char *)args[ARG_HOST];
    port = args[ARG_PORT] ? *(LONG *)args[ARG_PORT] : DEF_PORT;
    want = args[ARG_BYTES] ? (ULONG)*(LONG *)args[ARG_BYTES] : 0UL;
    to = args[ARG_TO] ? (const char *)args[ARG_TO] : "RAM:rsgap.dat";
    readsize = args[ARG_READSIZE] ? (ULONG)*(LONG *)args[ARG_READSIZE]
                                  : DEF_READSIZE;
    tag = args[ARG_TAG] ? (const char *)args[ARG_TAG] : "-";
    waitsecs = args[ARG_CONNECTWAIT] ? *(LONG *)args[ARG_CONNECTWAIT]
                                     : DEF_CONNECTWAIT;

    addr = rx_parse_ip(host);
    if (addr == 0 || port <= 0 || port > 65535 || readsize == 0)
    {
        rx_printf("rec=xfer tag=%s result=badargs rc=20\n", (LONG)tag);
        FreeArgs(rda);
        return 20;
    }

    if (!rx_timer_open())
    {
        rx_printf("rec=xfer tag=%s result=notimer rc=20\n", (LONG)tag);
        rc = 20;
        goto out;
    }
    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (SocketBase == NULL)
    {
        rx_printf("rec=xfer tag=%s result=nobsdsocket rc=20\n", (LONG)tag);
        rc = 20;
        goto out;
    }
    buf = AllocMem(readsize, MEMF_ANY);
    fh = Open((CONST_STRPTR)to, MODE_NEWFILE);
    if (buf == NULL || fh == 0)
    {
        rx_printf("rec=xfer tag=%s result=nobuffer rc=20\n", (LONG)tag);
        rc = 20;
        goto out;
    }

    s = rx_connect(addr, (UWORD)port, waitsecs, &tries, &lasterr);
    if (s < 0)
    {
        rx_printf("rec=xfer tag=%s host=%s port=%ld result=noconnect "
                  "connect_tries=%ld errno=%ld rc=10\n",
                  (LONG)tag, (LONG)host, port, tries, lasterr);
        rc = 10;
        goto out;
    }
    /* Before the clock starts, so it costs the transfer nothing. */
    la = 0; lp = 0;
    if (getsockname(s, (struct sockaddr *)&me, &melen) == 0)
        { la = me.sin_addr.s_addr; lp = me.sin_port; }
    (VOID)ReadEClock(&t0);

    for (;;)
    {
        n = recv(s, buf, (LONG)readsize, 0);
        if (n == 0)
            break;
        if (n < 0)
        {
            lasterr = Errno();
            result = "recverror";
            break;
        }
        if (Write(fh, buf, n) != n)
        {
            result = "writeerror";
            break;
        }
        got += (ULONG)n;
    }
    (VOID)ReadEClock(&t1);

    ms = rx_ms(&t0, &t1);
    q = ms ? got / ms : 0;
    frac = ms ? ((got % ms) * 100UL) / ms : 0;
    if (strcmp(result, "ok") == 0 && want != 0 && got != want)
        result = got < want ? "short" : "long";
    if (strcmp(result, "ok") != 0)
        rc = 10;

    rx_printf("rec=xfer tag=%s host=%s port=%ld local=%lu.%lu.%lu.%lu:%lu "
              "rx_bytes=%lu expect=%lu rx_ms=%lu rx_kbps=%lu.%02lu "
              "read_size=%lu connect_tries=%ld errno=%ld result=%s rc=%ld\n",
              (LONG)tag, (LONG)host, port,
              (la >> 24) & 255, (la >> 16) & 255, (la >> 8) & 255, la & 255, lp,
              got, want, ms, q, frac, readsize, tries,
              strcmp(result, "recverror") == 0 ? lasterr : 0L,
              (LONG)result, (LONG)rc);

out:
    if (s >= 0)
        CloseSocket(s);
    if (fh != 0)
        Close(fh);
    if (buf != NULL)
        FreeMem(buf, readsize);
    if (SocketBase != NULL)
        CloseLibrary(SocketBase);
    rx_timer_close();
    FreeArgs(rda);
    return rc;
}
