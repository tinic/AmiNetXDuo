/*
 * clients/dropbear/dbprofile.c, where an SSH handshake's 84 seconds go.
 *
 * docs/RESEARCH.md 31.5 measured a whole connection (96.06 s, 84.07 s once the
 * optimistic kex guess was off) and could not split it: that attempt
 * timestamped the server's log host-side and the split came out contradicting
 * itself, because a contended build host does not measure a guest.  Here the
 * guest times its own crypto, on the E-Clock, inside the process doing the
 * handshake.  Every row is a call count and a tick total from a real
 * connection to a real OpenSSH, not inferred from reading the code.
 *
 * -Wl,--wrap=SYM, the same mechanism clients/dropbear/build.sh already uses
 * for open/read/write/close.  Every reference to SYM from Dropbear's own
 * objects is redirected to __wrap_SYM here; __real_SYM is the untouched
 * original.  third_party/dropbear stays byte-identical to the tag.
 *
 * --wrap only catches cross-object calls, so curve25519.c's internal calls to
 * its own field arithmetic are invisible here: they are part of the primitive
 * being timed, not a separate row.
 *
 * Some rows call others: crypto_hash() inside ed25519 sign/verify calls
 * sha512_process(), and ltc_ecc_mulmod() calls into libtommath.  Every row is
 * inclusive time and the report says which rows are nested inside which; the
 * call graph does not support an exclusive breakdown.  Adding up the
 * non-nested rows is the meaningful sum.
 *
 * timer.device UNIT_ECLOCK, through the TimerBase that src/common/compat.c
 * already opens for ami_millis(), so there is one timer device open in the
 * process and this file does not open a second.  The 32-bit low word wraps
 * every ~100 minutes at 709 kHz and a difference survives one wrap, which is
 * longer than any run here.
 *
 * ReadEClock() costs a library call.  Everything measured is milliseconds or
 * seconds and the most-called row (chacha, once per packet) is a few dozen
 * calls, so the instrument's own cost is below the resolution of what it
 * reports.  It is charged to the row it is measuring, so it can only make a
 * row look slower.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/timer.h>

#include <stdio.h>
#include <stdlib.h>

#include "aminetxduo/compat.h"

/* compat.c opens UNIT_ECLOCK lazily on the first ami_millis() and publishes
   the base here; the inline ReadEClock() macro resolves through it. */
extern struct Device *TimerBase;


/* ------------------------------------------------------------------ slots */

enum {
    P_X25519,           /* curve25519 scalar multiply: kex keygen and secret  */
    P_ED25519_SIGN,     /* client publickey authentication                     */
    P_ED25519_VERIFY,   /* the server host key's signature over the exchange   */
    P_ECC_MULMOD,       /* P-256 scalar multiply (nistp256 kex/ECDSA builds)   */
    P_ECDH,             /* dropbear_ecc_shared_secret()                        */
    P_EXPTMOD,          /* RSA, and libtommath's modexp wherever it is reached */
    P_MP_INVMOD,        /* ECDSA's modular inverse                             */
    P_SHA512,           /* nested: inside ed25519 sign and verify              */
    P_SHA256,           /* the exchange hash and the key derivation            */
    P_CHACHA,           /* the record path, both directions                    */
    P_POLY1305,         /* the record path's MAC                               */
    P_SELECT,           /* not crypto: time blocked on the network             */
    P_SLOTS
};

static const char * const p_name[P_SLOTS] = {
    "curve25519 scalarmult",
    "ed25519 sign",
    "ed25519 verify",
    "P-256 ltc_ecc_mulmod",
    "P-256 ECDH shared",
    "mp_exptmod (RSA)",
    "mp_invmod",
    "  sha512_process",
    "  sha256_process",
    "  chacha_crypt",
    "  poly1305_process",
    "select() -- network wait"
};

/* Which rows are contained in another row above them, so the report can say
   so instead of leaving somebody to double-count. */
static const char * const p_inside[P_SLOTS] = {
    NULL, NULL, NULL, NULL,
    "P-256 ltc_ecc_mulmod",
    NULL, NULL,
    "ed25519 sign/verify",
    NULL, NULL, NULL, NULL
};

static ULONG p_calls[P_SLOTS];
static ULONG p_ticks[P_SLOTS];

static ULONG p_hz;
static ULONG p_start;
static int   p_reported;
static int   p_armed;


static ULONG p_tick(void)
{
    struct EClockVal ev;

    if (TimerBase == NULL)
        return 0;

    ReadEClock(&ev);
    return ev.ev_lo;
}

static void p_arm(void);

static void p_add(int slot, ULONG t0)
{
    p_arm();
    p_calls[slot]++;
    p_ticks[slot] += p_tick() - t0;
}

/* ticks -> milliseconds, outside every timed region: the divide is a
   __udivdi3 call (this toolchain's libgcc.a is empty and
   src/common/ami_udivdi3.c supplies it), which is fine on a report path and
   would not be fine inside a measurement. */
static ULONG p_ms(ULONG ticks)
{
    if (p_hz == 0)
        return 0;
    return (ULONG)(((unsigned long long)ticks * 1000ULL) /
                   (unsigned long long)p_hz);
}


/* ------------------------------------------------------------ the report */

static void p_report(void)
{
    ULONG total = p_tick() - p_start;
    ULONG named = 0;
    int   i;

    if (p_reported)
        return;
    p_reported = 1;

    for (i = 0; i < P_SLOTS; i++)
        if (p_inside[i] == NULL && i != P_SELECT)
            named += p_ticks[i];

    /*
     * stderr, not stdout.  The first profiling run printed nothing at all
     * through printf(), while Dropbear's own "Caution, skipping hostkey check"
     *, dbutil.c's fprintf(stderr), came through in the same transcript.
     * stderr is the stream that is wired up under ClientRun on this platform.
     */
    fprintf(stderr, "\n--- dbprofile: where this connection went ---\n");
    fprintf(stderr, "%-26s %7s %10s %8s\n", "primitive", "calls", "ms", "% wall");

    for (i = 0; i < P_SLOTS; i++) {
        ULONG ms;
        ULONG pct;

        if (p_calls[i] == 0)
            continue;

        ms  = p_ms(p_ticks[i]);
        pct = (total == 0) ? 0 : (ULONG)(((unsigned long long)p_ticks[i] * 100ULL)
                                         / (unsigned long long)total);

        fprintf(stderr, "%-26s %7lu %10lu %7lu%%%s%s\n",
               p_name[i], (unsigned long)p_calls[i], (unsigned long)ms,
               (unsigned long)pct,
               p_inside[i] ? "   nested in " : "",
               p_inside[i] ? p_inside[i] : "");

        AMI_INFO("dbprofile: %s calls %ld ms %ld",
                 (LONG)p_name[i], (LONG)p_calls[i], (LONG)p_ms(p_ticks[i]));
    }

    fprintf(stderr, "%-26s %7s %10lu %7lu%%\n", "public-key subtotal", "",
           (unsigned long)p_ms(named),
           (unsigned long)(total ? (((unsigned long long)named * 100ULL) /
                                    (unsigned long long)total) : 0));
    fprintf(stderr, "%-26s %7s %10lu %7s\n", "whole process", "",
           (unsigned long)p_ms(total), "100%");
    fprintf(stderr, "--- eclock %lu Hz ---\n\n", (unsigned long)p_hz);

    AMI_INFO("dbprofile: pk subtotal %ld ms of %ld ms wall",
             (LONG)p_ms(named), (LONG)p_ms(total));

    fflush(stderr);
}


/*
 * Arming is not done in the constructor.  The first version registered
 * p_report() with atexit() from a constructor and nothing was ever printed.
 * The constructor does run, the linked ___CTOR_LIST__ grew by one entry when
 * this file joined the link, but it runs before this crt0 has finished
 * setting newlib up, so an atexit() registered there does not survive and an
 * fprintf() from there goes nowhere.  amiga_dropbear.c's own constructor gets
 * away with it because it touches only dos.library.
 *
 * Registration therefore happens on the first wrapped call, which is inside
 * main() by construction.  Three exits are then armed and p_reported makes
 * them idempotent: atexit(), the toolchain's own DTOR list, and --wrap=exit.
 */
static void p_arm(void)
{
    if (p_armed)
        return;
    p_armed = 1;
    atexit(p_report);
}

__attribute__((destructor)) static void p_fini(void)
{
    p_report();
}

extern void __real_exit(int status);
void __wrap_exit(int status);
void __wrap_exit(int status)
{
    p_report();
    __real_exit(status);
}

/*
 * The constructor only takes the starting tick, which is exec and timer.device
 * only and therefore safe this early, the same reason amiga_dropbear.c's
 * constructor works.
 */
__attribute__((constructor)) static void p_init(void)
{
    struct EClockVal ev;

    (void)ami_millis();                 /* forces compat.c to open the timer */

    if (TimerBase != NULL) {
        p_hz = ReadEClock(&ev);
        p_start = ev.ev_lo;
    }
    if (p_hz == 0)
        p_hz = 709379UL;                /* PAL, if the device lies */
}


/* -------------------------------------------------------------- wrappers */

#define P_WRAP_VOID(slot, ret, name, params, args)                            \
    extern ret __real_##name params;                                          \
    ret __wrap_##name params;                                                 \
    ret __wrap_##name params                                                  \
    {                                                                         \
        ULONG t0 = p_tick();                                                  \
        __real_##name args;                                                   \
        p_add(slot, t0);                                                      \
    }

#define P_WRAP_RET(slot, ret, name, params, args)                             \
    extern ret __real_##name params;                                          \
    ret __wrap_##name params;                                                 \
    ret __wrap_##name params                                                  \
    {                                                                         \
        ULONG t0 = p_tick();                                                  \
        ret   rv = __real_##name args;                                        \
        p_add(slot, t0);                                                      \
        return rv;                                                            \
    }


/*
 * Two sets of names for the same three rows.
 *
 * With clients/dropbear/amiga_25519.c linked, that file owns
 * __wrap_dropbear_* and this one cannot: only one definition of a wrap symbol
 * can exist.  So when the accelerated build is being profiled the instrument
 * moves one level down and wraps src/crypto68k's own entry points, which
 * amiga_25519.o reaches across an object boundary and --wrap still catches.
 * The rows mean the same thing either way, so the A/B is comparable.
 */
#if DBPROF_FAST25519

P_WRAP_RET(P_X25519, int, c68k_x25519,
           (unsigned char *q, const unsigned char *n, const unsigned char *p),
           (q, n, p))

P_WRAP_VOID(P_ED25519_SIGN, void, c68k_ed25519_sign,
            (void *h, unsigned char *s, const unsigned char *m,
             unsigned long mlen, const unsigned char *sk,
             const unsigned char *pk),
            (h, s, m, mlen, sk, pk))

P_WRAP_RET(P_ED25519_VERIFY, int, c68k_ed25519_verify,
           (void *h, const unsigned char *m, unsigned long mlen,
            const unsigned char *s, const unsigned char *pk),
           (h, m, mlen, s, pk))

#else

#if DBPROF_CURVE25519
P_WRAP_VOID(P_X25519, void, dropbear_curve25519_scalarmult,
            (unsigned char *q, const unsigned char *n, const unsigned char *p),
            (q, n, p))
#endif

#if DBPROF_ED25519
P_WRAP_VOID(P_ED25519_SIGN, void, dropbear_ed25519_sign,
            (const unsigned char *m, unsigned long mlen, unsigned char *s,
             unsigned long *slen, const unsigned char *sk,
             const unsigned char *pk),
            (m, mlen, s, slen, sk, pk))

P_WRAP_RET(P_ED25519_VERIFY, int, dropbear_ed25519_verify,
           (const unsigned char *m, unsigned long mlen, const unsigned char *s,
            unsigned long slen, const unsigned char *pk),
           (m, mlen, s, slen, pk))
#endif

#endif /* DBPROF_FAST25519 */

#if DBPROF_ECC
/*
 * The two libtomcrypt/libtommath entry points a P-256 handshake goes through.
 * ltc_ecc_mulmod is every scalar multiply, the kex keypair, the ECDH, and
 * both halves of an ECDSA verify, so its call count matters as much as its
 * total.
 */
P_WRAP_RET(P_ECC_MULMOD, int, ltc_ecc_mulmod,
           (void *k, void *G, void *R, void *modulus, int map),
           (k, G, R, modulus, map))

P_WRAP_RET(P_ECDH, void *, dropbear_ecc_shared_secret,
           (void *pub_key, const void *priv_key), (pub_key, priv_key))

P_WRAP_RET(P_MP_INVMOD, int, mp_invmod,
           (const void *a, const void *b, void *c), (a, b, c))
#endif

#if DBPROF_RSA
P_WRAP_RET(P_EXPTMOD, int, mp_exptmod,
           (const void *G, const void *X, const void *P, void *Y),
           (G, X, P, Y))
#endif

P_WRAP_RET(P_SHA512, int, sha512_process,
           (void *md, const unsigned char *in, unsigned long inlen),
           (md, in, inlen))

P_WRAP_RET(P_SHA256, int, sha256_process,
           (void *md, const unsigned char *in, unsigned long inlen),
           (md, in, inlen))

P_WRAP_RET(P_CHACHA, int, chacha_crypt,
           (void *st, const unsigned char *in, unsigned long inlen,
            unsigned char *out),
           (st, in, inlen, out))

P_WRAP_RET(P_POLY1305, int, poly1305_process,
           (void *st, const unsigned char *in, unsigned long inlen),
           (st, in, inlen))

/*
 * Not a crypto row.  select() is clients/dropbear/amiga_dropbear.c's own, so
 * __real_select is that function, and what is measured is every moment this
 * process spent waiting for the network or for a DOS handle rather than
 * computing.  If the public-key rows and this one together account for the
 * wall clock, nothing unnamed is hiding in the handshake.
 */
P_WRAP_RET(P_SELECT, int, select,
           (int n, void *r, void *w, void *e, void *t), (n, r, w, e, t))
