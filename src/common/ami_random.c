/*
 * AmiNetXDuo, entropy collection and a hash DRBG.
 *
 * See include/aminetxduo/random.h for the scope and limits.  Self-contained
 * on purpose: nx_crypto is a TLS-only build and this must work without it.
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

/* compat.c owns the shared timer.device base; nothing here opens a second. */

/* ==================================================================== SHA-256
 *
 * FIPS 180-4, self-contained: no libc, no nx_crypto.  Re-check against the
 * published vectors in a host harness after ANY edit here.
 */

/* Every rotate and every addition here assumes exactly 32 bits. */
typedef char ami_random_ulong_is_32_bits[(sizeof(ULONG) == 4) ? 1 : -1];

typedef struct
{
    ULONG state[8];
    ULONG length;               /* bytes, low 32, this never hashes 4 GB */
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
 * The message schedule is a 16-word ring, not w[64]: round i reads only
 * w[i-16], w[i-15], w[i-7], w[i-2].  Bit-identical, and 192 bytes less stack
 * on the deepest frame under NX_RAND, which runs on the caller's stack.
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

            /* w[i&15] is w[i-16]. w[(i+9)&15] is w[i-7]. */
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
static ULONG  pool_internal_bits;
static BOOL   pool_started;
static volatile BOOL pool_gathering;   /* one random_gather() at a time */

/*
 * key <- SHA-256(DOMAIN_RESEED || key || counter || material).  The old key
 * MUST stay an input, so attacker-chosen material cannot steer the result.
 */
static VOID pool_mix(const void *data, ULONG length, ULONG credit_bits)
{
    Sha256 ctx;
    UBYTE  tag = DOMAIN_RESEED;
    UBYTE  digest[AMI_RANDOM_KEY_BYTES];
    BOOL   have_digest = FALSE;

    /* Fold caller material to 32 bytes OUTSIDE the Forbid(): its length is
       unbounded, and hashing it under Forbid() holds the scheduler off for
       one compression per 64 bytes. */
    if (data != NULL && length > 0)
    {
        sha256_init(&ctx);
        sha256_update(&ctx, data, length);
        sha256_final(&ctx, digest);
        have_digest = TRUE;
    }

    Forbid();

    sha256_init(&ctx);
    sha256_update(&ctx, &tag, 1);
    sha256_update(&ctx, pool_key, sizeof(pool_key));
    sha256_update(&ctx, &pool_counter, sizeof(pool_counter));
    if (have_digest)
        sha256_update(&ctx, digest, sizeof(digest));
    sha256_final(&ctx, pool_key);

    /* Saturate before adding.  credit_bits is supplied through a public API,
       so ULONG_MAX must not wrap a partially seeded pool back toward zero. */
    if (credit_bits >= 256UL - pool_bits)
        pool_bits = 256UL;
    else
        pool_bits += credit_bits;

    /* A reseed invalidates any buffered output. */
    pool_out_used = AMI_RANDOM_KEY_BYTES;

    Permit();

    if (have_digest)
    {
        ULONG i;

        for (i = 0; i < sizeof(digest); i++)
            digest[i] = 0;
    }
}

/* ============================================================ entropy sources
 *
 * Each source is weak on its own and the credits are deliberately conservative.
 */

/*
 * VHPOSR, the raster beam position.  A phase sample, not entropy: credited
 * NOTHING.  It is read because the read is a Chip RAM cycle that contends
 * with display DMA and so varies the interval measured below.
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
 * E-Clock jitter: repeated interval measurements, keeping the low delta bits.
 * Credit is MEASURED, not assumed -- one bit per varying bit position, capped
 * at 12, so a machine whose deltas are all identical is credited zero.
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

        /* The iteration count must vary with the sample index, or the loop
           settles into a fixed phase relationship with the video beam. */
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
 * Free memory figures, allocation addresses and freshly allocated residue.
 * Mixed in but credited ZERO: on a cold boot from a fixed image all three are
 * identical to the byte, and nothing here can tell that case from a used one.
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
 * The state of Exec itself.  Only IdleCount/DispCount are credited (4 bits
 * for the pair); AttnFlags, VBlankFrequency, the E-Clock rate, ThisTask and
 * LastAlert are hardware description and credited nothing.
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
 * The task lists: node address, saved SP and priority of every live task,
 * walked under Forbid().  Credited 2 bits -- a bare boot has the same five
 * tasks every time, a real Workbench far more.
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
 * The system clock, the strongest source here.  Without a set battery clock
 * timer.device starts at 1978-01-01 and this degenerates to uptime, so the
 * 8-bit credit is CONDITIONAL on a non-zero seconds field.
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
 * The sample MUST stay static, not a local: 460 bytes on a bsdsocket.library
 * vector runs on the caller's 4 KB Shell stack with no guard page.
 * One writer only -- ami_random_init() runs from InitResident().
 */
static EntropySample random_sample;

static VOID random_gather(VOID)
{
    EntropySample   *s = &random_sample;
    ULONG            credit = 0;
    ULONG            new_credit = 0;
    struct EClockVal ev;
    UBYTE           *p = (UBYTE *)s;
    ULONG            i;

    /* Zero the whole struct, and do not harvest whatever was there before: an
     * uninitialised read is undefined behaviour that the compiler can exploit. */
    for (i = 0; i < sizeof(*s); i++)
        p[i] = 0;

    /* Forces compat.c to open timer.device when it is not yet open. */
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

    /* Repeated views of the same machine, not independent samples: raise the
       trusted estimate only when a collection beats every earlier one, or
       repeated init calls alone would cross AMI_RANDOM_MIN_BITS. */
    if (credit > AMI_RANDOM_INTERNAL_MAX_BITS)
        credit = AMI_RANDOM_INTERNAL_MAX_BITS;

    if (credit > pool_internal_bits)
    {
        new_credit = credit - pool_internal_bits;
        pool_internal_bits = credit;
    }

    pool_mix(s, sizeof(*s), new_credit);

    /* Nothing else reads it, and a machine fingerprint left in the library BSS
       costs nothing to remove. */
    for (i = 0; i < sizeof(*s); i++)
        p[i] = 0;
}

/*
 * Repeat calls add; two collections AT ONCE are excluded, because
 * random_gather() writes the shared file-scope random_sample top to bottom.
 * A second caller returns immediately rather than waiting.
 */
VOID ami_random_init(VOID)
{
    Forbid();
    if (pool_gathering)
    {
        Permit();
        return;
    }
    pool_gathering = TRUE;
    pool_started   = TRUE;
    Permit();

    random_gather();

    pool_gathering = FALSE;

    AMI_DEBUG("random: entropy credit %lu bits, is_seeded=%s",
              (LONG)pool_bits,
              (LONG)(ami_random_is_seeded() ? "TRUE" : "FALSE"));
}

VOID ami_random_add_entropy(const void *data, ULONG length, ULONG credit_bits)
{
    /* Collect first if nothing has: setting pool_started here instead lets an
       early ami_random_srand() suppress the machine collection entirely. */
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

/* ========================================================== arrival timing
 *
 * The one source here that is not a property of the machine: the E-Clock
 * delta between frame arrivals.  ONLY the low eight bits are kept -- the high
 * bits are the sender's spacing, which anyone on the wire can also see; the
 * low bits are local scheduling and DMA jitter, which they cannot.
 * Credit is measured per batch, one bit per varying bit position.
 */

#define ARRIVAL_BATCH               16

/* How many of the delta's low bits are the local jitter rather than the
   sender's spacing.  Also the ceiling on one batch's credit. */
#define ARRIVAL_BITS_KEPT           8

static UBYTE  arrival_delta[ARRIVAL_BATCH];
static UWORD  arrival_n;
static ULONG  arrival_prev;
static BOOL   arrival_have_prev;
static ULONG  arrival_bits;             /* credited from this source so far */
static BOOL   arrival_done;             /* the gate on the receive path     */

static VOID arrival_flush(const UBYTE *batch)
{
    ULONG varying = 0;
    UBYTE changed = 0;
    UWORD i;

    /* Which bit positions moved across the batch. */
    for (i = 1; i < ARRIVAL_BATCH; i++)
        changed |= (UBYTE)(batch[i] ^ batch[0]);

    for (i = 0; i < ARRIVAL_BITS_KEPT; i++)
    {
        if ((changed & (UBYTE)(1U << i)) != 0)
            varying++;
    }

    if (varying > (AMI_RANDOM_ARRIVAL_MAX_BITS - arrival_bits))
        varying = AMI_RANDOM_ARRIVAL_MAX_BITS - arrival_bits;

    arrival_bits += varying;

    pool_mix(batch, (ULONG)ARRIVAL_BATCH, varying);

    AMI_DEBUG("random: arrival batch varied %lu of %lu bit(s), %lu credited",
              (LONG)varying, (LONG)ARRIVAL_BITS_KEPT, (LONG)arrival_bits);

    if (arrival_bits >= AMI_RANDOM_ARRIVAL_MAX_BITS ||
        pool_bits >= AMI_RANDOM_MIN_BITS)
    {
        arrival_done = TRUE;

        /* INFO rather than DEBUG: it fires exactly once per machine. */
        AMI_INFO("random: arrivals credited %lu bits, pool %lu, seeded=%s",
                 (LONG)arrival_bits, (LONG)pool_bits,
                 (LONG)(ami_random_is_seeded() ? "TRUE" : "FALSE"));
    }
}

VOID ami_random_arrival(VOID)
{
    struct EClockVal ev;
    UBYTE            batch[ARRIVAL_BATCH];
    ULONG            now;
    UWORD            i;
    BOOL             full = FALSE;

    if (arrival_done)
        return;

    if (TimerBase == NULL)
        return;

    /* NEVER ami_random_add_entropy() here -- it seeds on first use, a 22 ms
       machine collection, and this is a receive thread. */
    if (!pool_started)
        return;

    ReadEClock(&ev);
    now = ev.ev_lo;

    /* Multiple receive threads write this accumulator.  Only the index needs
       bracketing (a raced index would write past the array); the mixing does
       not -- pool_mix() takes its own Forbid(). */
    Forbid();

    if (!arrival_have_prev)
    {
        arrival_prev      = now;
        arrival_have_prev = TRUE;
    }
    else
    {
        arrival_delta[arrival_n++] = (UBYTE)(now - arrival_prev);
        arrival_prev               = now;

        if (arrival_n >= ARRIVAL_BATCH)
        {
            for (i = 0; i < ARRIVAL_BATCH; i++)
                batch[i] = arrival_delta[i];

            arrival_n = 0;
            full      = TRUE;
        }
    }

    Permit();

    if (full)
        arrival_flush(batch);
}

/* ================================================================ generation
 *
 * out  <- SHA-256(DOMAIN_GENERATE || key || counter++)
 * key  <- SHA-256(DOMAIN_RATCHET  || key || counter)   after each refill
 *
 * Forbid()/Permit(), not a semaphore: this must not block.  NOT
 * interrupt-callable -- Permit() from an interrupt can try to dispatch.
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

    /* Forbid() per BLOCK, not around the whole request: a large request must
       not hold the scheduler off.  Interleaving is safe -- pool_out_used
       advances inside the Forbid(), so no byte reaches two callers. */
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

unsigned int ami_crypto_rbg(unsigned int bits, unsigned char *result)
{
    /* The rand() mask above is a rand() obligation and must NOT apply to a
       key.  Zero is NX_CRYPTO_SUCCESS. */
    ami_random_bytes(result, (ULONG)((bits + 7u) >> 3));
    return 0u;
}

void ami_random_srand(unsigned int seed)
{
    ULONG value = (ULONG)seed;

    /* Mixed in, NOT assigned, and credited nothing: a caller asking for
       reproducibility must not be able to set the session-key generator. */
    ami_random_add_entropy(&value, sizeof(value), 0);
}
