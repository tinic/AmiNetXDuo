/*
 * tone [FREQ=hz] [AMPLITUDE=db] [SECS=n] [UNIT=n] [BUFMS=ms] [RATE=hz]
 *      [MARKMS=ms] [DUMP=file]
 *
 * A steady sine through ahi.device CMD_WRITE, 16-bit stereo, two buffers
 * double-buffered as hush does: the test signal for tools/forbidrec/
 * glitchdet.py, which finds where the USB audio path dropped it.
 *
 *   FREQ       1170 Hz: no block of 1, 2, 5, 10, 25, 50 or 250 ms is a
 *              whole number of cycles, so a repeated one steps the phase
 *   AMPLITUDE  20 dB below full scale (-20 dBFS)
 *   SECS       10; 0 plays until CTRL-C
 *   BUFMS      250 ms per buffer
 *   RATE       44100 Hz
 *   DUMP       file: write the buffers there instead of to AHI
 *   MARKMS     100: CTRL-F silences this much of the next buffer, phase
 *              kept, so a recording can be lined up with the Amiga's log
 *
 * CTRL-C is checked before every buffer and while one is waited for; the
 * two still queued are aborted.
 * Integer math only.  One key=value line per mark, one at the end.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <devices/ahi.h>
#include <proto/exec.h>
#include <proto/dos.h>

static const char version_tag[] __attribute__((used)) =
    "$VER: tone 1.0 (26.10.2026) AmiNetXDuo";

#define TBITS 10
#define TLEN  (1 << TBITS)

static WORD sine[TLEN + 1];

/* sin(pi/2 k/256) * 32767, k = 0..256: a table, not run-time 64-bit math,
 * which Emu68 got wrong (a DUMP= of the buffers showed it). */
static const WORD quarter[TLEN / 4 + 1] = {
        0,   201,   402,   603,   804,  1005,  1206,  1407,  1608,  1809,  2009,  2210,
     2410,  2611,  2811,  3012,  3212,  3412,  3612,  3811,  4011,  4210,  4410,  4609,
     4808,  5007,  5205,  5404,  5602,  5800,  5998,  6195,  6393,  6590,  6786,  6983,
     7179,  7375,  7571,  7767,  7962,  8157,  8351,  8545,  8739,  8933,  9126,  9319,
     9512,  9704,  9896, 10087, 10278, 10469, 10659, 10849, 11039, 11228, 11417, 11605,
    11793, 11980, 12167, 12353, 12539, 12725, 12910, 13094, 13279, 13462, 13645, 13828,
    14010, 14191, 14372, 14553, 14732, 14912, 15090, 15269, 15446, 15623, 15800, 15976,
    16151, 16325, 16499, 16673, 16846, 17018, 17189, 17360, 17530, 17700, 17869, 18037,
    18204, 18371, 18537, 18703, 18868, 19032, 19195, 19357, 19519, 19680, 19841, 20000,
    20159, 20317, 20475, 20631, 20787, 20942, 21096, 21250, 21403, 21554, 21705, 21856,
    22005, 22154, 22301, 22448, 22594, 22739, 22884, 23027, 23170, 23311, 23452, 23592,
    23731, 23870, 24007, 24143, 24279, 24413, 24547, 24680, 24811, 24942, 25072, 25201,
    25329, 25456, 25582, 25708, 25832, 25955, 26077, 26198, 26319, 26438, 26556, 26674,
    26790, 26905, 27019, 27133, 27245, 27356, 27466, 27575, 27683, 27790, 27896, 28001,
    28105, 28208, 28310, 28411, 28510, 28609, 28706, 28803, 28898, 28992, 29085, 29177,
    29268, 29358, 29447, 29534, 29621, 29706, 29791, 29874, 29956, 30037, 30117, 30195,
    30273, 30349, 30424, 30498, 30571, 30643, 30714, 30783, 30852, 30919, 30985, 31050,
    31113, 31176, 31237, 31297, 31356, 31414, 31470, 31526, 31580, 31633, 31685, 31736,
    31785, 31833, 31880, 31926, 31971, 32014, 32057, 32098, 32137, 32176, 32213, 32250,
    32285, 32318, 32351, 32382, 32412, 32441, 32469, 32495, 32521, 32545, 32567, 32589,
    32609, 32628, 32646, 32663, 32678, 32692, 32705, 32717, 32728, 32737, 32745, 32752,
    32757, 32761, 32765, 32766, 32767
};

static void sine_init(void)
{
    LONG k;

    for (k = 0; k <= TLEN / 4; k++)
    {
        sine[k]            = quarter[k];
        sine[TLEN / 2 - k] = quarter[k];
        sine[TLEN / 2 + k] = (WORD)-quarter[k];
        if (k)
            sine[TLEN - k] = (WORD)-quarter[k];
    }
    sine[TLEN] = sine[0];
}

/* floor(freq * 2^32 / rate), freq < rate, by long division in 32 bits. */
static ULONG phase_inc(ULONG freq, ULONG rate)
{
    ULONG q = 0, r = freq;
    int   i;

    for (i = 0; i < 32; i++)
    {
        r <<= 1;
        q <<= 1;
        if (r >= rate)
        {
            r -= rate;
            q |= 1;
        }
    }
    return q;
}

static ULONG ph, inc;
static LONG  amp;

static void fill(WORD *b, LONG frames, LONG quiet)
{
    LONG i;

    for (i = 0; i < frames; i++)
    {
        ULONG j = ph >> (32 - TBITS);
        LONG  f = (LONG)((ph >> (16 - TBITS)) & 0xffff);
        LONG  s = sine[j] + (((sine[j + 1] - sine[j]) * f) >> 16);
        LONG  p = (s * amp) >> 15;
        WORD  v;

        /* The clamp keeps p a full LONG: as a WORD cast, gcc extracts it
         * with bfextu straight after the muls.l, and Emu68 returns 0xffff
         * there for every negative product. */
        if (p > 32767)
            p = 32767;
        v = i < quiet ? 0 : (WORD)p;

        b[2 * i]     = v;
        b[2 * i + 1] = v;
        ph += inc;
    }
}

/* DUMP=file: the buffers AHI would get, written to a file instead, so the
 * waveform can be checked without the audio path. */
static int dump_to(const char *name, LONG fr, LONG nbuf, LONG bufms)
{
    WORD *b = AllocMem(fr * 4, MEMF_PUBLIC);
    BPTR  fh;
    LONG  n, o[2];

    if (!b)
        return 20;
    if (!(fh = Open((CONST_STRPTR)name, MODE_NEWFILE)))
    {
        FreeMem(b, fr * 4);
        PutStr((CONST_STRPTR)"tone=dump_open_failed\n");
        return 20;
    }
    for (n = 0; n < nbuf && !(SetSignal(0, 0) & SIGBREAKF_CTRL_C); n++)
    {
        fill(b, fr, 0);
        if (Write(fh, b, fr * 4) != fr * 4)
            break;
    }
    Close(fh);
    FreeMem(b, fr * 4);
    o[0] = n * bufms;
    o[1] = fr * 4;
    VPrintf((CONST_STRPTR)"tone=dumped audio_ms=%ld buf_bytes=%ld\n", o);
    return 0;
}

int main(void)
{
    LONG               arg[8] = { 0 }, o[8];
    struct RDArgs     *rda;
    LONG               freq = 1170, db = 20, secs = 10, unit = 0, bufms = 250;
    LONG               rate = 44100, markms = 100, fr, nbuf, n, i, err = 0;
    LONG               marks = 0, brk = 0, quiet;
    ULONG              portsig;
    struct MsgPort    *mp;
    char               dump[108] = { 0 };
    struct AHIRequest *r[2], *link = NULL;
    WORD              *b[2];

    rda = ReadArgs((CONST_STRPTR)"FREQ/N,AMPLITUDE/N,SECS/N,UNIT/N,BUFMS/N,RATE/N,MARKMS/N,DUMP/K",
                   arg, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), (CONST_STRPTR)"tone");
        return 20;
    }
    if (arg[0]) freq   = *(LONG *)arg[0];
    if (arg[1]) db     = *(LONG *)arg[1];
    if (arg[2]) secs   = *(LONG *)arg[2];
    if (arg[3]) unit   = *(LONG *)arg[3];
    if (arg[4]) bufms  = *(LONG *)arg[4];
    if (arg[5]) rate   = *(LONG *)arg[5];
    if (arg[6]) markms = *(LONG *)arg[6];
    if (arg[7])
    {
        LONG l = 0;

        while (((char *)arg[7])[l] && l < (LONG)sizeof(dump) - 1)
        {
            dump[l] = ((char *)arg[7])[l];
            l++;
        }
        dump[l] = 0;
    }
    FreeArgs(rda);
    if (db < 0)
        db = -db;
    if (freq < 1 || freq >= rate / 2 || rate < 8000 || rate > 96000 || bufms < 10 ||
        bufms > 1000 || db > 90 || secs < 0)
    {
        PutStr((CONST_STRPTR)"tone=bad_args\n");
        return 20;
    }

    sine_init();
    inc = phase_inc((ULONG)freq, (ULONG)rate);
    amp = 32767;
    for (i = 0; i < db; i++)
        amp = (LONG)(((ULONG)amp * 58409UL + 32768UL) >> 16); /* -1 dB */
    fr   = rate * bufms / 1000;
    nbuf = secs ? (secs * 1000 + bufms - 1) / bufms : 0x7fffffff;

    if (dump[0])
        return dump_to(dump, fr, nbuf, bufms);
    if (!(mp = CreateMsgPort()))
        return 20;
    r[0] = (struct AHIRequest *)CreateIORequest(mp, sizeof(struct AHIRequest));
    if (!r[0])
    {
        DeleteMsgPort(mp);
        return 20;
    }
    r[0]->ahir_Version = 4;
    if (OpenDevice((CONST_STRPTR)"ahi.device", unit, (struct IORequest *)r[0], 0))
    {
        PutStr((CONST_STRPTR)"tone=open_failed\n");
        DeleteIORequest((struct IORequest *)r[0]);
        DeleteMsgPort(mp);
        return 20;
    }
    r[1] = (struct AHIRequest *)AllocMem(sizeof(struct AHIRequest), MEMF_PUBLIC);
    b[0] = AllocMem(fr * 4, MEMF_PUBLIC);
    b[1] = AllocMem(fr * 4, MEMF_PUBLIC);
    if (!r[1] || !b[0] || !b[1])
    {
        PutStr((CONST_STRPTR)"tone=no_memory\n");
        if (b[1]) FreeMem(b[1], fr * 4);
        if (b[0]) FreeMem(b[0], fr * 4);
        if (r[1]) FreeMem(r[1], sizeof(struct AHIRequest));
        CloseDevice((struct IORequest *)r[0]);
        DeleteIORequest((struct IORequest *)r[0]);
        DeleteMsgPort(mp);
        return 20;
    }
    CopyMem(r[0], r[1], sizeof(struct AHIRequest));
    SetSignal(0, SIGBREAKF_CTRL_F);
    portsig = 1UL << mp->mp_SigBit;

    for (n = 0; n < nbuf && !brk; n++)
    {
        struct AHIRequest *q = r[n & 1];
        ULONG              sig = SetSignal(0, SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_F);

        /* Wait for the buffer with CTRL-C in the mask: a device that stops
         * completing writes must not keep the player from stopping. */
        while (!(sig & SIGBREAKF_CTRL_C) && n >= 2 && !CheckIO((struct IORequest *)q))
            sig |= Wait(portsig | SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_F);
        if (sig & SIGBREAKF_CTRL_C)
        {
            brk = 1;
            break;
        }
        if (n >= 2)
        {
            WaitIO((struct IORequest *)q);
            if (q->ahir_Std.io_Error)
                err++;
        }
        quiet = 0;
        if (sig & SIGBREAKF_CTRL_F)
        {
            quiet = rate * markms / 1000;
            if (quiet > fr)
                quiet = fr;
            o[0] = ++marks;
            o[1] = n;
            o[2] = n * bufms;
            o[3] = markms;
            VPrintf((CONST_STRPTR)"tone=mark n=%ld buf=%ld audio_ms=%ld gap_ms=%ld\n", o);
            Flush(Output());
        }
        fill(b[n & 1], fr, quiet);
        q->ahir_Std.io_Command = CMD_WRITE;
        q->ahir_Std.io_Data    = b[n & 1];
        q->ahir_Std.io_Length  = fr * 4;
        q->ahir_Std.io_Offset  = 0;
        q->ahir_Frequency      = rate;
        q->ahir_Type           = AHIST_S16S;
        q->ahir_Volume         = 0x10000;
        q->ahir_Position       = 0x8000;
        q->ahir_Link           = link;
        SendIO((struct IORequest *)q);
        link = q;
    }
    /* The two still out: n - 1 and n - 2. */
    for (i = n - 2; i < n; i++)
        if (i >= 0)
        {
            struct IORequest *q = (struct IORequest *)r[i & 1];

            if (brk && !CheckIO(q))
                AbortIO(q);
            WaitIO(q);
        }

    o[0] = (LONG)(brk ? "break" : "secs");
    o[1] = n * bufms;
    o[2] = freq;
    o[3] = -db;
    o[4] = unit;
    o[5] = bufms;
    o[6] = err;
    o[7] = marks;
    VPrintf((CONST_STRPTR)"tone=done reason=%s audio_ms=%ld freq=%ld dbfs=%ld unit=%ld bufms=%ld errs=%ld marks=%ld\n", o);
    CloseDevice((struct IORequest *)r[0]);
    FreeMem(b[0], fr * 4);
    FreeMem(b[1], fr * 4);
    FreeMem(r[1], sizeof(struct AHIRequest));
    DeleteIORequest((struct IORequest *)r[0]);
    DeleteMsgPort(mp);
    return 0;
}
