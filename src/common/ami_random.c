/*
 * AmiNetXDuo -- entropy collection and a hash DRBG.
 *
 * See include/aminetxduo/random.h for the scope and limits.  The conditioning
 * is textbook, the collection is estimated, and ami_random_is_seeded() reports
 * which.
 *
 *   NX_RAND is consumed by the NetX Duo core (IP identification, TCP initial
 *   sequence numbers, ephemeral ports), by the DHCP/DNS add-ons, and by
 *   nx_secure (ECDHE private keys, the TLS client random).  Only the last is a
 *   TLS build, so the generator cannot live under src/tls and nx_crypto's
 *   SHA-256 cannot be reused; hence the small one here.  Measured cost in the
 *   shipping library: 5,328 bytes of text (SHA-256, the K table and the
 *   collection) less the 104 saved by deleting the xorshift in
 *   src/bsdsocket/library_runtime.c that this replaces, a net 5,224 bytes.
 *
 *   The state has to survive being read: a TLS client random goes on the wire
 *   in clear, and with an LCG or an xorshift that is the whole state.  SHA-256
 *   counter mode plus a forward ratchet costs ~300-500 us per 32-byte block on
 *   a 14 MHz 68020, one hash per eight rand() calls, which the packet paths
 *   can afford (a 200 packet/s TCP stream spends well under 1% of the CPU
 *   here).
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/random.h"
#include "aminetxduo/compat.h"

#include <exec/execbase.h>
#include <exec/lists.h>
#include <exec/tasks.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/timer.h>

/*
 * compat.c defines the shared timer.device base (the `TimerBase` that
 * proto/timer.h declares) and opens it on the first ami_millis() call. Nothing
 * here opens a second one.
 */

/* ==================================================================== SHA-256
 *
 * FIPS 180-4.  Self-contained: no libc, no nx_crypto -- a shared library must
 * not drag newlib in, and nx_crypto is a TLS-only build.
 *
 * Verified against the published vectors (empty string, "abc", both
 * multi-block examples from FIPS 180-4 Appendix B, and the one-million-'a'
 * vector) by lifting this block verbatim into a host harness.  Repeat that
 * after any edit here.  A harness that defines ULONG as `unsigned long` fails
 * all five, because that is 64 bits on a modern host and 32 on m68k-amigaos.
 * Hence the assertion below.
 */

/* Every rotate and every addition here assumes exactly 32 bits. */
typedef char ami_random_ulong_is_32_bits[(sizeof(ULONG) == 4) ? 1 : -1];

typedef struct
{
    ULONG state[8];
    ULONG length;               /* bytes, low 32 -- we never hash 4 GB */
    ULONG block_used;
    UBYTE block[64];
} Sha256;

static const ULONG sha256_k[64] =
{
    0x428A2F98UL, 0x71374491UL, 0xB5C0FBCFUL, 0xE9B5DBA5UL,
    0x3956C25BUL, 0x59F111F1UL, 0x923F82A4UL, 0xAB1C5ED5UL,
    0xD807AA98UL, 0x12835B01UL, 0x243185BEUL, 0x550C7DC3UL,
    0x72BE5D74UL, 0x80DEB1FEUL, 0x9BDC06A7UL, 0xC19BF174UL,
    0xE49B69C1UL, 0xEFBE4786UL, 0x0FC19DC6UL, 0x240CA1CCUL,
    0x2DE92C6FUL, 0x4A7484AAUL, 0x5CB0A9DCUL, 0x76F988DAUL,
    0x983E5152UL, 0xA831C66DUL, 0xB00327C8UL, 0xBF597FC7UL,
    0xC6E00BF3UL, 0xD5A79147UL, 0x06CA6351UL, 0x14292967UL,
    0x27B70A85UL, 0x2E1B2138UL, 0x4D2C6DFCUL, 0x53380D13UL,
    0x650A7354UL, 0x766A0ABBUL, 0x81C2C92EUL, 0x92722C85UL,
    0xA2BFE8A1UL, 0xA81A664BUL, 0xC24B8B70UL, 0xC76C51A3UL,
    0xD192E819UL, 0xD6990624UL, 0xF40E3585UL, 0x106AA070UL,
    0x19A4C116UL, 0x1E376C08UL, 0x2748774CUL, 0x34B0BCB5UL,
    0x391C0CB3UL, 0x4ED8AA4AUL, 0x5B9CCA4FUL, 0x682E6FF3UL,
    0x748F82EEUL, 0x78A5636FUL, 0x84C87814UL, 0x8CC70208UL,
    0x90BEFFFAUL, 0xA4506CEBUL, 0xBEF9A3F7UL, 0xC67178F2UL
};

#define ROR32(x, n)     (((x) >> (n)) | ((x) << (32 - (n))))

/*
 * The message schedule is a 16-word ring, not the w[64] the specification is
 * written with. Round i only reads w[i-16], w[i-15], w[i-7] and w[i-2], so the
 * other 48 words are dead the moment they are produced and w[i&15] can be
 * updated in place. Bit-identical, and 256 bytes of stack instead of 64.
 *
 * That matters here because this is the deepest frame under NX_RAND, and
 * NX_RAND is on the caller's stack: an ephemeral port, a TCP ISN and a DNS
 * query ID are all drawn inside a bsdsocket.library vector, and an AmigaDOS
 * Shell gives 4 KB with no guard page.
 */
static VOID sha256_compress(Sha256 *ctx, const UBYTE *block)
{
    ULONG w[16];
    ULONG a, b, c, d, e, f, g, h;
    int   i;

    for (i = 0; i < 16; i++)
    {
        w[i] = ((ULONG)block[i * 4]     << 24) |
               ((ULONG)block[i * 4 + 1] << 16) |
               ((ULONG)block[i * 4 + 2] <<  8) |
               ((ULONG)block[i * 4 + 3]);
    }

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; i++)
    {
        ULONG s0, s1, ch, maj, t1, t2;

        if (i >= 16)
        {
            ULONG x = w[(i +  1) & 15];         /* w[i-15] */
            ULONG y = w[(i + 14) & 15];         /* w[i-2]  */

            s0 = ROR32(x, 7) ^ ROR32(x, 18) ^ (x >> 3);
            s1 = ROR32(y, 17) ^ ROR32(y, 19) ^ (y >> 10);

            /* w[i&15] is w[i-16]; w[(i+9)&15] is w[i-7]. */
            w[i & 15] += s0 + w[(i + 9) & 15] + s1;
        }

        s1  = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
        ch  = (e & f) ^ ((~e) & g);
        t1  = h + s1 + ch + sha256_k[i] + w[i & 15];
        s0  = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
        maj = (a & b) ^ (a & c) ^ (b & c);
        t2  = s0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
    ctx->state[3] += d; ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
}

static VOID sha256_init(Sha256 *ctx)
{
    ctx->state[0] = 0x6A09E667UL; ctx->state[1] = 0xBB67AE85UL;
    ctx->state[2] = 0x3C6EF372UL; ctx->state[3] = 0xA54FF53AUL;
    ctx->state[4] = 0x510E527FUL; ctx->state[5] = 0x9B05688CUL;
    ctx->state[6] = 0x1F83D9ABUL; ctx->state[7] = 0x5BE0CD19UL;
    ctx->length     = 0;
    ctx->block_used = 0;
}

static VOID sha256_update(Sha256 *ctx, const void *data, ULONG length)
{
    const UBYTE *p = (const UBYTE *)data;

    ctx->length += length;

    while (length > 0)
    {
        ULONG take = 64UL - ctx->block_used;

        if (take > length)
            take = length;

        {
            ULONG i;

            for (i = 0; i < take; i++)
                ctx->block[ctx->block_used + i] = p[i];
        }

        ctx->block_used += take;
        p               += take;
        length          -= take;

        if (ctx->block_used == 64)
        {
            sha256_compress(ctx, ctx->block);
            ctx->block_used = 0;
        }
    }
}

static VOID sha256_final(Sha256 *ctx, UBYTE *digest)
{
    ULONG bits = ctx->length << 3;
    ULONG i;

    ctx->block[ctx->block_used++] = 0x80;

    if (ctx->block_used > 56)
    {
        while (ctx->block_used < 64)
            ctx->block[ctx->block_used++] = 0;
        sha256_compress(ctx, ctx->block);
        ctx->block_used = 0;
    }
    while (ctx->block_used < 56)
        ctx->block[ctx->block_used++] = 0;

    /* Length in bits, big-endian 64.  The high word is always 0 here. */
    ctx->block[56] = 0; ctx->block[57] = 0; ctx->block[58] = 0;
    ctx->block[59] = (UBYTE)(ctx->length >> 29);
    ctx->block[60] = (UBYTE)(bits >> 24);
    ctx->block[61] = (UBYTE)(bits >> 16);
    ctx->block[62] = (UBYTE)(bits >>  8);
    ctx->block[63] = (UBYTE)(bits);

    sha256_compress(ctx, ctx->block);

    for (i = 0; i < 8; i++)
    {
        digest[i * 4]     = (UBYTE)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (UBYTE)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (UBYTE)(ctx->state[i] >>  8);
        digest[i * 4 + 3] = (UBYTE)(ctx->state[i]);
    }
}

/* ===================================================================== pool */

#define AMI_RANDOM_KEY_BYTES    32

/*
 * Domain separators, so material mixed in one role can never be confused with
 * output produced in another.
 */
#define DOMAIN_RESEED           0x52    /* 'R' */
#define DOMAIN_GENERATE         0x47    /* 'G' */
#define DOMAIN_RATCHET          0x4B    /* 'K' */

static UBYTE  pool_key[AMI_RANDOM_KEY_BYTES];
static UBYTE  pool_out[AMI_RANDOM_KEY_BYTES];
static ULONG  pool_out_used = AMI_RANDOM_KEY_BYTES;   /* forces a first block */
static ULONG  pool_counter;
static ULONG  pool_bits;
static BOOL   pool_started;

/*
 * key <- SHA-256(DOMAIN_RESEED || key || counter || material)
 *
 * The old key is always an input, so a caller who supplies attacker-chosen
 * material cannot steer the result; the worst they achieve is no improvement.
 */
static VOID pool_mix(const void *data, ULONG length, ULONG credit_bits)
{
    Sha256 ctx;
    UBYTE  tag = DOMAIN_RESEED;

    Forbid();

    sha256_init(&ctx);
    sha256_update(&ctx, &tag, 1);
    sha256_update(&ctx, pool_key, sizeof(pool_key));
    sha256_update(&ctx, &pool_counter, sizeof(pool_counter));
    if (data != NULL && length > 0)
        sha256_update(&ctx, data, length);
    sha256_final(&ctx, pool_key);

    pool_bits += credit_bits;
    if (pool_bits > 256UL)
        pool_bits = 256UL;

    /* A reseed invalidates any buffered output. */
    pool_out_used = AMI_RANDOM_KEY_BYTES;

    Permit();
}

/* ============================================================ entropy sources
 *
 * Each source is weak on its own.  The comment on each says what it is worth
 * and why its credit is what it is.  The credits are conservative; see
 * docs/RESEARCH.md.
 */

/*
 * VHPOSR, the raster beam position.  Its low byte advances once per two
 * pixels, so on a running machine it is a free-running counter at ~3.5 MHz.
 * That makes it a phase sample rather than entropy: if the machine is
 * deterministic from reset then so is the beam.  It is read because the read
 * is a Chip RAM cycle contending with display DMA, which is what makes the
 * interval below variable; the value itself is credited nothing.
 */
#define CUSTOM_VHPOSR   ((volatile UWORD *)0x00DFF006)

typedef struct
{
    ULONG eclock_lo;
    ULONG eclock_hi;
    ULONG systime_secs;
    ULONG systime_micro;
    ULONG avail_any;
    ULONG avail_chip;
    ULONG avail_fast;
    ULONG avail_largest;
    ULONG avail_public;
    ULONG idle_count;
    ULONG disp_count;
    ULONG attn_flags;
    ULONG sys_flags;
    ULONG quantum;
    ULONG elapsed;
    ULONG vblank_freq;
    ULONG power_freq;
    ULONG eclock_freq;
    ULONG last_alert[4];
    ULONG this_task;
    ULONG stack_addr;
    ULONG alloc_addr[4];
    UBYTE alloc_bytes[64];
    ULONG task_hash;
    ULONG task_count;
    UWORD raster[64];
    UWORD jitter[64];
} EntropySample;

/*
 * E-Clock jitter.  Take an interval measurement repeatedly and keep the low
 * bits of the delta.  On real hardware the interval varies: bitplane and
 * sprite DMA steal cycles from the CPU, memory refresh lands where it lands,
 * and level 2/3/6 interrupts arrive asynchronously.  A cycle-exact emulator
 * booting a fixed image might reproduce all of that and yield nothing.
 *
 * That cannot be settled from inside the function, so it is measured instead:
 * count how many bit positions varied across the samples and credit one bit
 * per varying position, capped at 12.  A machine where every delta is
 * identical gets zero.
 *
 * Measured under FS-UAE: 22-27 distinct delta values out of 256 samples,
 * spanning 52 to 263 E-Clock ticks, over three cold boots, so this source
 * scores 7-9 bits there.  The cap stays at 12 rather than the ~4.6
 * bits/sample that 24 distinct values would nominally support, because an
 * emulator's timing variation comes from the host's scheduler and how much of
 * that an attacker can see or influence has not been analysed.
 */
static ULONG gather_jitter(EntropySample *s)
{
    ULONG varying = 0;
    ULONG first   = 0;
    ULONG changed = 0;
    int   i, j;

    if (TimerBase == NULL)
        return 0;

    for (i = 0; i < 64; i++)
    {
        struct EClockVal a, b;
        UWORD            spin = 0;

        ReadEClock(&a);

        /*
         * The work between the two reads is chip-bus traffic: reading a custom
         * register is a Chip RAM cycle, so it contends with whatever DMA the
         * display is doing.  The iteration count varies with the sample index
         * so the loop cannot settle into one fixed phase relationship with the
         * video beam.
         */
        for (j = 0; j < 16 + (i & 15); j++)
            spin = (UWORD)(spin ^ *CUSTOM_VHPOSR);

        ReadEClock(&b);

        s->raster[i] = spin;
        s->jitter[i] = (UWORD)(b.ev_lo - a.ev_lo);

        if (i == 0)
            first = s->jitter[i];
        else
            changed |= (ULONG)(s->jitter[i] ^ (UWORD)first);
    }

    for (i = 0; i < 16; i++)
    {
        if (changed & (1UL << i))
            varying++;
    }

    return (varying > 12) ? 12 : varying;
}

/*
 * Free memory figures, allocation addresses, and the residue in freshly
 * allocated blocks.
 *
 * Mixed in, credited zero, on measurement: tools/smoke/randtest.c printed
 * these across three cold boots under FS-UAE:
 *
 *   AvailMem ANY/CHIP/FAST/LARGEST   identical to the byte, all three runs
 *   AllocVec() address               identical, all three runs
 *   16 bytes of MEMF_ANY residue     identical, all three runs, and non-zero
 *
 * Crediting the residue because it came back non-zero would award 8 bits to a
 * value that is the same on every boot, since the allocator hands out the same
 * block to the same caller at the same point in the same boot sequence.
 *
 * On a machine that has been used -- a warm reboot, a Workbench that launched
 * and quit things, a fragmented pool -- these carry real information, and it
 * still goes into the pool.  It does not go into the accounting, because
 * nothing here can tell the two cases apart.
 */
static ULONG gather_memory(EntropySample *s)
{
    static const ULONG sizes[4] = { 61, 127, 509, 1021 };
    APTR  block[4];
    int   i;

    s->avail_any     = AvailMem(MEMF_ANY);
    s->avail_chip    = AvailMem(MEMF_CHIP);
    s->avail_fast    = AvailMem(MEMF_FAST);
    s->avail_largest = AvailMem(MEMF_ANY | MEMF_LARGEST);
    s->avail_public  = AvailMem(MEMF_PUBLIC);

    for (i = 0; i < 4; i++)
    {
        block[i] = AllocVec(sizes[i], MEMF_ANY);
        s->alloc_addr[i] = (ULONG)block[i];
    }

    /* 16 bytes of prior-owner residue from each block. */
    for (i = 0; i < 4; i++)
    {
        int j;

        for (j = 0; j < 16; j++)
            s->alloc_bytes[i * 16 + j] =
                (block[i] != NULL) ? ((UBYTE *)block[i])[j] : 0;
    }

    for (i = 0; i < 4; i++)
    {
        if (block[i] != NULL)
            FreeVec(block[i]);
    }

    return 0;
}

/*
 * Exec's own state.
 *
 * IdleCount and DispCount are free-running dispatcher counters, so they encode
 * how much scheduling has happened since reset.  They do move: 44 / 66 / 57
 * and 282 / 276 / 283 across three identical cold boots, a spread of a few
 * dozen counts.  Credited 4 bits for the pair, roughly log2(that spread) and
 * still an estimate.
 *
 * The rest of what this reads was constant on every boot measured: AttnFlags,
 * VBlankFrequency and the E-Clock rate are hardware description, ThisTask and
 * the stack address were identical all three runs, and LastAlert was
 * 0xFFFFFFFF every time.  Mixed in, credited nothing.
 */
static ULONG gather_exec(EntropySample *s)
{
    struct ExecBase *sb = (struct ExecBase *)SysBase;
    struct Task     *me = FindTask(NULL);
    ULONG            local;

    s->idle_count   = sb->IdleCount;
    s->disp_count   = sb->DispCount;
    s->attn_flags   = (ULONG)sb->AttnFlags;
    s->sys_flags    = (ULONG)sb->SysFlags;
    s->quantum      = (ULONG)sb->Quantum;
    s->elapsed      = (ULONG)sb->Elapsed;
    s->vblank_freq  = (ULONG)sb->VBlankFrequency;
    s->power_freq   = (ULONG)sb->PowerSupplyFrequency;
    s->eclock_freq  = sb->ex_EClockFrequency;
    s->last_alert[0] = sb->LastAlert[0];
    s->last_alert[1] = sb->LastAlert[1];
    s->last_alert[2] = sb->LastAlert[2];
    s->last_alert[3] = sb->LastAlert[3];
    s->this_task    = (ULONG)me;
    s->stack_addr   = (ULONG)&local;

    return 4;
}

/*
 * The task lists.  Walking TaskReady and TaskWait under Forbid() gives every
 * live task's node address, its saved stack pointer and its priority.  The
 * saved SP of a task parked in Wait() depends on how deep it was when it
 * blocked, which on a machine doing anything at all is not fixed.
 *
 * Credited 2 bits, generously: on a bare test boot there are perhaps five
 * tasks, the same five every time, and the one address checked directly
 * (FindTask(NULL)) was identical across three cold boots.  On a real Workbench
 * with a browser, a shell and some commodities it is worth considerably more,
 * hence not zero.
 */
static ULONG gather_tasks(EntropySample *s)
{
    struct ExecBase *sb = (struct ExecBase *)SysBase;
    struct List     *lists[2];
    ULONG            hash = 0x9E3779B9UL;
    ULONG            count = 0;
    int              which;

    lists[0] = &sb->TaskReady;
    lists[1] = &sb->TaskWait;

    Forbid();
    for (which = 0; which < 2; which++)
    {
        struct Node *node;

        for (node = lists[which]->lh_Head;
             node != NULL && node->ln_Succ != NULL;
             node = node->ln_Succ)
        {
            struct Task *t = (struct Task *)node;

            hash = (hash * 33UL) ^ (ULONG)t;
            hash = (hash * 33UL) ^ (ULONG)t->tc_SPReg;
            hash = (hash * 33UL) ^ (ULONG)t->tc_SPUpper;
            hash = (hash * 33UL) ^ (ULONG)(UBYTE)t->tc_Node.ln_Pri;
            count++;
        }
    }
    Permit();

    s->task_hash  = hash;
    s->task_count = count;

    return 2;
}

/*
 * The system clock, the strongest source on the list.
 *
 * On a machine with a battery-backed clock that somebody has set, this is real
 * wall time: 1532507776.382025 / 1532507783.606435 / 1532507798.617969 across
 * three cold boots, so both fields move.  Without one -- or on a boot that
 * never ran SetClock -- timer.device starts at 1978-01-01 and this degenerates
 * to uptime, the same every boot, so the credit is conditional on the seconds
 * field being non-zero.
 *
 * Credited 8 bits, far below the ~20 the microsecond field nominally holds.
 * An attacker who knows within a second when the machine was switched on has
 * only the sub-second part left to search, and one who can observe the boot
 * (anyone on the same LAN watching for DHCP) narrows it further.  8 is what
 * remains after assuming they can.
 */
static ULONG gather_clock(EntropySample *s)
{
    struct timeval tv;

    if (TimerBase == NULL)
        return 0;

    GetSysTime(&tv);
    s->systime_secs  = (ULONG)tv.tv_secs;
    s->systime_micro = (ULONG)tv.tv_micro;

    return (s->systime_secs != 0) ? 8 : 0;
}

/* ================================================================ collection */

/*
 * The sample is static, not a local: at 460 bytes it was the largest single
 * frame anywhere below a bsdsocket.library vector, and a vector runs on the
 * caller's stack -- 4 KB from an AmigaDOS Shell, with no guard page and no
 * MMU, so the overflow is silent and kills the machine somewhere else later.
 * -fstack-usage put random_gather at 516 bytes and it sat under send(),
 * connect() and every DNS lookup, since ami_random_bytes() seeds on first use.
 *
 * One writer: bsd_runtime_open() calls ami_random_init() from InitResident(),
 * before the first socket exists, so nothing races the collection.
 */
static EntropySample random_sample;

static VOID random_gather(VOID)
{
    EntropySample   *s = &random_sample;
    ULONG            credit = 0;
    struct EClockVal ev;
    UBYTE           *p = (UBYTE *)s;
    ULONG            i;

    /* Zero the whole struct rather than harvesting whatever was there before:
     * an uninitialised read is undefined behaviour the compiler may exploit. */
    for (i = 0; i < sizeof(*s); i++)
        p[i] = 0;

    /* Forces compat.c to open timer.device if it is not open yet. */
    (VOID) ami_millis();

    if (TimerBase != NULL)
    {
        ReadEClock(&ev);
        s->eclock_lo = ev.ev_lo;
        s->eclock_hi = ev.ev_hi;
    }

    credit += gather_clock(s);
    credit += gather_exec(s);
    credit += gather_memory(s);
    credit += gather_tasks(s);
    credit += gather_jitter(s);

    pool_mix(s, sizeof(*s), credit);

    /* Nothing else reads it, and leaving a machine fingerprint sitting in the
       library's BSS is free to avoid. */
    for (i = 0; i < sizeof(*s); i++)
        p[i] = 0;
}

VOID ami_random_init(VOID)
{
    pool_started = TRUE;
    random_gather();

    AMI_DEBUG("random: entropy credit %lu bits, is_seeded=%s",
              (LONG)pool_bits,
              (LONG)(ami_random_is_seeded() ? "TRUE" : "FALSE"));
}

VOID ami_random_add_entropy(const void *data, ULONG length, ULONG credit_bits)
{
    /*
     * Collect first if nothing has yet.  Setting pool_started here instead
     * would let an early ami_random_srand(), which routes through this
     * function, suppress the machine's own collection entirely, leaving the
     * pool holding only whatever the caller passed.
     */
    if (!pool_started)
        ami_random_init();

    pool_mix(data, length, credit_bits);
}

ULONG ami_random_entropy_bits(VOID)
{
    return pool_bits;
}

BOOL ami_random_is_seeded(VOID)
{
    return (BOOL)(pool_bits >= AMI_RANDOM_MIN_BITS);
}

/* ================================================================ generation
 *
 * out  <- SHA-256(DOMAIN_GENERATE || key || counter++)
 * key  <- SHA-256(DOMAIN_RATCHET  || key || counter)   after each refill
 *
 * The ratchet is what makes a disclosed output block useless for recovering
 * earlier ones: the key that produced it no longer exists anywhere.
 *
 * Forbid()/Permit() rather than a semaphore: this is called from the NetX Duo
 * IP thread, from adopted user tasks and from library code, and it must not
 * block.  It is not interrupt-callable, since Permit() from an interrupt can
 * try to dispatch, and nothing in the tree calls NX_RAND from one.
 */
static VOID random_refill(VOID)
{
    Sha256 ctx;
    UBYTE  tag;

    tag = DOMAIN_GENERATE;
    pool_counter++;
    sha256_init(&ctx);
    sha256_update(&ctx, &tag, 1);
    sha256_update(&ctx, pool_key, sizeof(pool_key));
    sha256_update(&ctx, &pool_counter, sizeof(pool_counter));
    sha256_final(&ctx, pool_out);

    tag = DOMAIN_RATCHET;
    sha256_init(&ctx);
    sha256_update(&ctx, &tag, 1);
    sha256_update(&ctx, pool_key, sizeof(pool_key));
    sha256_update(&ctx, &pool_counter, sizeof(pool_counter));
    sha256_final(&ctx, pool_key);

    pool_out_used = 0;
}

VOID ami_random_bytes(APTR buffer, ULONG length)
{
    UBYTE *out = (UBYTE *)buffer;

    if (out == NULL || length == 0)
        return;

    if (!pool_started)
        ami_random_init();

    /*
     * Forbid() is taken per block, not around the whole request.  A 32-byte
     * block costs one SHA-256 pair, a few hundred microseconds; a 16 KB
     * request is 512 of them, and a shared library must not hold the scheduler
     * off for the ~200 ms that would take.
     *
     * Releasing between blocks lets another task interleave and consume the
     * blocks after this one, which is harmless: pool_out_used advances inside
     * the Forbid(), so no byte is handed to two callers.
     */
    while (length > 0)
    {
        ULONG take;
        ULONG i;

        Forbid();

        if (pool_out_used >= AMI_RANDOM_KEY_BYTES)
            random_refill();

        take = AMI_RANDOM_KEY_BYTES - pool_out_used;
        if (take > length)
            take = length;

        for (i = 0; i < take; i++)
        {
            out[i] = pool_out[pool_out_used + i];
            /* Burn it: a byte handed out is never handed out twice. */
            pool_out[pool_out_used + i] = 0;
        }

        pool_out_used += take;

        Permit();

        out    += take;
        length -= take;
    }
}

ULONG ami_random_ulong(VOID)
{
    ULONG value;

    ami_random_bytes(&value, sizeof(value));
    return value;
}

int ami_random_rand(void)
{
    /* rand() is specified to return 0..RAND_MAX, and RAND_MAX is 0x7FFFFFFF. */
    return (int)(ami_random_ulong() & 0x7FFFFFFFUL);
}

void ami_random_srand(unsigned int seed)
{
    ULONG value = (ULONG)seed;

    /*
     * Mixed in, not assigned, and credited nothing.  See random.h: a caller
     * asking for reproducibility must not be able to hand an attacker the
     * state of the generator that makes their session keys.
     */
    ami_random_add_entropy(&value, sizeof(value), 0);
}
