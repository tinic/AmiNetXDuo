/*
 * The entropy pool and the hash DRBG behind it.
 *
 * THE SOURCE ASKED FOR THIS FILE.  ami_random.c:24-25, over its SHA-256:
 *
 *     FIPS 180-4, self-contained: no libc, no nx_crypto.  Re-check against
 *     the published vectors in a host harness after ANY edit here.
 *
 * There was no host harness.  A self-contained SHA-256 with no test is a
 * hash nobody has ever checked against the standard it names, and this one is
 * not decorative: ami_random_bytes() feeds tls_random.c, tls_conn.c and
 * tls_resume.c, and ami_random_ulong() is reached from bsdsocket's socket.c.
 * A compression function with a transcription error in its constants still
 * produces plausible-looking bytes.
 *
 * WHAT IS GROUND TRUTH HERE AND WHAT IS NOT.  The SHA-256 vectors are FIPS
 * 180-4's own and NIST's byte-oriented CAVS set -- genuinely independent of
 * this tree, unlike the ISA PnP checksum in test_netdev_isapnp.c where no
 * published vector was to hand and a second transcription had to stand in.
 * The DRBG has no published vectors to check against, so what is asserted
 * about it is the properties a generator must have rather than an output:
 * that it does not repeat, does not return a constant, and does not hand out
 * bytes before it is seeded.
 *
 * THE GATHERERS RUN, and their VALUES are deliberately not asserted.  Entropy
 * is entropy: gather_exec() may return anything.  What a test can say is that
 * they do not crash, that they are called, and that the credit they claim is
 * the credit the pool counts.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stddef.h>
#include <string.h>

/*
 * calloc/free by hand rather than <stdlib.h>.  That header pulls glibc's
 * <sys/time.h> under this build's flags, and glibc's struct timeval is not
 * the Amiga's -- tv_sec/tv_usec against tv_secs/tv_micro, one tag, two
 * shapes.  tests/bsdsocket/host/shim hits the same wall and solves it by
 * renaming the tag in a prelude; here it is cheaper not to pull the header.
 */
extern void *calloc(size_t nmemb, size_t size);
extern void  free(void *ptr);

/*
 * The threads and processes of the first-use race (N-094).  Host headers
 * FIRST, then the Amiga's timeval renamed, as tests/bsdsocket/host/shim/
 * host_prelude.h does: macOS's <pthread.h> brings POSIX struct timeval in,
 * and src/common/test/shim/devices/timer.h then defines the Amiga's under
 * the same tag.  Unix hosts only, which are the hosts this tier runs on.
 */
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#define timeval ami_timeval

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/lists.h>
#include <exec/tasks.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/timer.h>

/*
 * The beam, which a host does not have.  gather_jitter() spins reading
 * $DFF006 and that is a SEGV here, so the register is a variable the test
 * advances -- it has to MOVE, because the gatherer's whole purpose is to
 * measure how far it got between two clock reads.
 */
static UWORD fake_vhposr;
#define CUSTOM_VHPOSR   (&fake_vhposr)

#include "ami_random.c"

static int failures;
static int checks;

static void expect(int ok, const char *what)
{
    checks++;
    if (ok)
        return;

    printf("FAIL %s\n", what);
    failures++;
}

/* ---------------------------------------------------------------- world -- */

static struct ExecBase  exec_base;
static struct Task      me;
/*
 * A plausible task list.  gather_tasks() credits 2 bits and hashes the stack
 * pointer and priority of every live task -- "a bare boot has the same five
 * tasks every time, a real Workbench far more" -- so a machine with ONE task
 * genuinely has little entropy, and the pool saying so is the code being
 * right.  Six tasks is a quiet boot.
 */
static struct Task      others[6];
static struct Library  *timer_base_value;
struct ExecBase        *SysBase   = &exec_base;
struct Library         *TimerBase;

static int   forbid_depth;
static ULONG clock_ticks;

void Disable(void) { }
void Enable(void)  { }

/*
 * Forbid() excludes every other task, so here it is a recursive lock: a
 * second thread's Forbid() waits for the first's Permit(), as the scheduler
 * would make it.  forbid_depth is only touched by the holder.
 */
static pthread_mutex_t h_forbid_mx;
static pthread_once_t  h_forbid_once = PTHREAD_ONCE_INIT;

static void h_forbid_init(void)
{
    pthread_mutexattr_t a;

    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&h_forbid_mx, &a);
    pthread_mutexattr_destroy(&a);
}

void Forbid(void)
{
    pthread_once(&h_forbid_once, h_forbid_init);
    pthread_mutex_lock(&h_forbid_mx);
    forbid_depth++;
}

void Permit(void)
{
    if (forbid_depth <= 0)
    {
        printf("FAIL Permit() without a matching Forbid()\n");
        failures++;
        forbid_depth = 0;
        return;
    }
    forbid_depth--;
    pthread_mutex_unlock(&h_forbid_mx);
}

/* The task each host thread is.  NULL: the main thread, which is `me'. */
static __thread struct Task *h_self;

struct Task *FindTask(STRPTR name)
{
    (void)name;
    return h_self != NULL ? h_self : &me;
}

/*
 * A SignalSemaphore that blocks a DIFFERENT task for real and nests for the
 * owner, as Exec's does.  h_sem_waits counts the times a caller found it held
 * by someone else and had to wait.
 */
static pthread_mutex_t h_sem_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  h_sem_cv = PTHREAD_COND_INITIALIZER;
static int             h_sem_waits;

void InitSemaphore(struct SignalSemaphore *sem)
{
    memset(sem, 0, sizeof(*sem));
}

void ObtainSemaphore(struct SignalSemaphore *sem)
{
    struct Task *self = FindTask(NULL);

    pthread_mutex_lock(&h_sem_mx);
    if (sem->ss_Owner != NULL && sem->ss_Owner != self)
    {
        h_sem_waits++;
        pthread_cond_broadcast(&h_sem_cv);
        while (sem->ss_Owner != NULL && sem->ss_Owner != self)
            pthread_cond_wait(&h_sem_cv, &h_sem_mx);
    }
    sem->ss_Owner = self;
    sem->ss_NestCount++;
    pthread_mutex_unlock(&h_sem_mx);
}

void ReleaseSemaphore(struct SignalSemaphore *sem)
{
    pthread_mutex_lock(&h_sem_mx);
    if (--sem->ss_NestCount == 0)
        sem->ss_Owner = NULL;
    pthread_cond_broadcast(&h_sem_cv);
    pthread_mutex_unlock(&h_sem_mx);
}

ULONG AvailMem(ULONG requirements)
{
    /* Different answers per class, and moving, because a gatherer that asked
       once and reused the answer would look identical with a constant. */
    return 0x00100000UL + (requirements * 7UL) + (clock_ticks << 4);
}

APTR AllocVec(ULONG size, ULONG requirements)
{
    (void)requirements;
    return size != 0 ? calloc(1, size) : NULL;
}

void FreeVec(APTR memory)
{
    free(memory);
}

/*
 * A clock that advances.  ReadEClock() is the jitter source: gather_jitter()
 * reads it twice around a spin and mixes the difference, so a clock that did
 * not move would make that gatherer contribute a constant -- which is a state
 * worth being able to produce on purpose, hence clock_frozen.
 */
static int clock_frozen;

/*
 * NOT a constant step.  gather_jitter() credits entropy only when successive
 * samples DIFFER -- it compares every jitter reading against the first and
 * credits nothing when they all match -- so a clock advancing by a fixed
 * amount makes the gatherer return 0 and the pool never reaches its seeding
 * threshold.  That is the code being right about a machine whose clock has no
 * jitter, and it cost a failing assertion here before the model was fixed
 * rather than the assertion weakened.
 */
static ULONG jitter_state = 0x2545F491UL;

ULONG ReadEClock(struct EClockVal *dest)
{
    if (!clock_frozen)
    {
        jitter_state = jitter_state * 1103515245UL + 12345UL;
        clock_ticks += 7919UL + ((jitter_state >> 16) & 0x3fUL);
    }
    fake_vhposr = (UWORD)(fake_vhposr + 313u);   /* a PAL frame of raster */
    dest->ev_hi = clock_ticks >> 16;
    dest->ev_lo = clock_ticks;

    return 709379UL;            /* PAL E-clock, near enough */
}

VOID GetSysTime(struct timeval *dest)
{
    if (!clock_frozen)
        clock_ticks += 31UL;
    dest->tv_secs  = clock_ticks / 50UL;
    dest->tv_micro = (clock_ticks % 50UL) * 20000UL;
}

/*
 * compat.c's millisecond clock.  ami_random.c uses it to decide how long ago
 * it last gathered, so it has to advance with the same clock the gatherers
 * see or the pool would think no time had passed.
 */
static void h_race_start(void);

ULONG ami_millis(VOID)
{
    /* random_gather() calls this first, outside any Forbid(): the point at
       which another task's first call arrives in the race cases. */
    if (h_self == NULL)
        h_race_start();

    return clock_ticks / 50UL;
}

/* The shim has no NewList(); a list is three stores. */
static void NewList_(struct List *l)
{
    l->lh_Head     = (struct Node *)&l->lh_Tail;
    l->lh_Tail     = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

static void world_init(void)
{
    memset(&exec_base, 0, sizeof(exec_base));
    memset(&me, 0, sizeof(me));

    exec_base.IdleCount            = 1234;
    exec_base.DispCount            = 5678;
    exec_base.Quantum              = 4;
    exec_base.Elapsed              = 2;
    exec_base.SysFlags             = 0x8000;
    exec_base.AttnFlags            = 0x0002;      /* AFF_68020 */
    exec_base.VBlankFrequency      = 50;
    exec_base.PowerSupplyFrequency = 50;
    exec_base.ex_EClockFrequency   = 709379UL;
    exec_base.ThisTask             = &me;

    NewList_(&exec_base.TaskReady);
    NewList_(&exec_base.TaskWait);

    {
        UWORD t;

        memset(others, 0, sizeof(others));
        for (t = 0; t < 6u; t++)
        {
            struct List *l = (t & 1u) ? &exec_base.TaskWait
                                      : &exec_base.TaskReady;

            others[t].tc_Node.ln_Pri = (BYTE)(t * 5u);
            others[t].tc_SPReg       = (APTR)(APTR)&others[t];
            others[t].tc_SPUpper     = (APTR)(&others[t] + 1);
            others[t].tc_Node.ln_Succ = (struct Node *)&l->lh_Tail;
            others[t].tc_Node.ln_Pred = l->lh_TailPred;
            l->lh_TailPred->ln_Succ   = &others[t].tc_Node;
            l->lh_TailPred            = &others[t].tc_Node;
        }
    }

    timer_base_value = (struct Library *)(APTR)&exec_base;
    TimerBase        = timer_base_value;

    clock_frozen = 0;
    forbid_depth = 0;
}

/* ============================================================== SHA-256 == */

static void hex(const UBYTE *d, char *out)
{
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 32; i++)
    {
        out[i * 2]     = digits[(d[i] >> 4) & 0x0f];
        out[i * 2 + 1] = digits[d[i] & 0x0f];
    }
    out[64] = '\0';
}

static void sha_of(const void *msg, ULONG len, char *out)
{
    Sha256 ctx;
    UBYTE  digest[32];

    sha256_init(&ctx);
    sha256_update(&ctx, msg, len);
    sha256_final(&ctx, digest);
    hex(digest, out);
}

/*
 * FIPS 180-4 and the NIST CAVS byte-oriented set.  These are the published
 * answers, not something this tree computed: an implementation that agrees
 * with all of them is SHA-256 and one that disagrees with any is not.
 */
static void a_fips_vectors(void)
{
    static const struct { const char *msg; const char *want; } v[] =
    {
        /* The empty message. */
        { "",
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        /* FIPS 180-4 B.1, one block. */
        { "abc",
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        /* FIPS 180-4 B.2, two blocks -- the length crosses 56 bytes, which is
           the padding branch a one-block test never reaches. */
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
        /* 448 bits exactly: the length field lands in the last eight bytes of
           its own block, so this is the case that needs a SECOND block of
           padding and nothing else. */
        { "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
          "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
          "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1" },
        { "a",
          "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb" },
        { "message digest",
          "f7846f55cf23e14eebeab5b4e1550cad5b509e3348fbc4efa3a1413d393cb650" },
        { "abcdefghijklmnopqrstuvwxyz",
          "71c480df93d6ae2f1efad1447c66c9525e316218cf51fc8d9ed832f2daf18b73" },
        { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
          "db4bfcbd4da0cd85a60c3c37d3fbd8805c77f15fc6b1fdfe614ee0a7c8fdb4c0" }
    };
    char got[65];
    UWORD i;

    for (i = 0; i < (UWORD)(sizeof(v) / sizeof(v[0])); i++)
    {
        char what[96];

        sha_of(v[i].msg, (ULONG)strlen(v[i].msg), got);
        snprintf(what, sizeof(what), "FIPS vector %u (%lu bytes)",
                 i, (unsigned long)strlen(v[i].msg));
        if (strcmp(got, v[i].want) != 0)
            printf("     got  %s\n     want %s\n", got, v[i].want);
        expect(strcmp(got, v[i].want) == 0, what);
    }

    /* One million 'a', the classic long vector: 15,625 whole blocks, so every
       path through the block loop runs many times and the length field is
       larger than a byte count that fits in one. */
    {
        Sha256 ctx;
        UBYTE  digest[32];
        UBYTE  chunk[1000];
        int    n;

        memset(chunk, 'a', sizeof(chunk));
        sha256_init(&ctx);
        for (n = 0; n < 1000; n++)
            sha256_update(&ctx, chunk, (ULONG)sizeof(chunk));
        sha256_final(&ctx, digest);
        hex(digest, got);
        if (strcmp(got, "cdc76e5c9914fb9281a1c7e284d73e67"
                        "f1809a48a497200e046d39ccc7112cd0") != 0)
            printf("     got %s\n", got);
        expect(strcmp(got, "cdc76e5c9914fb9281a1c7e284d73e67"
                           "f1809a48a497200e046d39ccc7112cd0") == 0,
               "FIPS long vector, one million 'a'");
    }
}

/*
 * The same message fed in every split there is.  A compression function that
 * is right only when the caller happens to hand it whole blocks is wrong, and
 * the pool feeds it whatever a gatherer produced.
 */
static void b_update_is_split_independent(void)
{
    static const UBYTE msg[200] = { 0 };
    char   whole[65];
    UWORD  split;
    int    bad = 0;

    sha_of(msg, (ULONG)sizeof(msg), whole);

    for (split = 1; split < (UWORD)sizeof(msg); split++)
    {
        Sha256 ctx;
        UBYTE  digest[32];
        char   got[65];

        sha256_init(&ctx);
        sha256_update(&ctx, msg, split);
        sha256_update(&ctx, msg + split, (ULONG)sizeof(msg) - split);
        sha256_final(&ctx, digest);
        hex(digest, got);

        if (strcmp(got, whole) != 0)
        {
            printf("     split at %u differs\n", split);
            bad = 1;
            break;
        }
    }

    expect(!bad, "every split of one message hashes the same");

    /* And a zero-length update in the middle changes nothing. */
    {
        Sha256 ctx;
        UBYTE  digest[32];
        char   got[65];

        sha256_init(&ctx);
        sha256_update(&ctx, msg, 100);
        sha256_update(&ctx, msg, 0);
        sha256_update(&ctx, msg + 100, 100);
        sha256_final(&ctx, digest);
        hex(digest, got);
        expect(strcmp(got, whole) == 0, "a zero-length update is a no-op");
    }
}

/* ================================================================ DRBG === */

static int all_same(const UBYTE *p, ULONG n)
{
    ULONG i;

    for (i = 1; i < n; i++)
    {
        if (p[i] != p[0])
            return 0;
    }

    return 1;
}

/*
 * There are no published vectors for this construction, so what is asserted
 * is what a generator must do rather than what it must output.
 */
static void c_the_generator(void)
{
    UBYTE a[256];
    UBYTE b[256];
    UWORD i;

    world_init();
    ami_random_init();

    /*
     * THE THRESHOLD IS REAL, and this is the assertion worth having.  A
     * gather on this machine credits 23 bits against AMI_RANDOM_MIN_BITS of
     * 64, so the pool reports itself NOT seeded -- which is correct: six
     * tasks, a fake ExecBase and a synthetic clock genuinely are not 64 bits
     * of entropy.  An earlier draft asserted is_seeded()==TRUE here and the
     * honest fix was to assert the gate rather than tune the fakes until it
     * opened, because a test that forces TRUE cannot tell a working threshold
     * from one that always answers yes.
     */
    expect(ami_random_entropy_bits() > 0,
           "a gather credits the entropy it found");
    expect(ami_random_entropy_bits() < AMI_RANDOM_MIN_BITS,
           "and a quiet machine does not reach the seeding threshold");
    expect(ami_random_is_seeded() == FALSE,
           "so the pool says it is not seeded");
    expect(forbid_depth == 0, "with Forbid balanced");

    /* Enough credited entropy, and it flips -- so the gate is a threshold and
       not a constant. */
    {
        static const UBYTE seed[32] = { 9, 8, 7, 6, 5, 4, 3, 2 };

        ami_random_add_entropy(seed, sizeof(seed), AMI_RANDOM_MIN_BITS);
        expect(ami_random_entropy_bits() >= AMI_RANDOM_MIN_BITS,
               "credited entropy raises the estimate past the threshold");
        expect(ami_random_is_seeded() == TRUE, "and the pool is seeded");
    }

    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    ami_random_bytes(a, sizeof(a));
    ami_random_bytes(b, sizeof(b));

    expect(!all_same(a, sizeof(a)), "a fill is not one repeated byte");
    expect(memcmp(a, b, sizeof(a)) != 0,
           "two fills of the same length differ");

    /*
     * Every length, including the ones that are not a multiple of the hash
     * output: a generator that only works in 32-byte units would leave the
     * tail of an odd request untouched.
     *
     * A SENTINEL COMPARISON IS PROBABILISTIC AND THIS ONE FIRED.  It used to
     * preset the buffer to 0xa5, fill it ONCE, and fail if no byte differed --
     * so a correct generator failed whenever a 1-byte request happened to
     * produce 0xa5.  That is one draw in 256, and it reddened CI on macOS
     * (run 34427007666, "FAIL a 1-byte fill wrote something").  Measured on
     * the built binary rather than argued: 11 failures in 3000 runs, 1 in 273
     * against the 1 in 256 the mechanism predicts.
     *
     * The claim worth making does not depend on any single draw.  Repeat until
     * the tail byte takes a value other than the sentinel and fail only if it
     * NEVER does: a generator that does not write there fails every time, and
     * a correct one has a 256^-FILL_DRAWS chance of looking like it.  The
     * bounds check below stays exact -- writing past the end is not
     * probabilistic and is checked on every draw.
     */
#define FILL_DRAWS 16u
    for (i = 1; i <= 64u; i++)
    {
        UBYTE buf[65];
        UWORD d;
        int   tail_written = 0;
        int   overran = 0;

        for (d = 0; d < FILL_DRAWS; d++)
        {
            memset(buf, 0xa5, sizeof(buf));
            ami_random_bytes(buf, i);

            if (buf[i - 1u] != 0xa5)
                tail_written = 1;
            if (buf[i] != 0xa5)
            {
                overran = 1;
                break;
            }
        }

        if (!tail_written)
        {
            char what[72];

            snprintf(what, sizeof(what),
                     "a %u-byte fill wrote its last byte in %u draws", i,
                     (unsigned)FILL_DRAWS);
            expect(0, what);
            break;
        }
        if (overran)
        {
            char what[64];

            snprintf(what, sizeof(what), "a %u-byte fill stayed in bounds", i);
            expect(0, what);
            break;
        }
    }
    checks += 2;

    /* A zero-length request is not a special case that writes anyway. */
    {
        UBYTE guard[4];

        memset(guard, 0x5a, sizeof(guard));
        ami_random_bytes(guard, 0);
        expect(guard[0] == 0x5a && guard[3] == 0x5a,
               "a zero-length fill writes nothing");
        ami_random_bytes(NULL, 16);
        checks++;               /* reaching here is the assertion */
    }
}

/* Successive ULONGs are not the same number, and not a counter. */
static void d_ulongs_move(void)
{
    ULONG seen[64];
    UWORD i;
    UWORD j;
    int   dup = 0;
    int   consecutive = 0;

    world_init();
    ami_random_init();

    for (i = 0; i < 64u; i++)
        seen[i] = ami_random_ulong();

    for (i = 0; i < 64u; i++)
    {
        for (j = (UWORD)(i + 1u); j < 64u; j++)
        {
            if (seen[i] == seen[j])
                dup++;
        }
        if (i > 0 && seen[i] == seen[i - 1] + 1UL)
            consecutive++;
    }

    expect(dup == 0, "64 ULONGs with no repeat");
    expect(consecutive < 8, "and they are not a counter");
}

/*
 * Entropy added by a caller is credited, and the credit is bounded: a caller
 * claiming a million bits from four bytes must not be believed, or the pool
 * reports itself seeded when it is not.
 */
static void e_added_entropy_is_credited_and_bounded(void)
{
    static const UBYTE junk[32] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    ULONG before;
    ULONG after;

    world_init();
    ami_random_init();

    before = ami_random_entropy_bits();
    ami_random_add_entropy(junk, sizeof(junk), 16);
    after = ami_random_entropy_bits();
    expect(after >= before, "added entropy does not lower the estimate");

    /* An absurd claim from a few bytes. */
    ami_random_add_entropy(junk, 4, 0xffffffffUL);
    expect(ami_random_entropy_bits() >= after,
           "and an absurd credit does not underflow the estimate");

    /* NULL and zero length are handled rather than dereferenced. */
    ami_random_add_entropy(NULL, 16, 8);
    ami_random_add_entropy(junk, 0, 8);
    checks++;
    expect(forbid_depth == 0, "Forbid balanced across the additions");
}

/*
 * The arrival hook runs from the receive path -- ami_random_arrival() is
 * called per frame -- so it has to be cheap and it has to survive being
 * called before init.  Both are asserted here because the second one is a
 * crash on a machine and nothing else would show it.
 */
static void f_arrival_before_init(void)
{
    UWORD i;

    world_init();

    for (i = 0; i < 64u; i++)
        ami_random_arrival();
    checks++;

    ami_random_init();
    for (i = 0; i < 1000u; i++)
        ami_random_arrival();

    expect(forbid_depth == 0, "arrivals leave Forbid balanced");
    expect(ami_random_entropy_bits() > 0,
           "and a thousand arrivals leave the estimate standing");
}

/* A concurrent old flush can leave the credit just beyond its ceiling before
   another batch reaches its clamp.  The clamp must saturate, not subtract an
   unsigned value from a smaller ceiling and credit yet more bits. */
static void f_arrival_credit_ceiling(void)
{
    UBYTE batch[ARRIVAL_BATCH];
    UWORD i;

    world_init();
    for (i = 0; i < ARRIVAL_BATCH; i++)
        batch[i] = (UBYTE)(i & 1u);

    arrival_bits = AMI_RANDOM_ARRIVAL_MAX_BITS + 1UL;
    arrival_batches = 0;
    arrival_barren = 0;
    arrival_done = FALSE;
    pool_bits = 0;

    arrival_flush(batch);
    expect(arrival_bits == AMI_RANDOM_ARRIVAL_MAX_BITS,
           "arrival credit saturates at the ceiling after an interleaving");
    expect(arrival_done && arrival_batches == 1,
           "and closes the arrival gate once");
    arrival_flush(batch);
    expect(arrival_batches == 1 && forbid_depth == 0,
           "a queued batch after the gate closes does not add credit");
}

/*
 * A clock that does not move.  gather_jitter() and gather_clock() then
 * contribute a constant, which is the shape of a machine with no working
 * timer.device -- the generator must still produce differing output from its
 * other sources rather than repeating.
 */
static void g_a_frozen_clock_does_not_freeze_the_output(void)
{
    UBYTE a[64];
    UBYTE b[64];

    world_init();
    ami_random_init();
    clock_frozen = 1;

    ami_random_bytes(a, sizeof(a));
    ami_random_bytes(b, sizeof(b));

    expect(memcmp(a, b, sizeof(a)) != 0,
           "output still differs when the clock is stopped");
    expect(!all_same(a, sizeof(a)), "and is not one repeated byte");

    /* And with no timer.device at all. */
    TimerBase = NULL;
    ami_random_bytes(a, sizeof(a));
    ami_random_bytes(b, sizeof(b));
    expect(memcmp(a, b, sizeof(a)) != 0,
           "and with no timer.device open either");
    TimerBase = timer_base_value;
}


/* ===================================================== first-use race == */

/*
 * N-094.  Task A runs the first collection; task B makes its own first call
 * while A is inside it.  B is a real second thread with its own task, so the
 * pool's lock blocks it rather than nesting.  Each case runs in a child
 * process so it starts from a pool nothing has touched.
 */
enum { RACE_NONE, RACE_BYTES, RACE_INIT, RACE_ADD };

static int          h_race;
static struct Task  h_task_b;
static pthread_t    h_thread_b;
static int          h_b_started;
static int          h_b_create_failed;
static volatile int h_b_done;
static UBYTE        h_b_out[32];
static ULONG        h_b_bits;

static void *h_task_b_main(void *arg)
{
    static const UBYTE extra[4] = { 1, 2, 3, 4 };

    (void)arg;
    h_self = &h_task_b;

    if (h_race == RACE_BYTES)
        ami_random_bytes(h_b_out, sizeof(h_b_out));
    else if (h_race == RACE_INIT)
        ami_random_init();
    else
        ami_random_add_entropy(extra, sizeof(extra), 0);

    h_b_bits = pool_bits;

    pthread_mutex_lock(&h_sem_mx);
    h_b_done = 1;
    pthread_cond_broadcast(&h_sem_cv);
    pthread_mutex_unlock(&h_sem_mx);

    return NULL;
}

/* In A's collection: start B, and go on only once B has finished (the old
   behaviour) or is waiting for the pool (the new one). */
static void h_race_start(void)
{
    struct timespec until;
    int             waits;

    if (h_race == RACE_NONE || h_b_started)
        return;
    h_b_started = 1;

    pthread_mutex_lock(&h_sem_mx);
    waits = h_sem_waits;
    pthread_mutex_unlock(&h_sem_mx);

    if (pthread_create(&h_thread_b, NULL, h_task_b_main, NULL) != 0)
    {
        h_b_create_failed = 1;
        return;
    }

    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 5;

    pthread_mutex_lock(&h_sem_mx);
    while (!h_b_done && h_sem_waits == waits)
        if (pthread_cond_timedwait(&h_sem_cv, &h_sem_mx, &until) != 0)
            break;
    pthread_mutex_unlock(&h_sem_mx);
}

/* SHA-256('G' || 32 zero bytes || counter): what the unmixed key yields. */
static int h_is_zero_key_output(const UBYTE *out)
{
    ULONG ctr;

    for (ctr = 0; ctr < 4; ctr++)
    {
        UBYTE  zero[32];
        UBYTE  tag = DOMAIN_GENERATE;
        UBYTE  d[32];
        Sha256 ctx;

        memset(zero, 0, sizeof(zero));
        sha256_init(&ctx);
        sha256_update(&ctx, &tag, 1);
        sha256_update(&ctx, zero, sizeof(zero));
        sha256_update(&ctx, &ctr, sizeof(ctr));
        sha256_final(&ctx, d);
        if (memcmp(d, out, 32) == 0)
            return 1;
    }

    return 0;
}

static int h_race_child(int which)
{
    world_init();
    h_race = which;

    struct timespec until;

    /* Bounded twice: the whole child dies on SIGALRM, which the parent
       counts as a failure, and B's finish is awaited with a timeout. */
    alarm(30);

    ami_random_init();                      /* task A, the first collection */

    expect(h_b_started && !h_b_create_failed,
           "N-094: the other task's call arrived mid-collection");
    if (!h_b_started || h_b_create_failed)
        return 1;

    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 10;
    pthread_mutex_lock(&h_sem_mx);
    while (!h_b_done)
        if (pthread_cond_timedwait(&h_sem_cv, &h_sem_mx, &until) != 0)
            break;
    pthread_mutex_unlock(&h_sem_mx);

    expect(h_b_done, "N-094: the other task finished once the collection did");
    if (!h_b_done)
        return 1;
    expect(pthread_join(h_thread_b, NULL) == 0, "N-094: and was joined");
    expect(h_sem_waits >= 1,
           "N-094: it WAITED for the collection in progress");

    if (which == RACE_BYTES)
        expect(!h_is_zero_key_output(h_b_out),
               "N-094: its first bytes are not SHA-256('G'||0^32||n)");
    else if (which == RACE_INIT)
        expect(h_b_bits > 0,
               "N-094: a concurrent ami_random_init() returns with entropy in");
    else
        expect(h_b_bits > 0,
               "N-094: a concurrent ami_random_add_entropy() lands after the "
               "first collection");

    expect(forbid_depth == 0, "N-094: Forbid balanced across the race");

    return failures == 0 ? 0 : 1;
}

static void h_first_use_race(void)
{
    static const struct { int which; const char *name; } cases[] = {
        { RACE_BYTES, "N-094: concurrent first ami_random_bytes()" },
        { RACE_INIT,  "N-094: concurrent first ami_random_init()" },
        { RACE_ADD,   "N-094: concurrent first ami_random_add_entropy()" },
    };
    unsigned i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        int   status = 0;
        pid_t pid;

        fflush(stdout);
        pid = fork();
        if (pid == 0)
        {
            int rc = h_race_child(cases[i].which);

            fflush(stdout);
            _exit(rc);
        }

        if (pid > 0 && waitpid(pid, &status, 0) != pid)
            pid = -1;
        expect(pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
               cases[i].name);
    }
}

int main(void)
{
    /* First, while nothing has touched the pool: each case forks from here. */
    h_first_use_race();

    a_fips_vectors();
    b_update_is_split_independent();
    c_the_generator();
    d_ulongs_move();
    e_added_entropy_is_credited_and_bounded();
    f_arrival_before_init();
    f_arrival_credit_ceiling();
    g_a_frozen_clock_does_not_freeze_the_output();

    if (failures != 0)
    {
        printf("ami_random: %d of %d checks failed\n", failures, checks);
        return 1;
    }

    printf("ami_random: %d checks ok\n", checks);

    return 0;
}
