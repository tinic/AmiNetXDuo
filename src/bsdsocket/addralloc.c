/*
 * bsdsocket.library, the address allocation message.
 *
 *   CreateAddrAllocMessageA()   build one, with its buffers
 *   DeleteAddrAllocMessage()    free it
 *
 * Constructor and destructor for the struct AddressAllocationMessage that
 * BeginInterfaceConfig() takes. They do not touch the network: they validate,
 * allocate and fill in defaults.
 *
 * Same primary source as interfaces.c: NDK 3.2's
 * SANA+RoadshowTCP-IP/doc/bsdsocket.doc, plus libraries/bsdsocket.h from the
 * same NDK, used as an ABI reference only. No Roadshow, AmiTCP, AROSTCP or
 * Miami code was consulted or is present.
 *
 * The autodoc enumerates ten distinct error codes for this one call and says
 * which condition produces each, so each one is returned as documented. A
 * caller that gets CAAME_Client_identifier_too_short can fix its input, and
 * one that gets a generic failure cannot.
 *
 * Every buffer the tags ask for is carved out of a single block that the
 * message sits at the top of, so DeleteAddrAllocMessage() is one free of the
 * pointer it was handed. A separate allocation per buffer needs a bookkeeping
 * structure that the published message has no room for.
 *
 * "This routine can only deallocate address allocation messages created by
 * CreateAddrAllocMessageA() and will not work with anything else." To tell
 * the difference, the message carries a cookie in aam_Reserved. That field is
 * reserved from the application's side, not from the library's. A message the
 * caller filled in by hand, which the autodoc permits, does not have the
 * cookie and is refused rather than freed.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "aminetxduo/config.h"

#include "interfaces.h"

#include <proto/dos.h>
#include <proto/exec.h>

#include <dos/dosextens.h>

#include <dos/dostags.h>

/*
 * The cookie. Arbitrary, and unlikely in a hand-filled message: a caller that
 * memsets its message to zero, which the autodoc tells it to do, leaves
 * aam_Reserved at zero.
 */
#define BSD_AAM_COOKIE      0x414D4147L     /* 'AMAG' */

/* "cannot be longer than 255 characters ... names shorter than 2 characters
   will be ignored", both bounds are the autodoc's. */
#define BSD_AAM_CID_MIN     2
#define BSD_AAM_CID_MAX     255

/* Every size the caller can ask for, gathered before anything is allocated. */
typedef struct BsdAamWanted
{
    LONG    baw_Timeout;
    ULONG   baw_LeaseTime;
    ULONG   baw_RequestedAddress;
    STRPTR  baw_ClientId;
    LONG    baw_NAKMessage;
    LONG    baw_RouterTable;
    LONG    baw_DNSTable;
    LONG    baw_StaticRouteTable;
    LONG    baw_HostName;
    LONG    baw_DomainName;
    LONG    baw_BOOTPMessage;
    BOOL    baw_LeaseExpires;
    BOOL    baw_Unicast;
    struct MsgPort *baw_ReplyPort;
} BsdAamWanted;

/* Sizes are byte counts and table sizes are entry counts. A negative value is
   a bad argument, not a small request. */
static LONG bsd_aam_size(const struct TagItem *item)
{
    LONG value = (LONG)item->ti_Data;

    return (value > 0) ? value : 0;
}

/*
 * Everything in the block after the message itself is longword-aligned: two
 * of the buffers are arrays of ULONG and m68k faults on a misaligned one.
 * ami_alloc() returns memory aligned enough for anything, so the carving is
 * the only place alignment can be lost.
 */
static ULONG bsd_aam_round(ULONG size)
{
    return (size + 3UL) & ~3UL;
}

/*
 * Add one region to the running total. Returns FALSE if the sum does not fit.
 *
 * Each of the seven size tags is a caller-supplied LONG that only has to be
 * positive. Two of them at 0x7FFFFFFC sum to 0xFFFFFFF8, and the total wraps
 * to less than the message itself needs. The allocation then succeeds at the
 * wrapped size, and the struct's last fields are written past the end of it.
 * Every carved pointer handed back to the caller points outside the block, and
 * bsd_aam_store_lease() later copies a DHCP-supplied host name into one of
 * them. On a machine with no MMU that silently corrupts whatever the allocator
 * put next.
 *
 * For the three table tags `count` is a number of ULONGs rather than a byte
 * size, and that multiply can wrap on its own, so it is checked separately.
 *
 * An over-large request returns CAAME_Not_enough_memory, the same answer the
 * caller gets from arithmetic in a width wide enough to hold it.
 */
static BOOL bsd_aam_add(ULONG *total, ULONG count, ULONG unit)
{
    ULONG bytes;

    if (count == 0)
        return TRUE;

    if (count > (0xFFFFFFFFUL - 3UL) / unit)
        return FALSE;

    bytes = bsd_aam_round(count * unit);

    if (bytes > 0xFFFFFFFFUL - *total)
        return FALSE;

    *total += bytes;

    return TRUE;
}

LONG bsd_CreateAddrAllocMessageA(register LONG version __asm("d0"),
                                 register LONG protocol __asm("d1"),
                                 register STRPTR interface_name __asm("a0"),
                                 register struct AddressAllocationMessage **result_ptr __asm("a1"),
                                 register struct TagItem *tags __asm("a2"),
                                 register struct AmiSocketBase *SocketBase __asm("a6"))
{
    struct AddressAllocationMessage *aam;
    struct TagItem  *cursor;
    struct TagItem  *item;
    BsdAamWanted     want;
    NX_IP           *ip;
    ULONG            cid_len = 0;
    ULONG            total;
    UBYTE           *carve;

    (VOID)SocketBase;

    /*
     * The checks are in the order the autodoc's ERRORS section lists them. A
     * caller that got both the version and the result pointer wrong is told
     * about the pointer first, because that is the one that means nothing was
     * written anywhere.
     */
    if (result_ptr == NULL)
        return CAAME_Invalid_result_ptr;

    *result_ptr = NULL;

    /*
     * "version, Data structure version; this must be AAM_VERSION". The
     * header is more generous: it defines AAM_VERSION as 2 and
     * AAM_VERSION_MINIMUM as 1, which only means something if 1 is still
     * accepted. Both are taken. aam_Unicast is honoured only at 2 or above,
     * as the header says where it defines the field.
     */
    if (version < AAM_VERSION_MINIMUM || version > AAM_VERSION)
        return CAAME_Invalid_version;

    /*
     * "protocol, Configuration protocol type, either AAMP_BOOTP or
     * AAMP_DHCP." Taken literally. AAMP_SLOWAUTO and AAMP_FASTAUTO are legal
     * values of aam_Protocol for BeginInterfaceConfig() but are not among the
     * two this constructor names. A caller that wants one of them can fill
     * the message in by hand, which the autodoc permits.
     */
    if (protocol != AAMP_BOOTP && protocol != AAMP_DHCP)
        return CAAME_Invalid_protocol;

    if (interface_name == NULL || interface_name[0] == '\0' ||
        bsd_strlen((const char *)interface_name) >= (ULONG)BSD_IFNAME_SIZE)
        return CAAME_Invalid_interface_name;

    /*
     * "CAAME_Interface_not_found, The interface whose name you provided
     * could not be found." The only check in this file that consults the
     * running stack.
     */
    ip = netstack_ip();
    if (ip == NULL || bsd_if_index_of(ip, (const char *)interface_name) < 0)
        return CAAME_Interface_not_found;

    /* ---- what was asked for ------------------------------------------- */

    bsd_bzero(&want, sizeof(want));
    want.baw_Timeout   = AAM_TIMEOUT_MIN;       /* "Default ... 10 seconds" */
    want.baw_LeaseTime = DHCP_DEFAULT_LEASE_TIME;

    cursor = tags;
    while ((item = bsd_next_tag(&cursor)) != NULL)
    {
        switch (item->ti_Tag)
        {
            case CAAMTA_Timeout:
                want.baw_Timeout = (LONG)item->ti_Data;
                break;

            case CAAMTA_LeaseTime:
                want.baw_LeaseTime = (ULONG)item->ti_Data;
                break;

            case CAAMTA_RequestedAddress:
                want.baw_RequestedAddress = (ULONG)item->ti_Data;
                break;

            case CAAMTA_ClientIdentifier:
                want.baw_ClientId = (STRPTR)item->ti_Data;
                break;

            case CAAMTA_NAKMessageSize:
                want.baw_NAKMessage = bsd_aam_size(item);
                break;

            case CAAMTA_RouterTableSize:
                want.baw_RouterTable = bsd_aam_size(item);
                break;

            case CAAMTA_DNSTableSize:
                want.baw_DNSTable = bsd_aam_size(item);
                break;

            case CAAMTA_StaticRouteTableSize:
                want.baw_StaticRouteTable = bsd_aam_size(item);
                break;

            case CAAMTA_HostNameSize:
                want.baw_HostName = bsd_aam_size(item);
                break;

            case CAAMTA_DomainNameSize:
                want.baw_DomainName = bsd_aam_size(item);
                break;

            case CAAMTA_BOOTPMessageSize:
                want.baw_BOOTPMessage = bsd_aam_size(item);
                break;

            case CAAMTA_RecordLeaseExpiration:
                want.baw_LeaseExpires = (item->ti_Data != 0) ? TRUE : FALSE;
                break;

            case CAAMTA_ReplyPort:
                want.baw_ReplyPort = (struct MsgPort *)item->ti_Data;
                break;

            case CAAMTA_RequestUnicast:
                want.baw_Unicast = (item->ti_Data != 0) ? TRUE : FALSE;
                break;

            default:
                break;
        }
    }

    /*
     * The client identifier has three error codes of its own, kept distinct:
     * "not valid" is a pointer that is not a string, "too short" and "too
     * long" are lengths the DHCP option cannot carry.
     */
    if (want.baw_ClientId != NULL)
    {
        cid_len = bsd_strlen((const char *)want.baw_ClientId);

        if (cid_len == 0)
            return CAAME_Invalid_client_identifier;
        if (cid_len < (ULONG)BSD_AAM_CID_MIN)
            return CAAME_Client_identifier_too_short;
        if (cid_len > (ULONG)BSD_AAM_CID_MAX)
            return CAAME_Client_identifier_too_long;
    }

    /* "Due to how the configuration process works, the timeout must be at
       least 10 seconds long. If it is shorter, it is automatically extended
       to 10 seconds." Extended, not refused. */
    if (want.baw_Timeout < AAM_TIMEOUT_MIN)
        want.baw_Timeout = AAM_TIMEOUT_MIN;

    /* ---- one block ------------------------------------------------------ */

    total = bsd_aam_round(sizeof(*aam));

    if (!bsd_aam_add(&total, (ULONG)want.baw_NAKMessage,       1UL) ||
        !bsd_aam_add(&total, (ULONG)want.baw_RouterTable,      sizeof(ULONG)) ||
        !bsd_aam_add(&total, (ULONG)want.baw_DNSTable,         sizeof(ULONG)) ||
        !bsd_aam_add(&total, (ULONG)want.baw_StaticRouteTable, sizeof(ULONG)) ||
        !bsd_aam_add(&total, (ULONG)want.baw_HostName,         1UL) ||
        !bsd_aam_add(&total, (ULONG)want.baw_DomainName,       1UL) ||
        !bsd_aam_add(&total, (ULONG)want.baw_BOOTPMessage,     1UL) ||
        !bsd_aam_add(&total, want.baw_LeaseExpires
                                 ? (ULONG)sizeof(struct DateStamp) : 0UL, 1UL) ||
        !bsd_aam_add(&total, (cid_len != 0) ? cid_len + 1 : 0UL, 1UL))
    {
        return CAAME_Not_enough_memory;
    }

    aam = (struct AddressAllocationMessage *)ami_alloc(total);
    if (aam == NULL)
        return CAAME_Not_enough_memory;

    bsd_bzero(aam, total);

    carve = (UBYTE *)aam + bsd_aam_round(sizeof(*aam));

    /* ---- the message ---------------------------------------------------- */

    /*
     * "The message must be initialized like any other kind of struct Message
     * that is to be attached to a MsgPort and eventually replied with
     * ReplyMsg()." mn_Length is easy for a caller to omit and ReplyMsg()
     * needs it, so it is filled in here.
     */
    aam->aam_Message.mn_Node.ln_Type = NT_MESSAGE;
    aam->aam_Message.mn_Length       = (UWORD)sizeof(*aam);
    aam->aam_Message.mn_ReplyPort    = want.baw_ReplyPort;

    aam->aam_Reserved         = BSD_AAM_COOKIE;
    aam->aam_Result           = AAMR_Ignored;
    aam->aam_Version          = version;
    aam->aam_Protocol         = protocol;
    aam->aam_Timeout          = want.baw_Timeout;
    aam->aam_LeaseTime        = want.baw_LeaseTime;
    aam->aam_RequestedAddress = want.baw_RequestedAddress;

    bsd_strncpy(aam->aam_InterfaceName, (const char *)interface_name,
                sizeof(aam->aam_InterfaceName));

    /* "This requires aam_Version 2 or higher", libraries/bsdsocket.h, at
       the field. A version 1 message has no aam_Unicast to set. */
    if (version >= AAM_VERSION)
        aam->aam_Unicast = want.baw_Unicast;

    /* ---- and its buffers ------------------------------------------------- */

    if (want.baw_NAKMessage > 0)
    {
        aam->aam_NAKMessage     = (STRPTR)carve;
        aam->aam_NAKMessageSize = want.baw_NAKMessage;
        carve += bsd_aam_round((ULONG)want.baw_NAKMessage);
    }

    if (want.baw_RouterTable > 0)
    {
        aam->aam_RouterTable     = (ULONG *)carve;
        aam->aam_RouterTableSize = want.baw_RouterTable;
        carve += bsd_aam_round((ULONG)want.baw_RouterTable * sizeof(ULONG));
    }

    if (want.baw_DNSTable > 0)
    {
        aam->aam_DNSTable     = (ULONG *)carve;
        aam->aam_DNSTableSize = want.baw_DNSTable;
        carve += bsd_aam_round((ULONG)want.baw_DNSTable * sizeof(ULONG));
    }

    if (want.baw_StaticRouteTable > 0)
    {
        aam->aam_StaticRouteTable     = (ULONG *)carve;
        aam->aam_StaticRouteTableSize = want.baw_StaticRouteTable;
        carve += bsd_aam_round((ULONG)want.baw_StaticRouteTable * sizeof(ULONG));
    }

    if (want.baw_HostName > 0)
    {
        aam->aam_HostName     = (STRPTR)carve;
        aam->aam_HostNameSize = want.baw_HostName;
        carve += bsd_aam_round((ULONG)want.baw_HostName);
    }

    if (want.baw_DomainName > 0)
    {
        aam->aam_DomainName     = (STRPTR)carve;
        aam->aam_DomainNameSize = want.baw_DomainName;
        carve += bsd_aam_round((ULONG)want.baw_DomainName);
    }

    if (want.baw_BOOTPMessage > 0)
    {
        aam->aam_BOOTPMessage     = (UBYTE *)carve;
        aam->aam_BOOTPMessageSize = want.baw_BOOTPMessage;
        carve += bsd_aam_round((ULONG)want.baw_BOOTPMessage);
    }

    if (want.baw_LeaseExpires)
    {
        aam->aam_LeaseExpires = (struct DateStamp *)carve;
        carve += bsd_aam_round(sizeof(struct DateStamp));
    }

    /*
     * "The name will be duplicated and stored in the allocation message", so
     * the caller's string need not outlive this call. The copy lives in the
     * same block as everything else.
     */
    if (cid_len != 0)
    {
        aam->aam_ClientIdentifier = (STRPTR)carve;
        bsd_strncpy((char *)carve, (const char *)want.baw_ClientId,
                    cid_len + 1);
        carve += bsd_aam_round(cid_len + 1);
    }

    *result_ptr = aam;

    return CAAME_Success;
}

VOID bsd_DeleteAddrAllocMessage(register struct AddressAllocationMessage *aam __asm("a0"),
                                register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)SocketBase;

    /* "Passing a NULL pointer in place of a valid message address is
       harmless." */
    if (aam == NULL)
        return;

    /*
     * "This routine can only deallocate address allocation messages created
     * by CreateAddrAllocMessageA() and will not work with anything else."
     *
     * Refused quietly rather than freed. A hand-built message is legal for a
     * caller to have, BeginInterfaceConfig() takes one, and passing it
     * here must not corrupt the memory pool. This call has no errno to report
     * it through.
     */
    if (aam->aam_Reserved != BSD_AAM_COOKIE)
        return;

    /* Cleared before the free so that a double delete finds no cookie. */
    aam->aam_Reserved = 0;

    ami_free(aam);
}

/* ------------------------------------------------- BeginInterfaceConfig, */

/*
 * BeginInterfaceConfig() is documented asynchronous, "This routine starts an
 * asynchronous operation, very much like exec.library/SendIO()", and has to
 * be. The timeout is at least ten seconds. A block that long inside a library
 * call the caller expects to return at once deadlocks an application against
 * its own event loop.
 *
 * So one Process per request, which handles the whole lifecycle: start the
 * DHCP client on the interface, poll to its own deadline, fill the message in,
 * ReplyMsg() it, exit. This is also why the netstack's DHCP primitives are not
 * one blocking call: something has to own the deadline, and only this side has
 * a Process and a dos.library to Delay() with.
 *
 * The worker runs code out of the library segment and holds no OpenCnt
 * reference, like the TCP: handler (see bsd_lib_expunge()). The worker count is
 * therefore taken before CreateNewProc() and given back inside Forbid() as the
 * last thing the worker does, and expunge declines while it is non-zero.
 *
 * It does hold a transient netstack reference. BeginInterfaceConfig() returns
 * before the operation completes, so the opener that launched it may close;
 * without this separate reference the last close can dismantle and later
 * recreate the stack while the old worker is still polling its numeric slot.
 */

/* Defined below, with the rest of BeginInterfaceConfig()'s reporting. */
static VOID bsd_aam_reply(struct AddressAllocationMessage *aam, LONG result);

/*
 * 8 KB, not the 4 KB this used to be. -fstack-usage plus the call graph put
 * bsd_aam_worker's worst chain at 1,936 bytes, 47% of a 4 KB stack before
 * anything the analysis cannot see. It cannot see much here: the SANA-II calls
 * below go through the driver's own IO handling on this stack, and that driver
 * is third-party code. The chain is 1,136 now. There is one of these per
 * interface under configuration, and the margin is cheap.
 */
#define BSD_AAM_STACK       8192
#define BSD_AAM_PRI         0

/* How often the worker looks at the DHCP state. Ten times a second is much
   finer than DHCP moves and costs nothing next to the ten-second floor. */
#define BSD_AAM_POLL_TICKS  5           /* Delay() ticks: 1/10th of a second */

/* baj_Msg first: the job is handed to its worker by PutMsg() to the worker's
   own pr_MsgPort, so the message and the job are the same block. */
typedef struct BsdAamJob
{
    struct Message                   baj_Msg;
    struct AddressAllocationMessage *baj_Message;
    struct AmiSocketBase            *baj_Master;
    UWORD                            baj_Index;
    volatile BOOL                    baj_Abort;
    volatile BOOL                    baj_Done;
} BsdAamJob;

/*
 * Jobs in flight, at most one per interface. A fixed table rather than a list
 * because AbortInterfaceConfig() has to find a job from nothing but the
 * message pointer, and because the published API already limits it to one
 * allocation per interface, AAMR_Busy is what the second asker gets.
 */
static BsdAamJob *bsd_aam_jobs[AMI_CFG_MAX_INTERFACES];
static LONG       bsd_aam_workers;

BOOL bsd_aam_busy(VOID)
{
    return (bsd_aam_workers > 0) ? TRUE : FALSE;
}

/* The job for a message, or NULL. Called under Forbid(). */
static BsdAamJob *bsd_aam_find(const struct AddressAllocationMessage *aam)
{
    UWORD i;

    for (i = 0; i < (UWORD)AMI_CFG_MAX_INTERFACES; i++)
    {
        if (bsd_aam_jobs[i] != NULL && bsd_aam_jobs[i]->baj_Message == aam)
            return bsd_aam_jobs[i];
    }

    return NULL;
}

/* Everything the server said, into the buffers the caller reserved. */
static VOID bsd_aam_store_lease(struct AddressAllocationMessage *aam,
                                const AmiDhcpLease *lease)
{
    ULONG i;

    aam->aam_Address       = lease->adl_Address;
    aam->aam_SubnetMask    = lease->adl_NetMask;
    aam->aam_ServerAddress = lease->adl_Server;

    /* "The allocation process will eventually reset aam_RequestedAddress to
       zero." */
    aam->aam_RequestedAddress = 0;

    if (aam->aam_RouterTable != NULL && aam->aam_RouterTableSize > 0)
    {
        for (i = 0; i < (ULONG)aam->aam_RouterTableSize; i++)
            aam->aam_RouterTable[i] = (i < (ULONG)lease->adl_RouterCount)
                                          ? lease->adl_Router[i] : 0;
    }

    if (aam->aam_DNSTable != NULL && aam->aam_DNSTableSize > 0)
    {
        for (i = 0; i < (ULONG)aam->aam_DNSTableSize; i++)
            aam->aam_DNSTable[i] = (i < (ULONG)lease->adl_DnsCount)
                                       ? lease->adl_Dns[i] : 0;
    }

    if (aam->aam_StaticRouteTable != NULL &&
        aam->aam_StaticRouteTableSize > 0)
    {
        for (i = 0; i < (ULONG)aam->aam_StaticRouteTableSize; i++)
            aam->aam_StaticRouteTable[i] =
                (i < (ULONG)lease->adl_StaticRouteCount)
                                               ? lease->adl_StaticRoute[i] : 0;
    }

    if (aam->aam_HostName != NULL && aam->aam_HostNameSize > 0)
        bsd_strncpy((char *)aam->aam_HostName, lease->adl_HostName,
                    (ULONG)aam->aam_HostNameSize);

    if (aam->aam_DomainName != NULL && aam->aam_DomainNameSize > 0)
        bsd_strncpy((char *)aam->aam_DomainName, lease->adl_DomainName,
                    (ULONG)aam->aam_DomainNameSize);

    /*
     * "If the lease is infinitely long, then the DateStamp will contain all
     * zeroes." It is already zero from the allocation, so an infinite lease
     * needs no work. DateStamp() is called only when there is a finite lease
     * to add to the current time.
     */
    if (aam->aam_LeaseExpires != NULL &&
        lease->adl_LeaseSeconds != 0 &&
        lease->adl_LeaseSeconds != DHCP_INFINITE_LEASE_TIME)
    {
        struct DateStamp now;

        DateStamp(&now);

        now.ds_Minute += (LONG)(lease->adl_LeaseSeconds / 60UL);
        now.ds_Days   += now.ds_Minute / (24L * 60L);
        now.ds_Minute %= (24L * 60L);

        *aam->aam_LeaseExpires = now;
    }
}

/*
 * The worker. Runs as its own Process, so it can Delay() and block. The
 * vector that started it can do neither.
 */
static VOID bsd_aam_worker(VOID)
{
    struct AddressAllocationMessage *aam;
    struct AmiSocketBase            *master;
    struct Process                  *me = (struct Process *)FindTask(NULL);
    AmiDhcpLease                     lease;
    BsdAamJob                       *job;
    LONG                             result = AAMR_Timeout;
    LONG                             rc;

    /*
     * The launcher PutMsg()ed the job here, the same way tcp_ctrl_find() hands
     * a FIND packet to a session. It is the only message that ever arrives on
     * this port, and it arrives whether or not this Process got here first.
     *
     * Waited for in a loop rather than taken once. The slot in bsd_aam_jobs[]
     * is claimed before CreateNewProc(). A worker that gave up on a NULL
     * GetMsg() therefore left that slot claimed, with nothing to release it
     * and no message to reply. It can do neither, because the job is the
     * only thing that names the index and the message. Every other path out of
     * here releases the slot and replies, and that path cannot, so it is gone.
     * The launcher PutMsg()s exactly once and unconditionally, so a message
     * always arrives and the wait always ends.
     */
    do
    {
        WaitPort(&me->pr_MsgPort);
        job = (BsdAamJob *)GetMsg(&me->pr_MsgPort);
    }
    while (job == NULL);

    aam = job->baj_Message;
    master = job->baj_Master;

    rc = netstack_interface_dhcp_start(job->baj_Index,
                                       aam->aam_RequestedAddress);
    if (rc != AMI_NET_OK)
    {
        result = (rc == AMI_NET_ERR_BUSY) ? AAMR_Busy : AAMR_AddrChangeFailed;
    }
    else
    {
        /*
         * aam_Timeout is in seconds and was floored at ten when the message
         * was built. Floored again here, because a hand-filled message never
         * went through CreateAddrAllocMessageA(). Keep seconds and the ticks
         * within one second separate: a LONG timeout multiplied by 50 ticks
         * does not fit in a LONG for the full range the ABI permits.
         */
        LONG  seconds = aam->aam_Timeout;
        ULONG seconds_left;
        ULONG ticks_left = (ULONG)TICKS_PER_SECOND;

        if (seconds < AAM_TIMEOUT_MIN)
            seconds = AAM_TIMEOUT_MIN;

        seconds_left = (ULONG)seconds;

        while (seconds_left > 0)
        {
            LONG state;
            ULONG delay_ticks;

            if (job->baj_Abort)
            {
                result = AAMR_Aborted;
                break;
            }

            state = netstack_interface_dhcp_state(job->baj_Index);

            if (state == AMI_DHCP_BOUND)
            {
                result = AAMR_Success;
                break;
            }

            if (state < 0)
            {
                result = AAMR_AddrChangeFailed;
                break;
            }

            delay_ticks = (ticks_left < (ULONG)BSD_AAM_POLL_TICKS)
                              ? ticks_left : (ULONG)BSD_AAM_POLL_TICKS;
            Delay((LONG)delay_ticks);

            ticks_left -= delay_ticks;
            if (ticks_left == 0)
            {
                seconds_left--;
                ticks_left = (ULONG)TICKS_PER_SECOND;
            }
        }

        if (result == AAMR_Success)
        {
            if (netstack_interface_dhcp_lease(job->baj_Index, &lease)
                    == AMI_NET_OK)
                bsd_aam_store_lease(aam, &lease);
            else
                result = AAMR_AddrChangeFailed;
        }
        else
        {
            /*
             * Abandoned: stop the client and release the address if it got
             * one, so the server does not hold a lease nothing is using. On a
             * timeout it never got that far, which nx_dhcp_interface_release()
             * copes with.
             */
            (VOID)netstack_interface_dhcp_stop(job->baj_Index, TRUE);
        }
    }

    /*
     * The job leaves the table before the message is replied. After the reply
     * the message is the caller's again and can already be deleted, so nothing
     * must point at it and AbortInterfaceConfig() must no longer find it.
     */
    Forbid();
    if (job->baj_Index < (UWORD)AMI_CFG_MAX_INTERFACES &&
        bsd_aam_jobs[job->baj_Index] == job)
        bsd_aam_jobs[job->baj_Index] = NULL;
    job->baj_Done = TRUE;
    Permit();

    /* The slot can be removed again as soon as no worker can touch it. This
       must precede the transient stack release, because that release may be
       the operation that tears the netstack down. */
    netstack_interface_release(job->baj_Index);

    bsd_aam_reply(aam, result);

    ami_free(job);

    bsd_stack_transient_release(master);

    /*
     * Inside Forbid() so that expunge cannot see the count reach zero, free
     * the segment, and leave these last instructions running out of memory
     * that was handed back. Same reasoning as tcp_session_main()'s exit.
     */
    Forbid();
    bsd_aam_workers--;
}


/*
 * BeginInterfaceConfig() returns VOID. It reports everything through
 * aam_Result and the replied message. The ENOSYS stub this replaces therefore
 * hung rather than refused. It returned -1 in a register the caller cannot
 * see, and never replied the message the caller was already waiting on. An
 * application that called the documented BeginInterfaceConfig() and then
 * WaitPort() waited forever.
 *
 * The validation half is therefore implemented, and every outcome is reported
 * through the documented result codes, with the message replied.
 *
 * Only AAMP_DHCP is run. The other three are not:
 *
 *   AAMP_BOOTP                this stack has no BOOTP client. NetX Duo ships
 *                             DHCP. BOOTP is a different wire protocol and
 *                             nothing here implements it.
 *   AAMP_SLOWAUTO             RFC 3927 self-assignment. NX_AUTO_IP is in the
 *   AAMP_FASTAUTO             build and drives the LINKLOCAL configuration
 *                             type, so the protocol is there. What is missing
 *                             is any way to tell the two variants apart,
 *                             because the autodoc distinguishes them only by
 *                             timeout lengths it does not give.
 *
 * All three are replied AAMR_Ignored, "Your request was not understood and
 * was therefore ignored".
 *
 * The checks below run in the order that gives the caller the most useful
 * answer: what is wrong with the message first, then what is wrong with the
 * interface, then "this stack does not do that".
 */

/* Fill in the result and hand the message back. */
static VOID bsd_aam_reply(struct AddressAllocationMessage *aam, LONG result)
{
    aam->aam_Result = result;

    /*
     * "it will be returned to the caller via ReplyMsg()". No reply port means
     * the caller does not want the message back. exec's ReplyMsg() tolerates
     * that, but the check is explicit here rather than a reliance on it.
     */
    if (aam->aam_Message.mn_ReplyPort != NULL)
        ReplyMsg(&aam->aam_Message);
}

/*
 * Start a worker for one request, or reply the message with the reason it
 * cannot be started. Everything that can fail here fails before the Process
 * exists, so there is never a worker with nothing to do.
 */
static VOID bsd_aam_launch(struct AddressAllocationMessage *aam, UWORD index,
                           struct AmiSocketBase *master)
{
    struct Process *proc;
    struct TagItem  tags[6];
    struct Task    *me = FindTask(NULL);
    BsdAamJob      *job;

    /*
     * CreateNewProc() wants a Process to inherit from. A bare Task that asks
     * for an address allocation is told the request was ignored, rather than
     * crashed. bsd_tcp_handler_start() does the same for the same call.
     */
    if (me == NULL || me->tc_Node.ln_Type != NT_PROCESS)
    {
        netstack_interface_release(index);
        bsd_stack_transient_release(master);
        bsd_aam_reply(aam, AAMR_Ignored);
        return;
    }

    job = (BsdAamJob *)ami_alloc(sizeof(*job));
    if (job == NULL)
    {
        netstack_interface_release(index);
        bsd_stack_transient_release(master);
        bsd_aam_reply(aam, AAMR_NoMemory);
        return;
    }

    job->baj_Message = aam;
    job->baj_Master  = master;
    job->baj_Index   = index;
    job->baj_Abort   = FALSE;
    job->baj_Done    = FALSE;

    /*
     * "AAMR_Busy, Address allocation is already in progress for this
     * interface." The table slot is claimed here rather than in the worker so
     * that two racing callers cannot both get one.
     */
    Forbid();

    if (bsd_aam_jobs[index] != NULL)
    {
        Permit();
        ami_free(job);
        netstack_interface_release(index);
        bsd_stack_transient_release(master);
        bsd_aam_reply(aam, AAMR_Busy);
        return;
    }

    bsd_aam_jobs[index] = job;
    bsd_aam_workers++;

    Permit();

    job->baj_Msg.mn_Node.ln_Type = NT_MESSAGE;
    job->baj_Msg.mn_ReplyPort    = NULL;
    job->baj_Msg.mn_Length       = (UWORD)sizeof(*job);

    tags[0].ti_Tag  = NP_Entry;
    tags[0].ti_Data = (ULONG)bsd_aam_worker;
    tags[1].ti_Tag  = NP_Name;
    tags[1].ti_Data = (ULONG)"bsdsocket address allocation";
    tags[2].ti_Tag  = NP_StackSize;
    tags[2].ti_Data = BSD_AAM_STACK;
    tags[3].ti_Tag  = NP_Priority;
    tags[3].ti_Data = (ULONG)BSD_AAM_PRI;
    tags[4].ti_Tag  = NP_Cli;
    tags[4].ti_Data = FALSE;
    tags[5].ti_Tag  = TAG_DONE;
    tags[5].ti_Data = 0;

    /*
     * Outside the Forbid(). CreateNewProc() inherits the caller's current
     * directory, which is a DupLock(), a packet to a file system and a wait
     * on the reply. A Forbid() held across it is therefore broken for as long
     * as that takes, and protects nothing. The hand-over is a PutMsg() to the
     * new Process's own port instead of a shared slot, so that Forbid() has
     * nothing left to guard.
     */
    proc = CreateNewProc(tags);

    if (proc == NULL)
    {
        Forbid();
        bsd_aam_jobs[index] = NULL;
        bsd_aam_workers--;
        Permit();

        ami_free(job);
        netstack_interface_release(index);
        bsd_stack_transient_release(master);
        bsd_aam_reply(aam, AAMR_NoMemory);
        return;
    }

    PutMsg(&proc->pr_MsgPort, &job->baj_Msg);
}

VOID bsd_BeginInterfaceConfig(register struct AddressAllocationMessage *aam __asm("a0"),
                              register struct AmiSocketBase *SocketBase __asm("a6"))
{
    struct AmiSocketBase *master;
    NX_IP                *ip;
    NX_INTERFACE         *nxif;
    UWORD                 index;
    LONG                  rc;

    /* Nothing to reply through, so nothing can be reported. */
    if (aam == NULL)
        return;

    /*
     * "You must fill in this member or the message will be rejected." The
     * test is a range rather than an equality because of the header's
     * AAM_VERSION_MINIMUM. See bsd_CreateAddrAllocMessageA().
     */
    if (aam->aam_Version < AAM_VERSION_MINIMUM ||
        aam->aam_Version > AAM_VERSION)
    {
        bsd_aam_reply(aam, AAMR_VersionUnknown);
        return;
    }

    /* aam_InterfaceName is a fixed sixteen bytes and a hand-filled message
       need not terminate it, so it is bounded here. */
    if (aam->aam_InterfaceName[0] == '\0')
    {
        bsd_aam_reply(aam, AAMR_InterfaceNotKnown);
        return;
    }

    aam->aam_InterfaceName[sizeof(aam->aam_InterfaceName) - 1] = '\0';

    rc = netstack_interface_claim(aam->aam_InterfaceName, &index);
    if (rc != AMI_NET_OK)
    {
        bsd_aam_reply(aam, AAMR_InterfaceNotKnown);
        return;
    }

    ip = netstack_ip();
    nxif = &ip->nx_ip_interface[index];

    /*
     * "The BOOTP protocol only works with broadcast interfaces (e.g. Ethernet
     * hardware). The interface you provided is not of this kind."
     *
     * NetX Duo records that as address mapping being needed: an interface
     * that resolves addresses with ARP is one that broadcasts. An interface
     * with no SANA-II device behind it cannot broadcast either.
     */
    if (nxif->nx_interface_address_mapping_needed == 0 ||
        nxif->nx_interface_additional_link_info == NULL)
    {
        netstack_interface_release(index);
        bsd_aam_reply(aam, AAMR_InterfaceWrongType);
        return;
    }

    /* "The interface already has an IP address assigned." */
    if (nxif->nx_interface_ip_address != 0)
    {
        netstack_interface_release(index);
        bsd_aam_reply(aam, AAMR_AddressKnown);
        return;
    }

    /*
     * The request is in order. Only DHCP is run, see the block comment
     * above for what the other three need, and the rest still get their
     * message back.
     */
    if (aam->aam_Protocol != AAMP_DHCP)
    {
        netstack_interface_release(index);
        bsd_aam_reply(aam, AAMR_Ignored);
        return;
    }

    master = SocketBase;
    if (master != NULL && master->sb_Master != NULL)
        master = master->sb_Master;

    if (bsd_stack_transient_hold(master) != 0)
    {
        netstack_interface_release(index);
        bsd_aam_reply(aam, AAMR_InterfaceNotKnown);
        return;
    }

    bsd_aam_launch(aam, index, master);
}

VOID bsd_AbortInterfaceConfig(register struct AddressAllocationMessage *aam __asm("a0"),
                              register struct AmiSocketBase *SocketBase __asm("a6"))
{
    BsdAamJob *job;

    (VOID)SocketBase;

    if (aam == NULL)
        return;

    /*
     * "There is no guarantee that the message can be intercepted and the IP
     * address allocation process aborted by this routine. In fact, the
     * process can complete before this routine has managed to abort it."
     *
     * The flag is raised under Forbid() and the worker reads it at the top of
     * its next poll. If the worker already took the job out of the table,
     * which it does before it replies, there is nothing to find. That is the
     * race the autodoc describes. A flag set in a job that is no longer listed
     * writes into memory that is about to be freed.
     *
     * A message that was never begun, or one already replied, finds nothing
     * and does nothing. Both are legal for a caller to do.
     */
    Forbid();

    job = bsd_aam_find(aam);
    if (job != NULL)
        job->baj_Abort = TRUE;

    Permit();
}
