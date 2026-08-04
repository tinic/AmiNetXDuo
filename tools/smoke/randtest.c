/*
 * Entropy pool probe, what is src/common/ami_random.c actually worth?
 *
 * Three separate questions, and it is worth being clear which is which:
 *
 *   1. Is the CONDITIONING correct?  The obvious output failure modes,
 *      constant output, a repeated block, a stuck bit, a wildly skewed byte
 *      distribution, plus the DRBG wiring.  These are cheap and a failure
 *      here is a real bug.  The SHA-256 itself is NOT checked here; it is
 *      verified against the FIPS 180-4 vectors in a host harness, because
 *      exposing the hash just to test it would be the wrong trade.
 *
 *   2. Is there any ENTROPY going in?  Reported, not asserted: the pool's own
 *      credit, whether ami_random_is_seeded() clears the bar, and how many
 *      distinct E-Clock interval deltas the machine produced.  Passing these
 *      "tests" would only mean the machine looked lively today.
 *
 *   3. Does a COLD BOOT give the same stream twice?  This is the one that
 *      matters and it cannot be answered from inside one run.  The probe
 *      writes its first 32 output bytes to DH0:randtest.txt; run it twice
 *      from a cold boot and diff the file.  Identical output would NOT
 *      automatically be a bug, it would be the honest measurement of how
 *      little a fixed boot image has to offer.  Measured under FS-UAE: three
 *      cold boots gave three different streams, driven almost entirely by the
 *      host wall clock reaching GetSysTime().  Different is not the same
 *      claim as unpredictable; see the source dump below for which individual
 *      sources moved and which did not.
 *
 *   AMINETXDUO_RUN_TAG=rand1 ./tools/fsuae-run.sh -t 120 \
 *       build/cm/tools/smoke/randtest
 *   AMINETXDUO_RUN_TAG=rand2 ./tools/fsuae-run.sh -t 120 \
 *       build/cm/tools/smoke/randtest
 *   diff build/testhd-rand1/randtest.txt build/testhd-rand2/randtest.txt
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
/* <exec/execbase.h> explicitly, for the struct ExecBase that dump_sources()
   reads IdleCount/DispCount/LastAlert out of.  NDK 3.2's <proto/exec.h> drags
   it in and NDK 3.9's does not, so relying on that is the difference between
   building and "invalid use of undefined type 'struct ExecBase'". */
#include <exec/execbase.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include <string.h>

#include "aminetxduo/compat.h"
#include "aminetxduo/random.h"

static const char version_tag[] __attribute__((used)) =
    "$VER: randtest 1.0 (25.7.2026)";

#define SAMPLES         4096            /* ULONGs drawn */
#define BLOCK_BYTES     (SAMPLES * 4)

static LONG checks, failures;

static void check(const char *what, BOOL ok)
{
    checks++;
    if (!ok)
        failures++;
    Printf((CONST_STRPTR)"  %s %s\n", (LONG)(ok ? "ok  " : "FAIL"), (LONG)what);
    AMI_ERROR("  %s %s", (LONG)(ok ? "ok  " : "FAIL"), (LONG)what);
}

static void report(const char *what, LONG value)
{
    Printf((CONST_STRPTR)"  --   %s: %ld\n", (LONG)what, value);
    AMI_ERROR("  --   %s: %ld", (LONG)what, value);
}

static void hex32(char *out, const UBYTE *in, int n)
{
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = 0; i < n; i++)
    {
        out[i * 2]     = digits[(in[i] >> 4) & 15];
        out[i * 2 + 1] = digits[in[i] & 15];
    }
    out[n * 2] = '\0';
}

/*
 * Structural checks on the DRBG.
 *
 * NOT a SHA-256 known-answer test: ami_random.c does not export its hash and
 * widening the API to allow one would be the wrong trade.  The hash is
 * verified separately by lifting that block into a host harness and running
 * the FIPS 180-4 vectors through it, see the comment above sha256_init() in
 * src/common/ami_random.c.
 *
 * What is left for here is the wiring: consecutive draws must differ (a stuck
 * counter or a dead ratchet fails), and a draw after add_entropy() must differ
 * from one before it (a reseed that silently does nothing fails).
 */
static void check_mix_changes_output(void)
{
    UBYTE before[16], after[16], again[16];

    ami_random_bytes(before, sizeof(before));
    ami_random_bytes(after, sizeof(after));
    check("consecutive draws differ", memcmp(before, after, 16) != 0);

    ami_random_add_entropy("aminetxduo", 10, 0);
    ami_random_bytes(again, sizeof(again));
    check("draw after add_entropy differs", memcmp(after, again, 16) != 0);
}

/*
 * How much does the E-Clock interval actually move on this machine?  Repeats
 * the measurement ami_random.c's gather_jitter() does, and reports the number
 * of DISTINCT delta values seen.  1 means the machine is a metronome and the
 * jitter source contributed nothing.
 */
static void probe_jitter(void)
{
    ULONG seen[32];
    ULONG distinct = 0;
    ULONG spread_lo = 0xFFFFFFFFUL;
    ULONG spread_hi = 0;
    int   i, j;

    (VOID) ami_millis();                /* opens timer.device */

    for (i = 0; i < 256; i++)
    {
        struct EClockVal a, b;
        UWORD            spin = 0;
        ULONG            delta;

        ReadEClock(&a);
        for (j = 0; j < 16 + (i & 15); j++)
            spin = (UWORD)(spin ^ *(volatile UWORD *)0x00DFF006);
        ReadEClock(&b);
        (void)spin;

        delta = b.ev_lo - a.ev_lo;

        if (delta < spread_lo) spread_lo = delta;
        if (delta > spread_hi) spread_hi = delta;

        for (j = 0; j < (int)distinct; j++)
        {
            if (seen[j] == delta)
                break;
        }
        if (j == (int)distinct && distinct < 32)
            seen[distinct++] = delta;
    }

    report("distinct E-Clock interval deltas (of 256 samples)", (LONG)distinct);
    report("  smallest delta (ticks)", (LONG)spread_lo);
    report("  largest delta  (ticks)", (LONG)spread_hi);
}

/*
 * The individual sources, printed raw.
 *
 * ami_random.c deliberately does not expose a per-source breakdown, an
 * attacker-facing number that granular would be a liability in a shipping
 * library, so this reads the same things it reads and prints them.  Diffing
 * two cold-boot runs of this block is what tells you WHICH source is
 * responsible for the streams differing, and which are constants dressed up
 * as entropy.
 */
static void dump_sources(void)
{
    struct ExecBase *sb = (struct ExecBase *)SysBase;
    struct timeval   tv;
    APTR             p;
    char             line[64];

    (VOID) ami_millis();

    tv.tv_secs = 0;
    tv.tv_micro = 0;
    if (TimerBase != NULL)
        GetSysTime(&tv);

    Printf((CONST_STRPTR)"  src  GetSysTime           %lu.%06lu\n",
           (LONG)tv.tv_secs, (LONG)tv.tv_micro);
    Printf((CONST_STRPTR)"  src  AvailMem ANY         %lu\n",
           (LONG)AvailMem(MEMF_ANY));
    Printf((CONST_STRPTR)"  src  AvailMem CHIP        %lu\n",
           (LONG)AvailMem(MEMF_CHIP));
    Printf((CONST_STRPTR)"  src  AvailMem FAST        %lu\n",
           (LONG)AvailMem(MEMF_FAST));
    Printf((CONST_STRPTR)"  src  AvailMem LARGEST     %lu\n",
           (LONG)AvailMem(MEMF_ANY | MEMF_LARGEST));
    Printf((CONST_STRPTR)"  src  IdleCount            %lu\n",
           (LONG)sb->IdleCount);
    Printf((CONST_STRPTR)"  src  DispCount            %lu\n",
           (LONG)sb->DispCount);
    Printf((CONST_STRPTR)"  src  FindTask(NULL)       %08lx\n",
           (LONG)(APTR)FindTask(NULL));
    Printf((CONST_STRPTR)"  src  LastAlert[0]         %08lx\n",
           (LONG)sb->LastAlert[0]);

    p = AllocVec(512, MEMF_ANY);
    if (p != NULL)
    {
        Printf((CONST_STRPTR)"  src  AllocVec address     %08lx\n", (LONG)p);
        hex32(line, (const UBYTE *)p, 16);
        Printf((CONST_STRPTR)"  src  uninitialised bytes  %s\n", (LONG)line);
        FreeVec(p);
    }
}

int main(void)
{
    UBYTE *buf;
    ULONG  counts[256];
    ULONG  ones = 0;
    ULONG  i;
    ULONG  min_count = 0xFFFFFFFFUL;
    ULONG  max_count = 0;
    BPTR   fh;
    char   line[80];

    AMI_ERROR("randtest: start");
    Printf((CONST_STRPTR)"randtest: entropy pool probe\n");

    /* ---- the raw sources, for a cold-boot diff --------------------------- */

    dump_sources();

    /* ---- what the pool thinks of itself ---------------------------------- */

    {
        ULONG t0 = ami_millis();

        ami_random_init();
        report("ami_random_init() cost (ms)", (LONG)(ami_millis() - t0));
    }

    report("credited entropy (bits)", (LONG)ami_random_entropy_bits());
    Printf((CONST_STRPTR)"  --   ami_random_is_seeded(): %s\n",
           (LONG)(ami_random_is_seeded() ? "TRUE" : "FALSE"));
    AMI_ERROR("  --   ami_random_is_seeded(): %s",
              (LONG)(ami_random_is_seeded() ? "TRUE" : "FALSE"));

    probe_jitter();

    /* ---- structural checks ----------------------------------------------- */

    check_mix_changes_output();

    buf = (UBYTE *)AllocVec(BLOCK_BYTES, MEMF_ANY);
    if (buf == NULL)
    {
        Printf((CONST_STRPTR)"randtest: out of memory\n");
        return RETURN_FAIL;
    }

    ami_random_bytes(buf, BLOCK_BYTES);

    /* Constant output / stuck bytes. */
    {
        BOOL varied = FALSE;

        for (i = 1; i < BLOCK_BYTES; i++)
        {
            if (buf[i] != buf[0])
            {
                varied = TRUE;
                break;
            }
        }
        check("output is not constant", varied);
    }

    /* A repeated 32-byte block would mean the DRBG cycled or the counter
       stopped advancing.  Compare every block against the first few. */
    {
        BOOL repeat = FALSE;
        ULONG a, b;

        for (a = 0; a + 32 <= BLOCK_BYTES && !repeat; a += 32)
        {
            for (b = a + 32; b + 32 <= BLOCK_BYTES; b += 32)
            {
                if (memcmp(&buf[a], &buf[b], 32) == 0)
                {
                    repeat = TRUE;
                    break;
                }
            }
        }
        check("no repeated 32-byte block in 16 KB", !repeat);
    }

    /* Bit balance.  16 KB is 131072 bits; expect 65536 +- ~1000. */
    for (i = 0; i < BLOCK_BYTES; i++)
    {
        UBYTE v = buf[i];

        while (v != 0)
        {
            ones += (v & 1);
            v >>= 1;
        }
    }
    report("bits set out of 131072", (LONG)ones);
    check("bit balance within 2%",
          ones > 64225UL && ones < 66847UL);

    /* Byte distribution.  4096 draws * 4 bytes / 256 values = 64 expected. */
    for (i = 0; i < 256; i++)
        counts[i] = 0;
    for (i = 0; i < BLOCK_BYTES; i++)
        counts[buf[i]]++;
    for (i = 0; i < 256; i++)
    {
        if (counts[i] < min_count) min_count = counts[i];
        if (counts[i] > max_count) max_count = counts[i];
    }
    report("rarest byte value seen (expect ~64)", (LONG)min_count);
    report("commonest byte value seen (expect ~64)", (LONG)max_count);
    check("no byte value missing entirely", min_count > 0);
    check("no byte value grossly over-represented", max_count < 160);

    /* ---- the cold-boot artefact ------------------------------------------ */

    hex32(line, buf, 32);
    Printf((CONST_STRPTR)"  --   first 32 bytes: %s\n", (LONG)line);
    AMI_ERROR("  --   first 32 bytes: %s", (LONG)line);

    fh = Open((CONST_STRPTR)"DH0:randtest.txt", MODE_NEWFILE);
    if (fh != 0)
    {
        FPuts(fh, (CONST_STRPTR)line);
        FPuts(fh, (CONST_STRPTR)"\n");
        Close(fh);
        Printf((CONST_STRPTR)"  --   wrote DH0:randtest.txt -- "
                             "diff two cold-boot runs\n");
    }
    else
    {
        Printf((CONST_STRPTR)"  --   could not write DH0:randtest.txt\n");
    }

    FreeVec(buf);

    Printf((CONST_STRPTR)"randtest: %ld checks, %ld failures\n",
           (LONG)checks, (LONG)failures);
    AMI_ERROR("randtest: %ld checks, %ld failures", (LONG)checks, (LONG)failures);

    return (failures == 0) ? RETURN_OK : RETURN_FAIL;
}
