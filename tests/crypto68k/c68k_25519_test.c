/*
 * AmiNetXDuo, on-Amiga check for src/crypto68k/c68k_25519.S.
 *
 * The host tier (tests/crypto68k/host/test_c68k_25519.c) cannot cover this
 * file: it runs portable C on x86-64, and the whole point of the assembly is
 * that it is m68k and that it is DIFFERENT code on each part.  So the same
 * class of check has to run on the machine.
 *
 * Two checks, and the first is the one that matters:
 *
 *   1. c68k_25519_fe_mul (shipped) against c68k_25519_fe_mul_ref (portable C)
 *      over random inputs, INCLUDING the saturated ones.  A field multiply
 *      that is wrong in the carry is the failure this tree has already had
 *      once: fe_fold dropped a carry out of the top limb, lost exactly 38,
 *      and passed every published RFC vector, because with lazy reduction 0
 *      is routinely carried as 2^256-38 and adding 38 to it carries straight
 *      through all eight limbs.  Random inputs against a reference is what
 *      found it then and is what this repeats against the assembly.
 *
 *   2. A Diffie-Hellman agreement through the full c68k_x25519, so the check
 *      covers the ladder and the reduction and not only the multiply.  It
 *      needs no published constant: a*(b*G) and b*(a*G) have to be equal, and
 *      a carry bug anywhere below will separate them.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "c68k_25519.h"

extern void c68k_25519_fe_mul(uint32_t r[8], const uint32_t a[8],
                              const uint32_t b[8]);
extern void c68k_25519_fe_mul_ref(uint32_t r[8], const uint32_t a[8],
                                  const uint32_t b[8]);
extern int  c68k_25519_fe_mul_is_asm(void);
extern void c68k_25519_fe_sqr(uint32_t r[8], const uint32_t a[8]);
extern void c68k_25519_fe_sqr_ref(uint32_t r[8], const uint32_t a[8]);
extern void c68k_25519_fe_add(uint32_t r[8], const uint32_t a[8],
                              const uint32_t b[8]);
extern void c68k_25519_fe_add_ref(uint32_t r[8], const uint32_t a[8],
                                  const uint32_t b[8]);
extern void c68k_25519_fe_sub(uint32_t r[8], const uint32_t a[8],
                              const uint32_t b[8]);
extern void c68k_25519_fe_sub_ref(uint32_t r[8], const uint32_t a[8],
                                  const uint32_t b[8]);

static int failures;

/* xorshift32; the inputs only have to cover the carry space, not be secure. */
static uint32_t rng_state = 0x2545f491u;

static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void ck(const char *what, int ok)
{
    if (!ok) {
        failures++;
        printf("FAIL %s\n", what);
    }
}

static int fe_eq(const uint32_t a[8], const uint32_t b[8])
{
    int i;

    for (i = 0; i < 8; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static void show(const char *tag, const uint32_t v[8])
{
    int i;

    printf("  %s", tag);
    for (i = 7; i >= 0; i--)
        printf(" %08lx", (unsigned long)v[i]);
    printf("\n");
}

/*
 * The interesting inputs are the saturated ones.  0xffffffff throughout is
 * where every limb carries; 2^256-38 and 2^256-37 are how this representation
 * actually carries 0 and 1, which is the case the original bug lived in.
 */
static void fill(uint32_t v[8], int kind)
{
    int i;

    switch (kind) {
    case 0:
        for (i = 0; i < 8; i++)
            v[i] = 0u;
        break;
    case 1:
        for (i = 0; i < 8; i++)
            v[i] = 0xffffffffu;
        break;
    case 2:                                  /* 2^256 - 38 */
        v[0] = 0xffffffdau;
        for (i = 1; i < 8; i++)
            v[i] = 0xffffffffu;
        break;
    case 3:                                  /* 2^256 - 37 */
        v[0] = 0xffffffdbu;
        for (i = 1; i < 8; i++)
            v[i] = 0xffffffffu;
        break;
    case 4:
        v[0] = 1u;
        for (i = 1; i < 8; i++)
            v[i] = 0u;
        break;
    default:
        for (i = 0; i < 8; i++)
            v[i] = rnd();
        break;
    }
}

int main(void)
{
    uint32_t a[8], b[8], got[8], want[8];
    unsigned char ska[32], skb[32], pka[32], pkb[32], s1[32], s2[32];
    int i, j, n;

    printf("fe_mul: %s\n",
           c68k_25519_fe_mul_is_asm() ? "ASSEMBLY" : "portable C");

    /* Every combination of the named edge cases, then a lot of random. */
    for (i = 0; i < 6; i++) {
        for (j = 0; j < 6; j++) {
            fill(a, i);
            fill(b, j);
            c68k_25519_fe_mul(got, a, b);
            c68k_25519_fe_mul_ref(want, a, b);
            if (!fe_eq(got, want)) {
                failures++;
                printf("FAIL fe_mul edge %d x %d\n", i, j);
                show("a   ", a);
                show("b   ", b);
                show("got ", got);
                show("want", want);
            }
        }
    }

    n = 4000;
    for (i = 0; i < n; i++) {
        fill(a, 99);
        fill(b, 99);
        /* Every 16th case pairs a random value with a saturated one, which is
           where a carry that only escapes sometimes shows up. */
        if ((i & 15) == 0)
            fill(b, 1 + (i % 4));
        c68k_25519_fe_mul(got, a, b);
        c68k_25519_fe_mul_ref(want, a, b);
        if (!fe_eq(got, want)) {
            failures++;
            printf("FAIL fe_mul random %d\n", i);
            show("a   ", a);
            show("b   ", b);
            show("got ", got);
            show("want", want);
            break;
        }
    }
    printf("fe_mul vs reference: %d edge + %d random\n", 36, n);

    /* The squaring, the same way.  It is 36 products against the multiply
       64, and a different routine end to end, so a passing fe_mul says
       nothing at all about it. */
    for (i = 0; i < 6; i++) {
        fill(a, i);
        c68k_25519_fe_sqr(got, a);
        c68k_25519_fe_sqr_ref(want, a);
        if (!fe_eq(got, want)) {
            failures++;
            printf("FAIL fe_sqr edge %d\n", i);
            show("a   ", a);
            show("got ", got);
            show("want", want);
        }
    }
    for (i = 0; i < n; i++) {
        fill(a, 99);
        if ((i & 15) == 0)
            fill(a, 1 + (i % 4));
        c68k_25519_fe_sqr(got, a);
        c68k_25519_fe_sqr_ref(want, a);
        if (!fe_eq(got, want)) {
            failures++;
            printf("FAIL fe_sqr random %d\n", i);
            show("a   ", a);
            show("got ", got);
            show("want", want);
            break;
        }
    }
    printf("fe_sqr vs reference: %d edge + %d random\n", 6, n);

    /* And against the multiply.  Checking fe_sqr against fe_mul on random
       inputs is what found the original fe_fold carry bug, because two
       routines that share a broken helper agree with each other and every
       published vector passes anyway. */
    for (i = 0; i < 512; i++) {
        fill(a, 99);
        c68k_25519_fe_sqr(got, a);
        c68k_25519_fe_mul(want, a, a);
        if (!fe_eq(got, want)) {
            failures++;
            printf("FAIL fe_sqr vs fe_mul %d\n", i);
            show("a   ", a);
            show("got ", got);
            show("want", want);
            break;
        }
    }
    printf("fe_sqr vs fe_mul: 512 random\n");

    /* fe_sqr(t, t) is all over the ladder, so alias that too. */
    fill(a, 99);
    c68k_25519_fe_sqr_ref(want, a);
    c68k_25519_fe_sqr(a, a);
    ck("fe_sqr r==a aliasing", fe_eq(a, want));

    /* Aliasing: r may be a or b, and the ladder relies on it. */
    fill(a, 99);
    fill(b, 99);
    memcpy(want, a, sizeof want);
    c68k_25519_fe_mul_ref(got, want, b);
    c68k_25519_fe_mul(a, a, b);              /* r aliases a */
    ck("fe_mul r==a aliasing", fe_eq(a, got));

    fill(a, 99);
    fill(b, 99);
    c68k_25519_fe_mul_ref(got, a, b);
    c68k_25519_fe_mul(b, a, b);              /* r aliases b */
    ck("fe_mul r==b aliasing", fe_eq(b, got));

    /* fe_add and fe_sub.  The saturated inputs matter more here than
       anywhere: fe_add's carry out and fe_sub's borrow out are each worth 38,
       and dropping either is silent.  The edge table is every pair, which is
       where 2^256-38 meets 2^256-37. */
    for (i = 0; i < 6; i++) {
        for (j = 0; j < 6; j++) {
            fill(a, i);
            fill(b, j);
            c68k_25519_fe_add(got, a, b);
            c68k_25519_fe_add_ref(want, a, b);
            if (!fe_eq(got, want)) {
                failures++;
                printf("FAIL fe_add edge %d x %d\n", i, j);
                show("a   ", a); show("b   ", b);
                show("got ", got); show("want", want);
            }
            c68k_25519_fe_sub(got, a, b);
            c68k_25519_fe_sub_ref(want, a, b);
            if (!fe_eq(got, want)) {
                failures++;
                printf("FAIL fe_sub edge %d x %d\n", i, j);
                show("a   ", a); show("b   ", b);
                show("got ", got); show("want", want);
            }
        }
    }
    for (i = 0; i < n; i++) {
        fill(a, 99);
        fill(b, 99);
        if ((i & 15) == 0)
            fill(b, 1 + (i % 4));
        c68k_25519_fe_add(got, a, b);
        c68k_25519_fe_add_ref(want, a, b);
        if (!fe_eq(got, want)) {
            failures++;
            printf("FAIL fe_add random %d\n", i);
            show("a   ", a); show("b   ", b);
            show("got ", got); show("want", want);
            break;
        }
        c68k_25519_fe_sub(got, a, b);
        c68k_25519_fe_sub_ref(want, a, b);
        if (!fe_eq(got, want)) {
            failures++;
            printf("FAIL fe_sub random %d\n", i);
            show("a   ", a); show("b   ", b);
            show("got ", got); show("want", want);
            break;
        }
    }
    printf("fe_add/fe_sub vs reference: %d edge + %d random\n", 36, n);

    /* Aliasing for both; the ladder does fe_add(a, x2, z2) style calls with
       the destination overlapping a source constantly. */
    fill(a, 99); fill(b, 99);
    c68k_25519_fe_add_ref(want, a, b);
    c68k_25519_fe_add(a, a, b);
    ck("fe_add r==a aliasing", fe_eq(a, want));

    fill(a, 99); fill(b, 99);
    c68k_25519_fe_sub_ref(want, a, b);
    c68k_25519_fe_sub(b, a, b);
    ck("fe_sub r==b aliasing", fe_eq(b, want));

    /* The whole ladder, through a DH agreement. */
    for (i = 0; i < 32; i++) {
        ska[i] = (unsigned char)rnd();
        skb[i] = (unsigned char)rnd();
    }
    ck("x25519_base a", c68k_x25519_base(pka, ska) == 0);
    ck("x25519_base b", c68k_x25519_base(pkb, skb) == 0);
    ck("x25519 a*B",    c68k_x25519(s1, ska, pkb) == 0);
    ck("x25519 b*A",    c68k_x25519(s2, skb, pka) == 0);
    ck("DH agreement",  memcmp(s1, s2, 32) == 0);

    if (failures == 0)
        printf("PASS\n");
    else
        printf("FAILED %d\n", failures);
    return failures ? 20 : 0;
}
