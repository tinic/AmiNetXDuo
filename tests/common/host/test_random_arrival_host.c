/*
 * The frame-arrival entropy source must STOP.
 *
 * Credit was once the only exit: arrival_done latched when the source had
 * credited AMI_RANDOM_ARRIVAL_MAX_BITS, and credit only grows when the low
 * bits of the inter-arrival delta move across a batch.  A perfectly regular
 * cadence credits nothing, so the gate stayed open for the life of the machine
 * and every sixteenth received frame paid a SHA-256 compression.
 *
 * What has to be true of the bound is BOTH halves, and the second is the one
 * that makes it safe: it must stop a barren source, and it must NOT truncate
 * one that is producing anything at all.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "aminetxduo/random.h"

static int checks;
static int failures;

static void ck(const char *what, int ok)
{
    checks++;
    if (!ok)
    {
        failures++;
        printf("FAIL %s\n", what);
    }
}

/* How many batches a source crediting `per` bits each runs before it stops.
   `every` is the batch period at which it credits: 1 credits every batch, 16
   credits one batch in sixteen and is barren in between. */
static unsigned long batches_until_stop_every(unsigned long per,
                                              unsigned long every)
{
    unsigned long bits = 0, n = 0, barren = 0;

    for (;;)
    {
        if (ami_random_arrival_stop(bits, n, 0, barren))
            return n;

        {
            unsigned long credit = ((n % every) == 0UL) ? per : 0UL;

            n++;
            bits += credit;
            if (bits > AMI_RANDOM_ARRIVAL_MAX_BITS)
                bits = AMI_RANDOM_ARRIVAL_MAX_BITS;
            if (credit == 0UL)
                barren++;
            else
                barren = 0UL;
        }

        if (n > 100000UL)
            return n;           /* never stops: the defect this guards */
    }
}

static unsigned long batches_until_stop(unsigned long per)
{
    return batches_until_stop_every(per, 1UL);
}

int main(void)
{
    unsigned long barren, one, healthy, sparse;

    printf("random: the arrival source has to stop\n");

    /* A BARREN SOURCE STOPS.  Zero credit for ever was the defect; the bound
       is the only thing that ends it. */
    barren = batches_until_stop(0);
    ck("a source crediting nothing stops",
       barren == AMI_RANDOM_ARRIVAL_MAX_BARREN);
    ck("and it is the barren rule that stops it, not the backstop",
       AMI_RANDOM_ARRIVAL_MAX_BARREN < AMI_RANDOM_ARRIVAL_MAX_BATCHES);
    printf("  barren      %lu batches (%lu frames)\n",
           barren, barren * 16UL);

    /* AND THE BOUND MUST NOT BE WHAT STOPS A WORKING ONE.  One bit a batch is
       the slowest source that is still a source; it must reach its credit
       before the batch cap, or the cap is silently costing entropy. */
    one = batches_until_stop(1);
    ck("one bit a batch stops on CREDIT, not on the bound",
       one == AMI_RANDOM_ARRIVAL_MAX_BITS &&
       one < AMI_RANDOM_ARRIVAL_MAX_BATCHES);
    printf("  1 bit/batch %lu batches (credit %lu, bound %lu)\n",
           one, (unsigned long)AMI_RANDOM_ARRIVAL_MAX_BITS,
           (unsigned long)AMI_RANDOM_ARRIVAL_MAX_BATCHES);

    healthy = batches_until_stop(8);
    ck("a healthy source stops in eight batches", healthy == 8UL);
    printf("  8 bits/batch %lu batches\n", healthy);

    /* The pool reaching its own bar ends the sampling whatever the arrivals
       have done, and that path must not be disturbed by the new one. */
    ck("a seeded pool stops it immediately",
       ami_random_arrival_stop(0, 0, AMI_RANDOM_MIN_BITS, 0) != 0);
    ck("an unseeded pool at zero batches does not",
       ami_random_arrival_stop(0, 0, AMI_RANDOM_MIN_BITS - 1UL, 0) == 0);

    /* The bound has to leave room for the slowest real source, with margin.
       If someone lowers it below the credit ceiling this fires. */
    ck("the bound is clear of the credit ceiling",
       AMI_RANDOM_ARRIVAL_MAX_BATCHES > AMI_RANDOM_ARRIVAL_MAX_BITS);

    /*
     * THE BARREN RULE IS CONSECUTIVE, AND THIS IS THE CHECK THAT SAYS SO.
     * A source crediting one bit every EIGHTH batch is barren seven batches
     * out of eight and must still reach its ceiling: with a cumulative count
     * it would be cut off almost at once.
     */
    sparse = batches_until_stop_every(1UL, 8UL);
    ck("one bit every eighth batch is not cut off by the barren rule",
       sparse > AMI_RANDOM_ARRIVAL_MAX_BARREN);
    printf("  1 bit/8 batches %lu batches\n", sparse);

    /*
     * AND IT RUNS TO THE BACKSTOP, NOT TO ITS CREDIT, WHICH IS A PRE-EXISTING
     * PROPERTY AND NOT THE BARREN RULE'S DOING.  Reaching 64 bits at one bit
     * per eight batches needs 512 batches and the backstop is 128, so this
     * source is truncated by the bound that was already there.  The check is
     * here so the next reader does not blame the wrong rule: raising
     * MAX_BATCHES is what would change it, and the campaign's reason for 128
     * is the cost of asking, not the credit.
     */
    ck("and the backstop, not the barren rule, is what ends it",
       sparse == AMI_RANDOM_ARRIVAL_MAX_BATCHES);

    /* And a source that goes quiet for longer than the rule allows stops. */
    ck("a gap longer than the rule stops it",
       ami_random_arrival_stop(1UL, 40UL, 0UL,
                               AMI_RANDOM_ARRIVAL_MAX_BARREN) != 0);
    ck("a gap one short of it does not",
       ami_random_arrival_stop(1UL, 40UL, 0UL,
                               AMI_RANDOM_ARRIVAL_MAX_BARREN - 1UL) == 0);

    printf("\n%d checks, %d failure(s)\n", checks, failures);

    return failures == 0 ? 1 - 1 : 1;
}
