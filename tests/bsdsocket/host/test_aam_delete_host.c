/*
 * F-029: DeleteAddrAllocMessage() on a message a worker still owns.  The
 * worker reads the message, writes the lease and the result into it and
 * replies it; a delete between BeginInterfaceConfig() and that reply freed it
 * under the worker.  Now the job is orphaned and the worker frees the message
 * instead of replying it.  A delete with no worker, and a second delete, are
 * as before.
 *
 * addralloc.c is #included.  The worker runs on this thread, driven through
 * the stubs below: a DHCP state that a test can use to delete the message
 * mid-flight, and an allocator that poisons what it frees so a write after
 * the free shows.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long h_checks;
static unsigned long h_failures;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        h_checks++;                                                           \
        if (!(cond)) {                                                        \
            h_failures++;                                                     \
            printf("  FAIL %s\n", (what));                                    \
        }                                                                     \
    } while (0)

#define H_POISON    0xA5

static struct Process    h_proc;
static struct Message   *h_posted;
static int               h_replies;
static int               h_replied_freed;
static int               h_polls;
static int               h_delete_at_poll;
static int               h_delete_twice;
static struct AddressAllocationMessage *h_aam;
static ULONG             h_aam_size;
static int               h_aam_frees;
static int               h_forbid;

/* ---- exec and dos ---- */

VOID Forbid(VOID) { h_forbid++; }
VOID Permit(VOID) { h_forbid--; }
struct Task *FindTask(const char *name) { (void)name; return &h_proc.pr_Task; }
VOID PutMsg(struct MsgPort *port, struct Message *m) { (void)port; h_posted = m; }
struct MsgPort *WaitPort(struct MsgPort *port) { return port; }
struct Message *GetMsg(struct MsgPort *port)
{
    struct Message *m = h_posted;

    (void)port;
    h_posted = NULL;
    return m;
}
LONG Delay(ULONG ticks) { (void)ticks; return 0; }
struct Process *CreateNewProc(const struct TagItem *tags) { (void)tags; return &h_proc; }

static int h_is_freed(const void *p, ULONG size)
{
    const unsigned char *b = (const unsigned char *)p;
    ULONG i;

    for (i = 0; i < size; i++)
        if (b[i] != H_POISON)
            return 0;
    return 1;
}

VOID ReplyMsg(struct Message *m)
{
    h_replies++;
    if ((void *)m == (void *)h_aam && h_aam_frees > 0)
        h_replied_freed++;
}

/* ---- the allocator: a free is poisoned and kept, never reused ---- */

static ULONG h_alloc_size;
static unsigned h_alloc_calls;
static BOOL h_alloc_refuse;

static APTR h_allocate(ULONG size)
{
    UBYTE *p;

    h_alloc_calls++;
    h_alloc_size = size;
    if (h_alloc_refuse || size > 4096)
        return NULL; /* Never make giant host allocations in overflow tests. */
    p = calloc(1, (size_t)size + 8);
    if (p != NULL)
        memset(p + size, H_POISON, 8);
    return p;
}

APTR ami_alloc_tagged(ULONG size, const char *site)
{
    (void)site;
    return h_allocate(size);
}

#ifndef ami_alloc
APTR ami_alloc(ULONG size) { return h_allocate(size); }
#endif

VOID ami_free(APTR p)
{
    if (p == NULL)
        return;
    if (p == (APTR)h_aam)
    {
        h_aam_frees++;
        memset(p, H_POISON, h_aam_size);
        return;                         /* kept, so a late write shows */
    }
    free(p);
}

/* ---- the netstack ---- */

LONG netstack_interface_dhcp_start_lease(UWORD index, ULONG addr, ULONG lease)
{ (void)index; (void)addr; (void)lease; return AMI_NET_OK; }

LONG netstack_interface_dhcp_state(UWORD index)
{
    (void)index;
    h_polls++;
    if (h_polls == h_delete_at_poll)
    {
        bsd_DeleteAddrAllocMessage(h_aam, NULL);
        if (h_delete_twice)
            bsd_DeleteAddrAllocMessage(h_aam, NULL);
    }
    return (h_polls >= 3) ? AMI_DHCP_BOUND : AMI_DHCP_WORKING;
}

LONG netstack_interface_dhcp_lease(UWORD index, AmiDhcpLease *out)
{
    (void)index;
    memset(out, 0, sizeof(*out));
    return AMI_NET_OK;
}

LONG netstack_interface_dhcp_stop(UWORD index, BOOL release)
{ (void)index; (void)release; return AMI_NET_OK; }

VOID netstack_interface_release(UWORD index) { (void)index; }

/* bsd_aam_store_lease()'s helpers. */
VOID bsd_strncpy(char *dst, const char *src, ULONG size)
{
    if (size == 0)
        return;
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

VOID DateStamp(struct DateStamp *ds) { (void)ds; }
VOID bsd_stack_transient_release(struct AmiSocketBase *base) { (void)base; }

#include "addralloc.c"

/* Creator-only collaborators; the lifecycle tests above retain their stubs. */
ULONG bsd_strlen(const char *s) { return (ULONG)strlen(s); }
VOID bsd_bzero(APTR p, ULONG size) { memset(p, 0, size); }
LONG bsd_if_index_of(NX_IP *ip, const char *name)
{ return ip != NULL && strcmp(name, "eth0") == 0 ? 0 : -1; }
struct TagItem *bsd_next_tag(struct TagItem **cursor)
{
    while (*cursor != NULL)
    {
        struct TagItem *item = *cursor;
        *cursor = item + 1;
        switch (item->ti_Tag)
        {
            case TAG_DONE: *cursor = NULL; return NULL;
            case TAG_IGNORE: break;
            case TAG_MORE: *cursor = (struct TagItem *)item->ti_Data; break;
            case TAG_SKIP: *cursor += item->ti_Data; break;
            default: return item;
        }
    }
    return NULL;
}

static LONG h_create_buffers(const LONG counts[7], struct AddressAllocationMessage **out)
{
    static NX_IP ip;
    struct AmiSocketBase base;
    struct TagItem tags[17];
    static const ULONG size_tags[7] =
    {
        CAAMTA_NAKMessageSize, CAAMTA_RouterTableSize, CAAMTA_DNSTableSize,
        CAAMTA_StaticRouteTableSize, CAAMTA_HostNameSize, CAAMTA_DomainNameSize,
        CAAMTA_BOOTPMessageSize
    };
    unsigned i;

    memset(&base, 0, sizeof(base));
    base.sb_StackRefs = 1;
    base.sb_StackIp = &ip;
    for (i = 0; i < 7; i++)
    {
        tags[i].ti_Tag = size_tags[i];
        tags[i].ti_Data = (ULONG)counts[i];
    }
    tags[0].ti_Data = 13; /* overwritten by the final NAK size tag */
    tags[7] = (struct TagItem){ CAAMTA_RecordLeaseExpiration, 1 };
    tags[8] = (struct TagItem){ CAAMTA_ClientIdentifier, (uintptr_t)"client42" };
    tags[9] = (struct TagItem){ CAAMTA_Timeout, 5 };
    tags[10] = (struct TagItem){ CAAMTA_LeaseTime, 123 };
    tags[11] = (struct TagItem){ CAAMTA_RequestedAddress, 0x11223344UL };
    tags[12] = (struct TagItem){ CAAMTA_ReplyPort, (uintptr_t)&h_proc.pr_MsgPort };
    tags[13] = (struct TagItem){ CAAMTA_RequestUnicast, 1 };
    tags[14] = (struct TagItem){ CAAMTA_NAKMessageSize + 0x10000UL, 999 };
    /* Last assignment wins even when the same size is assigned twice. */
    tags[15] = (struct TagItem){ CAAMTA_NAKMessageSize, (ULONG)counts[0] };
    tags[16] = (struct TagItem){ TAG_DONE, 0 };
    return bsd_CreateAddrAllocMessageA(AAM_VERSION, AAMP_DHCP, (STRPTR)"eth0",
                                       out, tags, &base);
}

static ULONG h_round4(ULONG n) { return (n + 3) & ~3UL; }

static void t_create_buffer_layout(void)
{
    const LONG seed[7] = { 1, 2, 3, 4, 5, 6, 7 };
    const LONG samples[] = { 0, -1, (-2147483647 - 1), 1, 5 };
    const ULONG unit[7] = { 1, 4, 4, 4, 1, 1, 1 };
    unsigned field, sample;

    for (field = 0; field < 7; field++)
        for (sample = 0; sample < sizeof(samples) / sizeof(samples[0]); sample++)
        {
            LONG counts[7], actual_sizes[7];
            APTR actual[7];
            struct AddressAllocationMessage *aam = NULL;
            UBYTE *next;
            unsigned i;

            memcpy(counts, seed, sizeof(counts));
            counts[field] = samples[sample];
            h_alloc_calls = 0;
            CHECK(h_create_buffers(counts, &aam) == CAAME_Success && aam != NULL,
                  "buffer creator accepts nonpositive sizes as absent");
            if (aam == NULL)
                continue;
            actual[0] = aam->aam_NAKMessage; actual_sizes[0] = aam->aam_NAKMessageSize;
            actual[1] = aam->aam_RouterTable; actual_sizes[1] = aam->aam_RouterTableSize;
            actual[2] = aam->aam_DNSTable; actual_sizes[2] = aam->aam_DNSTableSize;
            actual[3] = aam->aam_StaticRouteTable; actual_sizes[3] = aam->aam_StaticRouteTableSize;
            actual[4] = aam->aam_HostName; actual_sizes[4] = aam->aam_HostNameSize;
            actual[5] = aam->aam_DomainName; actual_sizes[5] = aam->aam_DomainNameSize;
            actual[6] = aam->aam_BOOTPMessage; actual_sizes[6] = aam->aam_BOOTPMessageSize;
            next = (UBYTE *)aam + h_round4(sizeof(*aam));
            for (i = 0; i < 7; i++)
            {
                LONG count = counts[i] > 0 ? counts[i] : 0;
                CHECK(actual_sizes[i] == count && actual[i] == (count ? (APTR)next : NULL),
                      "each public buffer gets exact size, order and aligned pointer");
                if (count)
                {
                    ULONG j, bytes = h_round4((ULONG)count * unit[i]);
                    CHECK(((uintptr_t)actual[i] & 3) == 0, "buffer starts on longword boundary");
                    for (j = 0; j < bytes; j++)
                        CHECK(next[j] == 0, "buffer and alignment padding zeroed");
                    next += bytes;
                }
            }
            CHECK(aam->aam_LeaseExpires == (struct DateStamp *)next,
                  "lease expiration follows all size buffers");
            next += h_round4(sizeof(struct DateStamp));
            CHECK(aam->aam_ClientIdentifier == next &&
                  strcmp((const char *)aam->aam_ClientIdentifier, "client42") == 0,
                  "client identifier follows lease with original NUL-terminated text");
            next += h_round4(sizeof("client42"));
            CHECK(h_alloc_calls == 1 && h_alloc_size == (ULONG)(next - (UBYTE *)aam),
                  "creator allocates exactly its padded output size");
            CHECK(h_is_freed(next, 8), "creator does not overrun final allocation canary");
            CHECK(aam->aam_Message.mn_ReplyPort == &h_proc.pr_MsgPort &&
                  aam->aam_Timeout == AAM_TIMEOUT_MIN && aam->aam_LeaseTime == 123 &&
                  aam->aam_RequestedAddress == 0x11223344UL && aam->aam_Unicast &&
                  aam->aam_Reserved == BSD_AAM_COOKIE && aam->aam_Result == AAMR_Ignored &&
                  strcmp(aam->aam_InterfaceName, "eth0") == 0,
                  "scalar and reply metadata retained beside buffer descriptors");
            free(aam);
        }
}

static void t_create_overflow(void)
{
    LONG counts[7] = { 0, 0, 0, 0, 0, 0, 0 };
    struct AddressAllocationMessage *out;
    unsigned i;

    for (i = 1; i <= 3; i++)
    {
        counts[i] = 0x7FFFFFFF;
        out = (struct AddressAllocationMessage *)(uintptr_t)1;
        h_alloc_calls = 0;
        CHECK(h_create_buffers(counts, &out) == CAAME_Not_enough_memory &&
              out == NULL && h_alloc_calls == 0, "table multiplication overflow refused before allocation");
        counts[i] = 0;
    }
    counts[0] = counts[4] = 0x7FFFFFFF;
    out = (struct AddressAllocationMessage *)(uintptr_t)1;
    h_alloc_calls = 0;
    CHECK(h_create_buffers(counts, &out) == CAAME_Not_enough_memory &&
          out == NULL && h_alloc_calls == 0, "aggregate byte-buffer overflow refused before allocation");
    counts[0] = counts[4] = 0;
    out = (struct AddressAllocationMessage *)(uintptr_t)1;
    h_alloc_calls = 0;
    h_alloc_refuse = TRUE;
    CHECK(h_create_buffers(counts, &out) == CAAME_Not_enough_memory &&
          out == NULL && h_alloc_calls == 1, "allocator refusal leaves result NULL");
    h_alloc_refuse = FALSE;
}

/* A message as CreateAddrAllocMessageA() leaves it, and a job running it. */
static void h_start(void)
{
    /* ami_free() keeps the poisoned message alive for post-delete checks.
       The previous scenario is finished now; release its backing allocation. */
    free(h_aam);
    h_aam = NULL;
    h_aam_size = (ULONG)sizeof(*h_aam);
    h_aam = (struct AddressAllocationMessage *)calloc(1, h_aam_size);
    h_aam->aam_Reserved = BSD_AAM_COOKIE;
    h_aam->aam_Version  = AAM_VERSION;
    h_aam->aam_Timeout  = AAM_TIMEOUT_MIN;
    h_aam->aam_Message.mn_ReplyPort = &h_proc.pr_MsgPort;
    h_aam_frees = 0;
    h_replies = 0;
    h_replied_freed = 0;
    h_polls = 0;
    h_delete_at_poll = 0;
    h_delete_twice = 0;
    memset(bsd_aam_jobs, 0, sizeof(bsd_aam_jobs));
    bsd_aam_workers = 0;
    h_proc.pr_Task.tc_Node.ln_Type = NT_PROCESS;

    bsd_aam_launch(h_aam, 0, NULL);
}

int main(void)
{
    t_create_buffer_layout();
    t_create_overflow();
    /* Deleted by the caller while the worker is polling: not freed under it,
       not replied, freed once by the worker, nothing written after. */
    h_start();
    h_delete_at_poll = 1;
    bsd_aam_worker();
    CHECK(h_aam_frees == 1, "an in-flight message is freed exactly once");
    CHECK(h_replies == 0 && h_replied_freed == 0,
          "and never replied once deleted");
    CHECK(h_is_freed(h_aam, h_aam_size),
          "and nothing wrote into it after the free");
    CHECK(h_polls == 1, "the worker stopped before polling again");
    CHECK(bsd_aam_workers == 0 && bsd_aam_jobs[0] == NULL,
          "and left no job behind");

    /* Deleted twice in flight: the second finds no cookie, as before. */
    h_start();
    h_delete_at_poll = 1;
    h_delete_twice = 1;
    bsd_aam_worker();
    CHECK(h_aam_frees == 1, "a double delete in flight frees once");

    /* No delete: replied, not freed, as before. */
    h_start();
    bsd_aam_worker();
    CHECK(h_replies == 1 && h_aam_frees == 0, "a kept message is replied");
    CHECK(h_aam->aam_Result == AAMR_Success, "with its result");

    /* Deleted after the reply: freed at once, and a second delete is
       harmless. */
    bsd_DeleteAddrAllocMessage(h_aam, NULL);
    CHECK(h_aam_frees == 1, "a replied message is freed by the delete");
    bsd_DeleteAddrAllocMessage(h_aam, NULL);
    CHECK(h_aam_frees == 1, "and a second delete finds no cookie");

    /* The worker exits inside Forbid(), once per run; nothing else leaks. */
    CHECK(h_forbid == 3, "Forbid balanced but for the three workers' exits");

    printf("aam_delete: %lu checks, %lu failures\n", h_checks, h_failures);
    free(h_aam);
    return (h_failures != 0) ? 1 : 0;
}
