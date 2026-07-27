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

#include <proto/exec.h>

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
 * WHAT IS STILL NOT IMPLEMENTED, AND WHY IT IS AAMR_Ignored
 *
 * The allocation itself. All four protocols are refused with AAMR_Ignored --
 * "Your request was not understood and was therefore ignored", which is
 * literally what happens here:
 *
 *   AAMP_BOOTP                this stack has no BOOTP client. NetX Duo ships
 *                             DHCP; BOOTP is a different wire protocol and
 *                             nothing here speaks it.
 *   AAMP_DHCP                 the machinery exists -- NetX Duo's DHCP client
 *                             takes a per-interface start, and the netstack
 *                             already owns one with a state-change callback
 *                             wired up -- but the API is ASYNCHRONOUS with a
 *                             MANDATORY timeout ("the timeout must be at
 *                             least 10 seconds"), and there is nowhere yet to
 *                             hang the deadline that fires when a server
 *                             never answers. A request that binds would be
 *                             replied by the existing callback; one that does
 *                             not would never be replied at all, which is the
 *                             precise defect this file exists to remove.
 *                             Half of that is worse than none of it.
 *   AAMP_SLOWAUTO             RFC 3927 self-assignment. NX_AUTO_IP is in the
 *   AAMP_FASTAUTO             build and drives the LINKLOCAL configuration
 *                             type, so this is the same story as DHCP: the
 *                             protocol is there and the completion path is
 *                             not.
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
     * Everything about the request is in order and this stack still will not
     * run it. See the block comment above for which protocol fails for which
     * reason; the caller gets its message back either way, which is the whole
     * difference between this and a stub.
     */
    bsd_aam_reply(aam, AAMR_Ignored);
}

VOID bsd_AbortInterfaceConfig(register struct AddressAllocationMessage *aam __asm("a0"),
                              register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)aam;
    (VOID)SocketBase;

    /*
     * "There is no guarantee that the message can be intercepted and the IP
     * address allocation process aborted by this routine. In fact, the
     * process can complete before this routine has managed to abort it."
     *
     * Nothing here ever has an allocation in flight -- BeginInterfaceConfig()
     * replies before it returns -- so there is never anything to intercept,
     * and doing nothing is not a stub standing in for the real behaviour: it
     * IS the real behaviour, and it is one the published contract already
     * tells callers to expect. The message has been replied by the time this
     * can be called, and touching it now would be touching memory the caller
     * owns again.
     */
}
