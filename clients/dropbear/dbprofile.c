/* clients/dropbear/dbprofile.c: per-primitive call counts and E-Clock ticks for
 * an SSH handshake, via -Wl,--wrap.  Rows nest (see p_inside), so only the
 * non-nested rows may be summed.  SPDX-License-Identifier: MIT */

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
    "select(), network wait"
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
   __udivdi3 call supplied by src/common/ami_udivdi3.c, which is fine on a
   report path and would not be fine inside a measurement. */
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

    /* stderr, not stdout: stdout prints nothing at all under ClientRun on this
       platform, while Dropbear's own fprintf(stderr) comes through. */
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


/* Not armed from the constructor: it runs before this crt0 has finished setting
   newlib up, so an atexit() registered there does not survive.  Arming happens
   on the first wrapped call; three exits fire and p_reported makes them once. */
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

/* The constructor only takes the starting tick, which is exec and timer.device
   only and therefore safe this early. */
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


/* Only one definition of a wrap symbol can exist, and clients/dropbear/
   amiga_25519.c owns __wrap_dropbear_* when it is linked.  So a profiled fast
   build wraps src/crypto68k's entry points instead; the rows mean the same. */
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
/* ltc_ecc_mulmod is every P-256 scalar multiply -- the kex keypair, the ECDH and
   both halves of an ECDSA verify -- so its call count matters as much as its
   total. */
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

/* Not a crypto row: __real_select is clients/dropbear/amiga_dropbear.c's own
   select(), so this measures every moment spent waiting for the network or a DOS
   handle rather than computing. */
P_WRAP_RET(P_SELECT, int, select,
           (int n, void *r, void *w, void *e, void *t), (n, r, w, e, t))
