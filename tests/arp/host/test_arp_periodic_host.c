/*
 * Audit N-007: _nx_arp_periodic_update must visit every active dynamic ARP
 * entry exactly once per pass.
 *
 * expiry:  N active entries, the one at pool position x expires during the
 *          pass (retries == NX_ARP_MAXIMUM_RETRIES).  The expiry re-links it
 *          at the pool tail; every other entry must still lose exactly one
 *          tick.  Run for every x, with a partly used and a full pool, and
 *          for every pair of expiries in one pass.
 * deleted: the entry in the middle of three is removed with
 *          _nx_arp_entry_delete (the `arp -d` path, NETCTRL_ARP_DELETE).  The
 *          pass must not touch it or ARP for its address, and must still
 *          reach the entry behind it.
 *
 * Linked for real: _nx_arp_enable, _nx_arp_entry_allocate,
 * _nx_arp_dynamic_entry_delete, _nx_arp_entry_delete and
 * _nx_arp_periodic_update.  Stubbed: ThreadX, the ARP request send (records
 * the address) and the packet release (counts).
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_arp.h"

#include <stdio.h>
#include <string.h>

#define POOL    19u     /* AMI_ARP_CACHE_SIZE / sizeof(NX_ARP) on the target */
#define TICKS   3u      /* start value; one pass must leave TICKS - 1 */

UINT _tx_thread_interrupt_disable(VOID)            { return 0; }
VOID _tx_thread_interrupt_restore(UINT p)          { (void) p; }
UINT _tx_mutex_get(TX_MUTEX *m, ULONG w)           { (void) m; (void) w; return TX_SUCCESS; }
UINT _tx_mutex_put(TX_MUTEX *m)                    { (void) m; return TX_SUCCESS; }
UINT _txe_mutex_get(TX_MUTEX *m, ULONG w)          { (void) m; (void) w; return TX_SUCCESS; }
UINT _txe_mutex_put(TX_MUTEX *m)                   { (void) m; return TX_SUCCESS; }

static ULONG sent[64];
static UINT  sent_count;
static UINT  released;

VOID _nx_arp_packet_send(NX_IP *ip_ptr, ULONG destination_ip, NX_INTERFACE *nx_interface)
{
    (void) ip_ptr;
    (void) nx_interface;
    if (sent_count < sizeof(sent) / sizeof(sent[0]))
        sent[sent_count++] = destination_ip;
}

VOID _nx_packet_transmit_release(NX_PACKET *packet_ptr)
{
    (void) packet_ptr;
    released++;
}

/* Unused by the arms, referenced by _nx_arp_enable and _nx_arp_entry_delete.  */
VOID _nx_arp_queue_process(NX_IP *ip_ptr)          { (void) ip_ptr; }

UINT _nx_arp_static_entry_delete(NX_IP *ip_ptr, ULONG ip_address,
                                 ULONG physical_msw, ULONG physical_lsw)
{
    (void) ip_ptr; (void) ip_address; (void) physical_msw; (void) physical_lsw;
    printf("FAIL static delete reached\n");
    return NX_NOT_SUCCESSFUL;
}

static NX_IP     ip;
static NX_ARP    cache[POOL];
static NX_ARP   *entry[POOL];       /* in pool order, head first */

static void setup(UINT n)
{
    UINT i;

    memset(&ip, 0, sizeof(ip));
    sent_count = 0;
    released   = 0;
    _nx_arp_enable(&ip, cache, (ULONG) sizeof(cache));

    /* Allocation puts each new entry at the pool head, so allocate in
       reverse to leave entry[0] at the head.  */
    for (i = n; i-- > 0;)
    {
        ULONG   addr  = 0x0A000001UL + i;
        UINT    index = (UINT) ((addr + (addr >> 8)) & NX_ARP_TABLE_MASK);
        NX_ARP *a;

        if (_nx_arp_entry_allocate(&ip, &ip.nx_ip_arp_table[index], NX_FALSE) != NX_SUCCESS)
        {
            printf("FAIL allocate %u\n", i);
            return;
        }
        a = ip.nx_ip_arp_dynamic_list;
        a -> nx_arp_ip_address        = addr;
        a -> nx_arp_entry_next_update = TICKS;
        a -> nx_arp_retries           = 0;
        entry[i] = a;
    }
}

/* The pool must still be one circle of POOL entries.  */
static int pool_ok(void)
{
    NX_ARP *a = ip.nx_ip_arp_dynamic_list;
    UINT    n = 0;

    do
    {
        if (a -> nx_arp_pool_next -> nx_arp_pool_previous != a)
            return 0;
        a = a -> nx_arp_pool_next;
        n++;
    } while (a != ip.nx_ip_arp_dynamic_list && n <= POOL);
    return n == POOL;
}

/* Expire the entries whose positions are set in `mask`, run one pass.  */
static int expire_case(UINT n, ULONG mask)
{
    static NX_PACKET queued[POOL];
    char line[512];
    int  len = 0;
    UINT i, expiring = 0;
    int  fail = 0;

    setup(n);
    for (i = 0; i < n; i++)
    {
        if (mask & (1UL << i))
        {
            entry[i] -> nx_arp_entry_next_update = 1;
            entry[i] -> nx_arp_retries           = NX_ARP_MAXIMUM_RETRIES;
            queued[i].nx_packet_queue_next       = NX_NULL;
            entry[i] -> nx_arp_packets_waiting   = &queued[i];
            expiring++;
        }
    }

    _nx_arp_periodic_update(&ip);

    for (i = 0; i < n; i++)
    {
        ULONG ticks;

        if (mask & (1UL << i))
        {
            if (entry[i] -> nx_arp_active_list_head != NX_NULL)
            {
                fail = 1;
                len += snprintf(line + len, sizeof(line) - (size_t) len,
                                " pos%u:still-active", i);
            }
            continue;
        }
        ticks = TICKS - entry[i] -> nx_arp_entry_next_update;
        if (ticks != 1)
        {
            fail = 1;
            len += snprintf(line + len, sizeof(line) - (size_t) len,
                            " pos%u:%lux", i, (unsigned long) ticks);
        }
    }
    if (ip.nx_ip_arp_dynamic_active_count != n - expiring
        || released != expiring || sent_count != 0 || !pool_ok())
    {
        fail = 1;
        len += snprintf(line + len, sizeof(line) - (size_t) len,
                        " count=%lu released=%u sent=%u pool=%d",
                        (unsigned long) ip.nx_ip_arp_dynamic_active_count,
                        released, sent_count, pool_ok());
    }
    if (fail)
        printf("FAIL n=%u expire mask 0x%05lx:%s\n", n, (unsigned long) mask, line);
    return fail;
}

static int arm_expiry(void)
{
    static const UINT sizes[] = { 8u, POOL };
    UINT s, n, x, y, pairs = 0;
    int  bad = 0, fail;

    for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
    {
        n = sizes[s];
        for (x = 0; x < n; x++)
        {
            fail = expire_case(n, 1UL << x);
            if (!fail)
                printf("ok   n=%u expire pos%u: every other entry decremented once\n", n, x);
            bad |= fail;
        }
    }

    /* Two expiries in one pass, including the pool head.  */
    for (x = 0; x < 8u; x++)
    {
        for (y = x + 1; y < 8u; y++)
        {
            bad |= fail = expire_case(8u, (1UL << x) | (1UL << y));
            pairs += !fail;
        }
    }
    printf("%s n=8 two expiries: %u of 28 pairs correct\n", pairs == 28 ? "ok  " : "FAIL", pairs);
    return bad;
}

static int arm_deleted(void)
{
    ULONG gone;
    UINT  i;
    int   bad = 0;

    setup(3);
    entry[1] -> nx_arp_entry_next_update = 1;   /* due this pass */
    gone = entry[1] -> nx_arp_ip_address;

    if (_nx_arp_entry_delete(&ip, gone) != NX_SUCCESS)
    {
        printf("FAIL _nx_arp_entry_delete\n");
        return 1;
    }

    _nx_arp_periodic_update(&ip);

    for (i = 0; i < 3; i += 2)
    {
        ULONG ticks = TICKS - entry[i] -> nx_arp_entry_next_update;

        printf("%s live pos%u decremented %lux (want 1)\n",
               ticks == 1 ? "ok  " : "FAIL", i, (unsigned long) ticks);
        bad |= ticks != 1;
    }
    printf("%s deleted pos1 next_update %lu (want 1, untouched), retries %u (want 0)\n",
           (entry[1] -> nx_arp_entry_next_update == 1 && entry[1] -> nx_arp_retries == 0)
               ? "ok  " : "FAIL",
           (unsigned long) entry[1] -> nx_arp_entry_next_update,
           (unsigned) entry[1] -> nx_arp_retries);
    bad |= entry[1] -> nx_arp_entry_next_update != 1 || entry[1] -> nx_arp_retries != 0;
    for (i = 0; i < sent_count; i++)
    {
        if (sent[i] == gone)
        {
            printf("FAIL ARP request sent for the deleted address 0x%08lx\n",
                   (unsigned long) gone);
            bad = 1;
        }
    }
    if (!pool_ok())
    {
        printf("FAIL pool circle broken\n");
        bad = 1;
    }
    return bad;
}

int main(int argc, char **argv)
{
    int bad;

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s expiry|deleted\n", argv[0]);
        return 2;
    }
    if (sizeof(cache) / sizeof(cache[0]) != POOL)
        return 2;
    if (strcmp(argv[1], "expiry") == 0)
        bad = arm_expiry();
    else if (strcmp(argv[1], "deleted") == 0)
        bad = arm_deleted();
    else
        return 2;
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad;
}
