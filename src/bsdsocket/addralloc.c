/*
 * bsdsocket.library -- the address allocation message.
 *
 *   CreateAddrAllocMessageA()   build one, with its buffers
 *   DeleteAddrAllocMessage()    give it back
 *
 * These are the constructor and destructor for the struct
 * AddressAllocationMessage that BeginInterfaceConfig() takes. They do not
 * touch the network: all they do is validate, allocate and fill in defaults.
 *
 * WRITTEN FROM THE AUTODOC
 *
 * Same primary source as interfaces.c: NDK 3.2's
 * SANA+RoadshowTCP-IP/doc/bsdsocket.doc, plus libraries/bsdsocket.h from the
 * same NDK, used as an ABI reference only. No Roadshow, AmiTCP, AROSTCP or
 * Miami code was consulted or is present.
 *
 * The autodoc enumerates ten distinct error codes for this one call and says
 * which condition produces each. That is unusually specific, and it is the
 * whole reason this file is worth writing rather than guessing: a caller that
 * gets CAAME_Client_identifier_too_short can fix its input, and one that gets
 * a generic failure cannot.
 *
 * ONE ALLOCATION, AND A COOKIE IN THE RESERVED FIELD
 *
 * Every buffer the tags ask for is carved out of a single block that the
 * message sits at the top of, so DeleteAddrAllocMessage() is one free of the
 * pointer it was handed. The alternative -- a separate allocation per buffer
 * -- would need a bookkeeping structure that the published message has no
 * room for.
 *
 * "This routine can only deallocate address allocation messages created by
 * CreateAddrAllocMessageA() and will not work with anything else." So it has
 * to be able to TELL, and the message carries a cookie in aam_Reserved to
 * make that possible. That field is the library's to use -- it is reserved
 * from the application's side, not from ours -- and a message the caller
 * filled in by hand, which the autodoc explicitly permits, simply will not
 * have it and is refused rather than freed.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "aminetxduo/config.h"

#include "tagwalk.h"
#include "interfaces.h"

#include <proto/dos.h>
#include <proto/exec.h>

#include <dos/dostags.h>

/*
 * The cookie. Arbitrary, and chosen to be something no plausible hand-filled
 * message would contain -- a caller that memsets its message to zero, which
 * the autodoc tells it to do, leaves aam_Reserved at zero.
 */
#define BSD_AAM_COOKIE      0x414D4147L     /* 'AMAG' */

/* "cannot be longer than 255 characters ... names shorter than 2 characters
   will be ignored" -- both bounds are the autodoc's. */
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

/* Sizes are byte counts and table sizes are entry counts; a negative one is
   not a small request, it is a bad argument. */
static LONG bsd_aam_size(const struct TagItem *item)
{
    LONG value = (LONG)item->ti_Data;

    return (value > 0) ? value : 0;
}

/*
 * Everything in the block after the message itself is longword-aligned,
 * because two of the buffers are arrays of ULONG and m68k will fault on a
 * misaligned one -- and because ami_alloc() returns memory aligned enough for
 * anything, the carving is the only place alignment can be lost.
 */
static ULONG bsd_aam_round(ULONG size)
{
    return (size + 3UL) & ~3UL;
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
     * The order below is the order the ERRORS section lists, and it matters:
     * a caller that got both the version and the result pointer wrong should
     * be told about the pointer first, because that is the one that means
     * nothing was written anywhere.
     */
    if (result_ptr == NULL)
        return CAAME_Invalid_result_ptr;

    *result_ptr = NULL;

    /*
     * "version -- Data structure version; this must be AAM_VERSION". The
     * header is more generous than that sentence: it defines AAM_VERSION as 2
     * AND AAM_VERSION_MINIMUM as 1, which only means something if 1 is still
     * accepted. Both are taken, and aam_Unicast is honoured only at 2 or
     * above -- the header says so where it defines the field.
     */
    if (version < AAM_VERSION_MINIMUM || version > AAM_VERSION)
        return CAAME_Invalid_version;

    /*
     * "protocol -- Configuration protocol type, either AAMP_BOOTP or
     * AAMP_DHCP." Read literally, and it is: AAMP_SLOWAUTO and AAMP_FASTAUTO
     * are legal values of aam_Protocol for BeginInterfaceConfig() but are not
     * among the two this constructor names, and a caller that wants one of
     * them may still fill the message in by hand -- which the autodoc
     * explicitly permits.
     */
    if (protocol != AAMP_BOOTP && protocol != AAMP_DHCP)
        return CAAME_Invalid_protocol;

    if (interface_name == NULL || interface_name[0] == '\0' ||
        bsd_strlen((const char *)interface_name) >= (ULONG)BSD_IFNAME_SIZE)
        return CAAME_Invalid_interface_name;

    /*
     * "CAAME_Interface_not_found -- The interface whose name you provided
     * could not be found." So this one does consult the running stack, and it
     * is the only part of this file that does.
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
     * The client identifier gets three error codes of its own, and they are
     * distinct on purpose: "not valid" is a pointer that is not a string,
     * "too short" and "too long" are lengths the DHCP option cannot carry.
     * A caller told which of the three it hit does not have to guess.
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

    total  = bsd_aam_round(sizeof(*aam));
    total += bsd_aam_round((ULONG)want.baw_NAKMessage);
    total += bsd_aam_round((ULONG)want.baw_RouterTable * sizeof(ULONG));
    total += bsd_aam_round((ULONG)want.baw_DNSTable * sizeof(ULONG));
    total += bsd_aam_round((ULONG)want.baw_StaticRouteTable * sizeof(ULONG));
    total += bsd_aam_round((ULONG)want.baw_HostName);
    total += bsd_aam_round((ULONG)want.baw_DomainName);
    total += bsd_aam_round((ULONG)want.baw_BOOTPMessage);
    total += want.baw_LeaseExpires ? bsd_aam_round(sizeof(struct DateStamp)) : 0;
    total += (cid_len != 0) ? bsd_aam_round(cid_len + 1) : 0;

    aam = (struct AddressAllocationMessage *)ami_alloc(total);
    if (aam == NULL)
        return CAAME_Not_enough_memory;

    bsd_bzero(aam, total);

    carve = (UBYTE *)aam + bsd_aam_round(sizeof(*aam));

    /* ---- the message ---------------------------------------------------- */

    /*
     * "The message must be initialized like any other kind of struct Message
     * that is to be attached to a MsgPort and eventually replied with
     * ReplyMsg()." mn_Length is the part a caller forgets and ReplyMsg()
     * needs; it is filled in here so that it cannot be forgotten.
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

    /* "This requires aam_Version 2 or higher" -- libraries/bsdsocket.h, at
       the field itself. A version 1 message has no aam_Unicast to set. */
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
     * "The name will be duplicated and stored in the allocation message" --
     * so the caller's string does not have to outlive this call, and the copy
     * lives in the same block as everything else.
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
     * Refused rather than freed, and quietly: a message built by hand is a
     * legal thing for a caller to have -- BeginInterfaceConfig() takes one --
     * and handing it here is a mistake that must not become a corrupted
     * memory pool. There is no errno on this call to report it through.
     */
    if (aam->aam_Reserved != BSD_AAM_COOKIE)
        return;

    /* Cleared before the free so that a double delete finds no cookie. */
    aam->aam_Reserved = 0;

    ami_free(aam);
}

/* ------------------------------------------------- BeginInterfaceConfig -- */

/*
 * THE WORKER
 *
 * BeginInterfaceConfig() is documented asynchronous -- "This routine starts an
 * asynchronous operation, very much like exec.library/SendIO()" -- and it has
 * to be, because the timeout it is given is at least ten seconds and blocking
 * a caller for ten seconds inside a library call it expects to return at once
 * is how an application deadlocks against its own event loop.
 *
 * So one Process per request. It owns the whole lifecycle: start the DHCP
 * client on the interface, poll to its own deadline, fill the message in,
 * ReplyMsg() it, and exit. That is why the netstack's DHCP primitives are not
 * one blocking call -- somebody has to own the deadline, and the only party
 * with a Process and a dos.library to Delay() with is this one.
 *
 * WHY THE COUNT, AND WHY EXPUNGE LOOKS AT IT
 *
 * The worker runs code out of the library segment while holding no OpenCnt
 * reference, exactly like the TCP: handler (see bsd_lib_expunge()). If the
 * last opener closes while a worker is between instructions, UnLoadSeg()
 * takes the ground out from under it. So the count is taken BEFORE
 * CreateNewProc() and given back inside Forbid() as the last thing the worker
 * does, and expunge declines while it is non-zero.
 */

/* Defined below, beside the rest of BeginInterfaceConfig()'s reporting. */
static VOID bsd_aam_reply(struct AddressAllocationMessage *aam, LONG result);

#define BSD_AAM_STACK       4096
#define BSD_AAM_PRI         0

/* How often the worker looks at the DHCP state. Ten times a second is far
   finer than DHCP moves and costs nothing next to the ten-second floor. */
#define BSD_AAM_POLL_TICKS  5           /* Delay() ticks: 1/10th of a second */

typedef struct BsdAamJob
{
    struct AddressAllocationMessage *baj_Message;
    UWORD                            baj_Index;
    volatile BOOL                    baj_Abort;
    volatile BOOL                    baj_Done;
} BsdAamJob;

/*
 * In flight, at most one per interface. A fixed table rather than a list
 * because AbortInterfaceConfig() has to find a job from nothing but the
 * message pointer, and because "at most one allocation per interface" is a
 * rule the published API already states -- AAMR_Busy exists for the second
 * asker.
 */
static BsdAamJob *bsd_aam_jobs[AMI_CFG_MAX_INTERFACES];
static LONG       bsd_aam_workers;

/* The hand-over to a freshly created Process, the same idiom tcp_handler.c
   uses: a static slot plus a SIGF_SINGLE handshake, so the pointer cannot
   outlive the launch. */
static BsdAamJob *bsd_aam_boot;
static struct Task *bsd_aam_boot_parent;

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
    UWORD i;

    aam->aam_Address       = lease->adl_Address;
    aam->aam_SubnetMask    = lease->adl_NetMask;
    aam->aam_ServerAddress = lease->adl_Server;

    /* "The allocation process will eventually reset aam_RequestedAddress to
       zero." */
    aam->aam_RequestedAddress = 0;

    if (aam->aam_RouterTable != NULL)
    {
        for (i = 0; i < (UWORD)aam->aam_RouterTableSize; i++)
            aam->aam_RouterTable[i] = (i < lease->adl_RouterCount)
                                          ? lease->adl_Router[i] : 0;
    }

    if (aam->aam_DNSTable != NULL)
    {
        for (i = 0; i < (UWORD)aam->aam_DNSTableSize; i++)
            aam->aam_DNSTable[i] = (i < lease->adl_DnsCount)
                                       ? lease->adl_Dns[i] : 0;
    }

    if (aam->aam_StaticRouteTable != NULL)
    {
        for (i = 0; i < (UWORD)aam->aam_StaticRouteTableSize; i++)
            aam->aam_StaticRouteTable[i] = (i < lease->adl_StaticRouteCount)
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
     * needs nothing done to it -- and DateStamp() is asked for the current
     * time only when there is a finite lease to add to it.
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
 * The worker. Runs as its own Process, so it may Delay() and it may block --
 * neither of which the vector that started it could have done.
 */
static VOID bsd_aam_worker(VOID)
{
    struct AddressAllocationMessage *aam;
    AmiDhcpLease lease;
    BsdAamJob   *job;
    LONG         deadline;
    LONG         result = AAMR_Timeout;
    LONG         rc;

    /* The hand-over, before anything that can block. */
    Forbid();
    job = bsd_aam_boot;
    bsd_aam_boot = NULL;
    Permit();

    if (bsd_aam_boot_parent != NULL)
        Signal(bsd_aam_boot_parent, SIGF_SINGLE);

    if (job == NULL)
    {
        Forbid();
        bsd_aam_workers--;
        return;
    }

    aam = job->baj_Message;

    rc = netstack_interface_dhcp_start(job->baj_Index,
                                       aam->aam_RequestedAddress);
    if (rc != AMI_NET_OK)
    {
        result = (rc == AMI_NET_ERR_BUSY) ? AAMR_Busy : AAMR_AddrChangeFailed;
    }
    else
    {
        /*
         * The deadline, in Delay() ticks. aam_Timeout is in seconds and has
         * already been floored at ten by whoever built the message -- and
         * floored again here, because a message filled in by hand never went
         * through CreateAddrAllocMessageA().
         */
        LONG seconds = aam->aam_Timeout;

        if (seconds < AAM_TIMEOUT_MIN)
            seconds = AAM_TIMEOUT_MIN;

        deadline = seconds * (LONG)TICKS_PER_SECOND;

        while (deadline > 0)
        {
            LONG state;

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

            Delay(BSD_AAM_POLL_TICKS);
            deadline -= BSD_AAM_POLL_TICKS;
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
             * Abandoned: stop the client and give the address back if it got
             * one, so the server does not hold a lease nothing is using. A
             * timeout means it never got that far, which nx_dhcp_interface_
             * release() copes with.
             */
            (VOID)netstack_interface_dhcp_stop(job->baj_Index, TRUE);
        }
    }

    /*
     * The job leaves the table BEFORE the message is replied. After the reply
     * the message is the caller's again -- it may already have been deleted
     * by the time this returns -- so nothing may point at it, and
     * AbortInterfaceConfig() must no longer find it.
     */
    Forbid();
    if (job->baj_Index < (UWORD)AMI_CFG_MAX_INTERFACES &&
        bsd_aam_jobs[job->baj_Index] == job)
        bsd_aam_jobs[job->baj_Index] = NULL;
    job->baj_Done = TRUE;
    Permit();

    bsd_aam_reply(aam, result);

    ami_free(job);

    /*
     * Inside Forbid() so that expunge cannot see the count reach zero, take
     * the segment away, and leave these last instructions executing out of
     * memory that has been handed back. The same reasoning as
     * tcp_session_main()'s exit.
     */
    Forbid();
    bsd_aam_workers--;
}


/*
 * WHY THIS IS HERE AT ALL, WHEN THE ALLOCATION ITSELF IS NOT
 *
 * BeginInterfaceConfig() returns VOID. Everything it has to say, it says by
 * filling in aam_Result and replying the message -- which means the ENOSYS
 * stub this replaces was not a refusal, it was a HANG: it returned -1 in a
 * register the caller cannot see, and never replied the message the caller
 * was already waiting on. An application that did the documented thing --
 * BeginInterfaceConfig() then WaitPort() -- waited forever.
 *
 * So the validation half is implemented and every one of its outcomes is
 * reported through the documented result codes, replied properly. A caller
 * gets its message back, learns which condition it hit, and can carry on.
 *
 * AAMP_DHCP IS RUN. The other three are not:
 *
 *   AAMP_BOOTP                this stack has no BOOTP client. NetX Duo ships
 *                             DHCP; BOOTP is a different wire protocol and
 *                             nothing here speaks it.
 *   AAMP_SLOWAUTO             RFC 3927 self-assignment. NX_AUTO_IP is in the
 *   AAMP_FASTAUTO             build and drives the LINKLOCAL configuration
 *                             type, so the protocol is there -- what is not
 *                             is any way to tell the two flavours apart,
 *                             since the autodoc distinguishes them only by
 *                             timeout lengths it does not give.
 *
 * All three are replied AAMR_Ignored -- "Your request was not understood and
 * was therefore ignored", which is literally what happens.
 *
 * The order of the checks below is the order that gives the caller the most
 * useful answer: what is wrong with the MESSAGE first, then what is wrong
 * with the INTERFACE, and only then "this stack does not do that".
 */

/* Fill in the result and hand the message back. */
static VOID bsd_aam_reply(struct AddressAllocationMessage *aam, LONG result)
{
    aam->aam_Result = result;

    /*
     * "it will be returned to the caller via ReplyMsg()". A message with no
     * reply port is a caller that does not want it back -- exec's ReplyMsg()
     * tolerates that, but saying so here is clearer than relying on it.
     */
    if (aam->aam_Message.mn_ReplyPort != NULL)
        ReplyMsg(&aam->aam_Message);
}

/*
 * Start a worker for one request, or reply the message with the reason it
 * could not be started. Everything that can fail here fails BEFORE the
 * Process exists, so there is never a worker with nothing to do.
 */
static VOID bsd_aam_launch(struct AddressAllocationMessage *aam, UWORD index)
{
    struct Process *proc;
    struct TagItem  tags[6];
    struct Task    *me = FindTask(NULL);
    BsdAamJob      *job;

    /*
     * CreateNewProc() wants a Process to inherit from. A bare Task that asks
     * for an address allocation is told the request was ignored rather than
     * taking the machine down -- the same rule bsd_tcp_handler_start()
     * follows for the same call.
     */
    if (me == NULL || me->tc_Node.ln_Type != NT_PROCESS)
    {
        bsd_aam_reply(aam, AAMR_Ignored);
        return;
    }

    job = (BsdAamJob *)ami_alloc(sizeof(*job));
    if (job == NULL)
    {
        bsd_aam_reply(aam, AAMR_NoMemory);
        return;
    }

    job->baj_Message = aam;
    job->baj_Index   = index;
    job->baj_Abort   = FALSE;
    job->baj_Done    = FALSE;

    /*
     * "AAMR_Busy -- Address allocation is already in progress for this
     * interface." One at a time, and the table slot is claimed here rather
     * than in the worker so that two callers racing cannot both get one.
     */
    Forbid();

    if (bsd_aam_jobs[index] != NULL)
    {
        Permit();
        ami_free(job);
        bsd_aam_reply(aam, AAMR_Busy);
        return;
    }

    bsd_aam_jobs[index] = job;
    bsd_aam_workers++;

    /* The hand-over slot is guarded by the same Forbid() as the launch, so
       two BeginInterfaceConfig() calls cannot swap each other's jobs. */
    bsd_aam_boot        = job;
    bsd_aam_boot_parent = me;

    SetSignal(0, SIGF_SINGLE);

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

    proc = CreateNewProc(tags);

    if (proc == NULL)
    {
        bsd_aam_jobs[index] = NULL;
        bsd_aam_workers--;
        bsd_aam_boot        = NULL;
        bsd_aam_boot_parent = NULL;
        Permit();

        ami_free(job);
        bsd_aam_reply(aam, AAMR_NoMemory);
        return;
    }

    Permit();

    /*
     * Bounded, and short: the worker signals as the first thing it does,
     * before anything that can block, because `job` must be out of the
     * hand-over slot before this returns and a second request can use it.
     */
    Wait(SIGF_SINGLE);

    bsd_aam_boot_parent = NULL;
}

VOID bsd_BeginInterfaceConfig(register struct AddressAllocationMessage *aam __asm("a0"),
                              register struct AmiSocketBase *SocketBase __asm("a6"))
{
    NX_IP        *ip;
    NX_INTERFACE *nxif;
    LONG          index;

    (VOID)SocketBase;

    /* Nothing to reply through, so nothing can be reported. */
    if (aam == NULL)
        return;

    /*
     * "You must fill in this member or the message will be rejected." The
     * header's AAM_VERSION_MINIMUM is what makes a range rather than an
     * equality the right test -- see the note in
     * bsd_CreateAddrAllocMessageA().
     */
    if (aam->aam_Version < AAM_VERSION_MINIMUM ||
        aam->aam_Version > AAM_VERSION)
    {
        bsd_aam_reply(aam, AAMR_VersionUnknown);
        return;
    }

    ip = netstack_ip();
    if (ip == NULL)
    {
        bsd_aam_reply(aam, AAMR_InterfaceNotKnown);
        return;
    }

    /* aam_InterfaceName is a fixed sixteen bytes and need not be terminated
       by a caller that filled it in by hand, so it is bounded here. */
    if (aam->aam_InterfaceName[0] == '\0')
    {
        bsd_aam_reply(aam, AAMR_InterfaceNotKnown);
        return;
    }

    aam->aam_InterfaceName[sizeof(aam->aam_InterfaceName) - 1] = '\0';

    index = bsd_if_index_of(ip, aam->aam_InterfaceName);
    if (index < 0)
    {
        bsd_aam_reply(aam, AAMR_InterfaceNotKnown);
        return;
    }

    nxif = &ip->nx_ip_interface[index];

    /*
     * "The BOOTP protocol only works with broadcast interfaces (e.g. Ethernet
     * hardware). The interface you provided is not of this kind."
     *
     * NetX Duo records that as address mapping being needed -- an interface
     * that resolves addresses with ARP is one that broadcasts. An interface
     * with no SANA-II device behind it cannot broadcast either.
     */
    if (nxif->nx_interface_address_mapping_needed == 0 ||
        nxif->nx_interface_additional_link_info == NULL)
    {
        bsd_aam_reply(aam, AAMR_InterfaceWrongType);
        return;
    }

    /* "The interface already has an IP address assigned." */
    if (nxif->nx_interface_ip_address != 0)
    {
        bsd_aam_reply(aam, AAMR_AddressKnown);
        return;
    }

    /*
     * Everything about the request is in order. Only DHCP is actually run --
     * see the block comment above for what the other three would need -- and
     * the rest still get their message back, which is the whole difference
     * between this and a stub.
     */
    if (aam->aam_Protocol != AAMP_DHCP)
    {
        bsd_aam_reply(aam, AAMR_Ignored);
        return;
    }

    bsd_aam_launch(aam, (UWORD)index);
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
     * That is not a caveat here, it is the design. The flag is raised under
     * Forbid() and the worker reads it at the top of its next poll; if the
     * worker has already taken the job out of the table -- which it does
     * BEFORE it replies -- there is nothing to find and nothing to raise,
     * which is exactly the race the autodoc is describing. Setting a flag in
     * a job that is no longer listed would be writing into memory that is
     * about to be freed.
     *
     * A message that was never begun, or one already replied, finds nothing
     * and does nothing. Both are legal things for a caller to do.
     */
    Forbid();

    job = bsd_aam_find(aam);
    if (job != NULL)
        job->baj_Abort = TRUE;

    Permit();
}
