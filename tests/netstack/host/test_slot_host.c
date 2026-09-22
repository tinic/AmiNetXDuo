/*
 * AmiNetXDuo, which interface slot a newcomer lands in.
 *
 * The vacant scan has to agree with nx_ip_interface_attach() -- first free,
 * in slot order -- and a slot is not free while a device, an attached
 * interface or a claim sits in it.  The yield pick is the other way round:
 * last slot first, only one the start-up pass took on its own, and never one
 * something holds a claim on.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_slot.h"

#include <stdio.h>
#include <string.h>

#include "aminetxduo/config.h"


static unsigned long h_checks;
static unsigned long h_failures;


static void h_check(int ok, const char *what)
{
    h_checks++;
    if (!ok)
    {
        h_failures++;
        printf("FAIL %s\n", what);
    }
}


/* A slot with an interface up in it, the way the tables look after a start. */
static void h_up(AmiNsSlot *slot, BOOL wanted)
{
    slot->attached = TRUE;
    slot->held     = TRUE;
    slot->wanted   = wanted;
    slot->claims   = 0;
}


static void h_case_vacant_is_first_free(void)
{
    AmiNsSlot table[AMI_CFG_MAX_ATTACHED];

    memset(table, 0, sizeof(table));
    h_check(ami_ns_slot_vacant(table, (UWORD)AMI_CFG_MAX_ATTACHED) == 0,
            "an empty table offers slot 0");

    h_up(&table[0], TRUE);
    h_check(ami_ns_slot_vacant(table, (UWORD)AMI_CFG_MAX_ATTACHED) == 1,
            "slot 0 taken, slot 1 is next");

    h_up(&table[1], FALSE);
    h_up(&table[2], FALSE);
    h_check(ami_ns_slot_vacant(table, (UWORD)AMI_CFG_MAX_ATTACHED) == 3,
            "three taken, the last one is offered");

    h_up(&table[3], FALSE);
    h_check(ami_ns_slot_vacant(table, (UWORD)AMI_CFG_MAX_ATTACHED) == -1,
            "a full table offers nothing");

    /* A hole in the middle is filled before the end: the same scan
       nx_ip_interface_attach() does. */
    memset(&table[1], 0, sizeof(table[1]));
    h_check(ami_ns_slot_vacant(table, (UWORD)AMI_CFG_MAX_ATTACHED) == 1,
            "a hole in the middle is taken first");

    h_check(ami_ns_slot_vacant(table, 1U) == -1,
            "the scan stops at the count it was given");
    h_check(ami_ns_slot_vacant(table, 0U) == -1, "zero slots, nothing");
    h_check(ami_ns_slot_vacant(NULL, (UWORD)AMI_CFG_MAX_ATTACHED) == -1,
            "no table, nothing");
}


static void h_case_any_one_of_three_things_makes_a_slot_taken(void)
{
    AmiNsSlot table[AMI_CFG_MAX_ATTACHED];

    /* An interface NetX Duo still has, with the device already gone: the
       state part way through a removal. */
    memset(table, 0, sizeof(table));
    table[0].attached = TRUE;
    h_check(ami_ns_slot_vacant(table, (UWORD)AMI_CFG_MAX_ATTACHED) == 1,
            "an attached interface with no device is not vacant");

    /* A device opened and not yet attached: the state between
       ami_sana2_open() and nx_ip_interface_attach(). */
    memset(table, 0, sizeof(table));
    table[0].held = TRUE;
    h_check(ami_ns_slot_vacant(table, (UWORD)AMI_CFG_MAX_ATTACHED) == 1,
            "a device with no interface is not vacant");

    /* A claim on an otherwise empty slot: a socket bound to a name whose
       interface is on its way back. */
    memset(table, 0, sizeof(table));
    table[0].claims = 1;
    h_check(ami_ns_slot_vacant(table, (UWORD)AMI_CFG_MAX_ATTACHED) == 1,
            "a claimed slot is not vacant");

    /* `wanted` alone says nothing about vacancy. */
    memset(table, 0, sizeof(table));
    table[0].wanted = TRUE;
    h_check(ami_ns_slot_vacant(table, (UWORD)AMI_CFG_MAX_ATTACHED) == 0,
            "wanted on an empty slot does not take it");
}


static void h_case_yield_is_last_unwanted_first(void)
{
    AmiNsSlot table[AMI_CFG_MAX_ATTACHED];
    UWORD     i;

    memset(table, 0, sizeof(table));
    h_check(ami_ns_slot_yield_candidate(table, (UWORD)AMI_CFG_MAX_ATTACHED)
                == -1,
            "an empty table has nothing to yield");

    /* Everything the boot found in the drawer: the highest slot goes. */
    for (i = 0; i < (UWORD)AMI_CFG_MAX_ATTACHED; i++)
        h_up(&table[i], FALSE);
    h_check(ami_ns_slot_yield_candidate(table, (UWORD)AMI_CFG_MAX_ATTACHED)
                == (LONG)AMI_CFG_MAX_ATTACHED - 1,
            "all unwanted, the last slot yields");

    /* The last one was named: the one below it goes instead. */
    table[AMI_CFG_MAX_ATTACHED - 1].wanted = TRUE;
    h_check(ami_ns_slot_yield_candidate(table, (UWORD)AMI_CFG_MAX_ATTACHED)
                == (LONG)AMI_CFG_MAX_ATTACHED - 2,
            "a named slot is skipped for the one below it");

    /* Everything named: nothing yields, however full. */
    for (i = 0; i < (UWORD)AMI_CFG_MAX_ATTACHED; i++)
        table[i].wanted = TRUE;
    h_check(ami_ns_slot_yield_candidate(table, (UWORD)AMI_CFG_MAX_ATTACHED)
                == -1,
            "all named, nothing yields");

    h_check(ami_ns_slot_yield_candidate(NULL, (UWORD)AMI_CFG_MAX_ATTACHED)
                == -1,
            "no table, nothing yields");
}


static void h_case_yield_needs_a_device_and_no_claim(void)
{
    AmiNsSlot table[AMI_CFG_MAX_ATTACHED];

    /* Slot 1 unwanted but claimed, slot 0 unwanted and free of claims. */
    memset(table, 0, sizeof(table));
    h_up(&table[0], FALSE);
    h_up(&table[1], FALSE);
    table[1].claims = 2;
    h_check(ami_ns_slot_yield_candidate(table, (UWORD)AMI_CFG_MAX_ATTACHED)
                == 0,
            "a claimed slot is passed over for a lower unclaimed one");

    table[0].claims = 1;
    h_check(ami_ns_slot_yield_candidate(table, (UWORD)AMI_CFG_MAX_ATTACHED)
                == -1,
            "every unwanted slot claimed, nothing yields");

    /* An attached interface with no device in the slot is not offered:
       there is nothing to close and give back. */
    memset(table, 0, sizeof(table));
    table[2].attached = TRUE;
    h_check(ami_ns_slot_yield_candidate(table, (UWORD)AMI_CFG_MAX_ATTACHED)
                == -1,
            "no device, nothing to yield");

    /* A device alone is enough: the yield closes it. */
    table[2].held = TRUE;
    h_check(ami_ns_slot_yield_candidate(table, (UWORD)AMI_CFG_MAX_ATTACHED)
                == 2,
            "a held unwanted slot yields");

    h_check(ami_ns_slot_yield_candidate(table, 2U) == -1,
            "the pick stops at the count it was given");
}


/* The two together: what ami_ns_interface_add_locked() does when a NAMED
   interface arrives and the table is full. */
static void h_case_full_table_named_newcomer(void)
{
    AmiNsSlot table[AMI_CFG_MAX_ATTACHED];
    LONG      victim;
    UWORD     i;

    for (i = 0; i < (UWORD)AMI_CFG_MAX_ATTACHED; i++)
        h_up(&table[i], i == 0);

    h_check(ami_ns_slot_vacant(table, (UWORD)AMI_CFG_MAX_ATTACHED) == -1,
            "full: no vacant slot");

    victim = ami_ns_slot_yield_candidate(table, (UWORD)AMI_CFG_MAX_ATTACHED);
    h_check(victim == (LONG)AMI_CFG_MAX_ATTACHED - 1,
            "full: the last boot-found slot is the one to yield");

    /* After the removal the slot is empty, and the vacant scan finds it. */
    memset(&table[victim], 0, sizeof(table[victim]));
    h_check(ami_ns_slot_vacant(table, (UWORD)AMI_CFG_MAX_ATTACHED) == victim,
            "the yielded slot is the vacant one");
}


int main(void)
{
    h_case_vacant_is_first_free();
    h_case_any_one_of_three_things_makes_a_slot_taken();
    h_case_yield_is_last_unwanted_first();
    h_case_yield_needs_a_device_and_no_claim();
    h_case_full_table_named_newcomer();

    printf("%lu checks, %lu failures\n", h_checks, h_failures);

    return (h_failures == 0) ? 0 : 1;
}
