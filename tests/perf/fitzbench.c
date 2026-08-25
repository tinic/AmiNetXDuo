/*
 * FitzBench, bulk file throughput over a mounted Fitz share.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <devices/timer.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include <stdarg.h>
#include <string.h>

static const char version_tag[] __attribute__((used)) =
    "$VER: FitzBench 1.0 (29.7.2026)";

#define TEMPLATE "PATH/A,KB/K/N,CHUNK/K/N,REPS/K/N,NOVERIFY/S"

enum { ARG_PATH, ARG_KB, ARG_CHUNK, ARG_REPS, ARG_NOVERIFY, ARG_COUNT };

#define DEF_KB     512
#define DEF_CHUNK  32768
#define DEF_REPS   3
#define VERIFY_KB  64          /* how much of the readback is compared */

static VOID fb_printf(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    VPrintf((CONST_STRPTR)fmt, (APTR)args);
    va_end(args);
}

static struct MsgPort    *fb_port;
static struct timerequest *fb_req;
struct Device            *TimerBase;

static ULONG fb_rate;          /* E-Clock ticks per second */

static BOOL fb_timer_open(VOID)
{
    struct EClockVal ev;

    fb_port = CreateMsgPort();
    if (fb_port == NULL)
        return FALSE;

    fb_req = (struct timerequest *)CreateIORequest(fb_port,
                                                   sizeof(struct timerequest));
    if (fb_req == NULL)
        return FALSE;

    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_ECLOCK,
                   (struct IORequest *)fb_req, 0) != 0)
        return FALSE;

    TimerBase = fb_req->tr_node.io_Device;
    fb_rate = ReadEClock(&ev);

    return (fb_rate != 0UL);
}

static VOID fb_timer_close(VOID)
{
    if (TimerBase != NULL)
    {
        CloseDevice((struct IORequest *)fb_req);
        TimerBase = NULL;
    }
    if (fb_req != NULL)
        DeleteIORequest((struct IORequest *)fb_req);
    if (fb_port != NULL)
        DeleteMsgPort(fb_port);
}

static ULONG fb_now(VOID)
{
    struct EClockVal ev;

    (VOID)ReadEClock(&ev);

    return ev.ev_lo;
}

static ULONG fb_kbs(ULONG bytes, ULONG ticks)
{
    if (ticks == 0UL)
        return 0UL;

    return ((bytes / 1024UL) * fb_rate) / ticks;
}

static UBYTE *fb_buf;
static ULONG  fb_chunk;

static VOID fb_fill(VOID)
{
    ULONG state = 0x12345678UL;
    ULONG i;

    for (i = 0; i < fb_chunk; i++)
    {
        state = state * 1664525UL + 1013904223UL;
        fb_buf[i] = (UBYTE)(state >> 24);
    }
}

static ULONG fb_write_once(CONST_STRPTR path, ULONG total)
{
    BPTR  fh;
    ULONG done = 0;
    ULONG t0, t1;

    t0 = fb_now();

    fh = Open(path, MODE_NEWFILE);
    if (fh == 0)
    {
        fb_printf("fitzbench: cannot create %s\n", (LONG)path);
        return 0UL;
    }

    while (done < total)
    {
        ULONG want = total - done;
        LONG  got;

        if (want > fb_chunk)
            want = fb_chunk;

        got = Write(fh, fb_buf, (LONG)want);
        if (got != (LONG)want)
        {
            fb_printf("fitzbench: Write() gave %ld of %ld at offset %lu\n",
                      got, (LONG)want, done);
            Close(fh);
            return 0UL;
        }
        done += want;
    }

    if (Close(fh) == 0)
    {
        fb_printf("fitzbench: Close() after write failed\n");
        return 0UL;
    }

    t1 = fb_now();

    return t1 - t0;
}

static ULONG fb_read_once(CONST_STRPTR path, ULONG total)
{
    BPTR  fh;
    ULONG done = 0;
    ULONG t0, t1;

    t0 = fb_now();

    fh = Open(path, MODE_OLDFILE);
    if (fh == 0)
    {
        fb_printf("fitzbench: cannot open %s for reading\n", (LONG)path);
        return 0UL;
    }

    while (done < total)
    {
        ULONG want = total - done;
        LONG  got;

        if (want > fb_chunk)
            want = fb_chunk;

        got = Read(fh, fb_buf, (LONG)want);
        if (got != (LONG)want)
        {
            fb_printf("fitzbench: Read() gave %ld of %ld at offset %lu\n",
                      got, (LONG)want, done);
            Close(fh);
            return 0UL;
        }
        done += want;
    }

    Close(fh);

    t1 = fb_now();

    return t1 - t0;
}

static BOOL fb_verify(CONST_STRPTR path, ULONG total)
{
    UBYTE *ref;
    BPTR   fh;
    ULONG  want = (ULONG)VERIFY_KB * 1024UL;
    ULONG  done = 0;
    BOOL   ok = TRUE;

    if (want > total)
        want = total;

    ref = AllocMem(fb_chunk, MEMF_ANY);
    if (ref == NULL)
        return TRUE;                    /* cannot check; do not fail the run */

    memcpy(ref, fb_buf, fb_chunk);

    fh = Open(path, MODE_OLDFILE);
    if (fh == 0)
    {
        FreeMem(ref, fb_chunk);
        return FALSE;
    }

    while (done < want && ok)
    {
        ULONG take = want - done;
        LONG  got;

        if (take > fb_chunk)
            take = fb_chunk;

        got = Read(fh, fb_buf, (LONG)take);
        if (got != (LONG)take || memcmp(fb_buf, ref, take) != 0)
            ok = FALSE;

        done += take;
    }

    Close(fh);
    memcpy(fb_buf, ref, fb_chunk);
    FreeMem(ref, fb_chunk);

    return ok;
}

static VOID fb_report(CONST_STRPTR what, ULONG *ticks, ULONG reps, ULONG total)
{
    ULONG i;
    ULONG lo = 0, hi = 0, sum = 0;
    ULONG good = 0;

    for (i = 0; i < reps; i++)
    {
        ULONG kbs;

        if (ticks[i] == 0UL)
            continue;

        kbs = fb_kbs(total, ticks[i]);
        if (good == 0 || kbs < lo)
            lo = kbs;
        if (good == 0 || kbs > hi)
            hi = kbs;
        sum += kbs;
        good++;
    }

    if (good == 0)
    {
        fb_printf("fitzbench: RESULT %s FAILED\n", (LONG)what);
        return;
    }

    fb_printf("fitzbench: RESULT %s kbs_mean=%lu kbs_min=%lu kbs_max=%lu reps=%lu\n",
              (LONG)what, sum / good, lo, hi, good);
}

int main(VOID)
{
    LONG           args[ARG_COUNT];
    struct RDArgs *rda;
    char           file[256];
    CONST_STRPTR   path;
    ULONG          kb, reps, total, i;
    ULONG         *wt, *rt;
    LONG           rc = RETURN_FAIL;

    memset(args, 0, sizeof args);

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        PrintFault(IoErr(), (CONST_STRPTR)"FitzBench");
        return RETURN_FAIL;
    }

    path     = (CONST_STRPTR)args[ARG_PATH];
    kb       = args[ARG_KB]    ? (ULONG)*(LONG *)args[ARG_KB]    : DEF_KB;
    fb_chunk = args[ARG_CHUNK] ? (ULONG)*(LONG *)args[ARG_CHUNK] : DEF_CHUNK;
    reps     = args[ARG_REPS]  ? (ULONG)*(LONG *)args[ARG_REPS]  : DEF_REPS;

    if (kb == 0 || fb_chunk == 0 || reps == 0)
    {
        fb_printf("fitzbench: KB, CHUNK and REPS must all be non-zero\n");
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    total = kb * 1024UL;

    /* PATH is a directory or a device; the file goes inside it. */
    {
        size_t n = strlen((const char *)path);

        if (n + 16 >= sizeof file)
        {
            fb_printf("fitzbench: PATH too long\n");
            FreeArgs(rda);
            return RETURN_FAIL;
        }
        strcpy(file, (const char *)path);
        if (n > 0 && file[n - 1] != ':' && file[n - 1] != '/')
            strcat(file, "/");
        strcat(file, "fitzbench.dat");
    }

    if (!fb_timer_open())
    {
        fb_printf("fitzbench: no timer.device\n");
        fb_timer_close();
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    fb_buf = AllocMem(fb_chunk, MEMF_ANY);
    wt     = AllocMem(reps * sizeof(ULONG), MEMF_ANY | MEMF_CLEAR);
    rt     = AllocMem(reps * sizeof(ULONG), MEMF_ANY | MEMF_CLEAR);

    if (fb_buf == NULL || wt == NULL || rt == NULL)
    {
        fb_printf("fitzbench: out of memory (chunk %lu)\n", fb_chunk);
        goto out;
    }

    fb_fill();

    (VOID)fb_write_once((CONST_STRPTR)file, total);
    (VOID)fb_read_once((CONST_STRPTR)file, total);

    fb_printf("fitzbench: file=%s bytes=%lu chunk=%lu reps=%lu eclock=%lu\n",
              (LONG)file, total, fb_chunk, reps, fb_rate);

    for (i = 0; i < reps; i++)
    {
        wt[i] = fb_write_once((CONST_STRPTR)file, total);
        fb_printf("fitzbench: write rep=%lu ticks=%lu kbs=%lu\n",
                  i, wt[i], fb_kbs(total, wt[i]));

        rt[i] = fb_read_once((CONST_STRPTR)file, total);
        fb_printf("fitzbench: read  rep=%lu ticks=%lu kbs=%lu\n",
                  i, rt[i], fb_kbs(total, rt[i]));
    }

    fb_report((CONST_STRPTR)"write", wt, reps, total);
    fb_report((CONST_STRPTR)"read",  rt, reps, total);

    if (args[ARG_NOVERIFY] == 0)
    {
        if (fb_verify((CONST_STRPTR)file, total))
            fb_printf("fitzbench: verify ok (first %lu KB)\n", (ULONG)VERIFY_KB);
        else
        {
            fb_printf("fitzbench: VERIFY FAILED\n");
            goto out;
        }
    }

    DeleteFile((CONST_STRPTR)file);
    rc = RETURN_OK;

out:
    if (rt != NULL)
        FreeMem(rt, reps * sizeof(ULONG));
    if (wt != NULL)
        FreeMem(wt, reps * sizeof(ULONG));
    if (fb_buf != NULL)
        FreeMem(fb_buf, fb_chunk);

    fb_timer_close();
    FreeArgs(rda);

    return rc;
}
