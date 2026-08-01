/*
 * AamProbe -- the address allocation message.
 *
 * BeginInterfaceConfig() returns VOID and reports everything by filling in
 * aam_Result and replying the message, so an ENOSYS stub for it hangs rather
 * than refuses: it returns -1 in a register the caller cannot see and never
 * replies the message the caller is waiting on.  The central assertion is
 * therefore that after BeginInterfaceConfig() returns, the message is back on
 * the port.  The ten error codes CreateAddrAllocMessageA() enumerates, the
 * buffers it carves and the defaults it fills in are checked too.
 *
 * The probe calls DeleteAddrAllocMessage() on a message it built itself, on
 * the stack.  "This routine can only deallocate address allocation messages
 * created by CreateAddrAllocMessageA() and will not work with anything else"
 * -- so it has to be able to tell, and a library that could not would free a
 * stack frame here and take the machine with it.
 *
 * Vectors are called by hand at their LVOs, as in the other probes.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <utility/tagitem.h>

/* <libraries/bsdsocket.h> pulls in <sys/socket.h>, which uses size_t and
   ssize_t without declaring them. Same ordering note as ifprobe.c. */
#include <stddef.h>
#include <sys/types.h>
#include <libraries/bsdsocket.h>

#include <proto/exec.h>
#include <proto/dos.h>

/* ------------------------------------------------------------- vectors ---- */

static LONG p_create_aam(struct Library *base, LONG version, LONG protocol,
                         const char *name,
                         struct AddressAllocationMessage **result,
                         struct TagItem *tags)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = version;
    register LONG            d1  __asm("d1") = protocol;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)name;
    register APTR            a1  __asm("a1") = (APTR)result;
    register APTR            a2  __asm("a2") = (APTR)tags;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");
    register LONG _clob_a2 __asm("a2");

    __asm __volatile ("jsr a6@(-474:W)"     /* CreateAddrAllocMessageA -0x1da */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1), "=r" (_clob_a2)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (a0), "r" (a1),
                        "r" (a2)
                      : "cc", "memory");
    return res;
}

static VOID p_delete_aam(struct Library *base,
                         struct AddressAllocationMessage *aam)
{
    register struct Library *a6 __asm("a6") = base;
    register APTR            a0 __asm("a0") = (APTR)aam;
    register LONG _clob_d0 __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-480:W)"     /* DeleteAddrAllocMessage -0x1e0 */
                      : "=r" (_clob_d0), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "cc", "memory");
}

static VOID p_begin_config(struct Library *base,
                           struct AddressAllocationMessage *aam)
{
    register struct Library *a6 __asm("a6") = base;
    register APTR            a0 __asm("a0") = (APTR)aam;
    register LONG _clob_d0 __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-486:W)"     /* BeginInterfaceConfig -0x1e6 */
                      : "=r" (_clob_d0), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "cc", "memory");
}

static VOID p_abort_config(struct Library *base,
                           struct AddressAllocationMessage *aam)
{
    register struct Library *a6 __asm("a6") = base;
    register APTR            a0 __asm("a0") = (APTR)aam;
    register LONG _clob_d0 __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-492:W)"     /* AbortInterfaceConfig -0x1ec */
                      : "=r" (_clob_d0), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "cc", "memory");
}

static LONG p_add_interface(struct Library *base, const char *name,
                            const char *device, LONG unit,
                            struct TagItem *tags)
{
    register struct Library *a6  __asm("a6") = base;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)name;
    register CONST_APTR      a1  __asm("a1") = (CONST_APTR)device;
    register LONG            d0  __asm("d0") = unit;
    register APTR            a2  __asm("a2") = (APTR)tags;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");
    register LONG _clob_a2 __asm("a2");

    __asm __volatile ("jsr a6@(-444:W)"     /* AddInterfaceTagList -0x1bc */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1), "=r" (_clob_a2)
                      : "r" (a6), "r" (a0), "r" (a1), "r" (d0), "r" (a2)
                      : "cc", "memory");
    return res;
}

static LONG p_remove_interface(struct Library *base, const char *name,
                               LONG force)
{
    register struct Library *a6  __asm("a6") = base;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)name;
    register LONG            d0  __asm("d0") = force;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-732:W)"     /* RemoveInterface -0x2dc */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0), "r" (d0)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_configure_interface(struct Library *base, const char *name,
                                  struct TagItem *tags)
{
    register struct Library *a6  __asm("a6") = base;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)name;
    register APTR            a1  __asm("a1") = (APTR)tags;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-450:W)"     /* ConfigureInterfaceTagList -0x1c2 */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (a0), "r" (a1)
                      : "cc", "memory");
    return res;
}

static struct List *p_obtain_interface_list(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register struct List    *res __asm("d0");

    __asm __volatile ("jsr a6@(-462:W)"     /* ObtainInterfaceList -0x1ce */
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static VOID p_release_interface_list(struct Library *base, struct List *list)
{
    register struct Library *a6 __asm("a6") = base;
    register struct List    *a0 __asm("a0") = list;
    register LONG _clob_d0 __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-456:W)"     /* ReleaseInterfaceList -0x1c8 */
                      : "=r" (_clob_d0), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "cc", "memory");
}

/* ----------------------------------------------------------------- helpers */

static VOID p_zero(APTR p, ULONG n)
{
    UBYTE *b = (UBYTE *)p;

    while (n-- != 0)
        *b++ = 0;
}

/*
 * The message, replied or not.  Everything BeginInterfaceConfig() reports goes
 * through this: a result code, and the message being back where the caller can
 * pick it up.
 */
static VOID p_begin_and_collect(struct Library *base, struct MsgPort *port,
                                struct AddressAllocationMessage *aam,
                                const char *what, LONG expect)
{
    struct Message *got;

    p_begin_config(base, aam);

    got = GetMsg(port);

    Printf((CONST_STRPTR)"begin %s: result %ld, %s%s\n",
           (LONG)what, aam->aam_Result,
           (LONG)((got == &aam->aam_Message) ? "replied"
                : (got == NULL)              ? "NOT REPLIED"
                                             : "replied SOMETHING ELSE"),
           (LONG)((got == &aam->aam_Message && aam->aam_Result == expect)
                      ? " -- correctly" : " -- WRONG"));
}

/* ------------------------------------------------------- the second caller */

/*
 * "AAMR_Busy -- Address allocation is already in progress for this interface."
 *
 * bsd_aam_launch() claims bsd_aam_jobs[index] under Forbid() and answers
 * AAMR_Busy to a launch that finds it taken.  That guard cannot be reached
 * from one Process: BeginInterfaceConfig() is asynchronous, but the caller
 * that already owns the interface is the one holding the job, so a second call
 * has to come from somewhere else while the first is still in flight.
 *
 * So this is a Process of its own with a bsdsocket.library base of its own --
 * an unrelated program asking for the same interface, which is the case the
 * autodoc's AAMR_Busy exists for.  It prints nothing (NP_Cli FALSE, no
 * Output()); everything it saw goes back in the block below and the parent
 * reports it.
 *
 * The interface it asks about is the one the parent has a DHCP worker running
 * on, with no address and the link down, so every check ahead of the guard --
 * version, name, broadcast type, address not already known, protocol -- passes
 * for it exactly as it did for the parent.  A result that is not AAMR_Busy is
 * therefore the guard, and not one of those.
 *
 * WHY THE RESULT CODE ALONE IS NOT THE ASSERTION
 *
 *   AAMR_Busy is answered in two places, and only one of them is the guard.
 *   bsd_aam_launch() refuses at the door.  If it did not, the worker would be
 *   created, and netstack_interface_dhcp_start() would then refuse the SECOND
 *   client on an interface that already has one -- AMI_NET_ERR_BUSY, which
 *   bsd_aam_worker() also reports as AAMR_Busy.  Deleting the guard therefore
 *   still produces AAMR_Busy, measured, so a probe that read only the result
 *   code would pass a build with no guard in it at all.
 *
 *   What separates them is WHEN.  A refusal is replied inside
 *   BeginInterfaceConfig(), so the message is on the port before the call
 *   returns.  The other answer costs a CreateNewProc(), a PutMsg() and a
 *   worker getting as far as its first DHCP call -- it is never there yet.  So
 *   the first GetMsg() is the assertion and the result code only says which
 *   refusal it was.
 */
static struct
{
    struct Task *sc_Parent;
    ULONG        sc_Signal;
    char         sc_Interface[16];
    LONG         sc_OpenFailed;
    LONG         sc_PortFailed;
    LONG         sc_CreateRc;
    LONG         sc_Result;
    LONG         sc_Replied;
    LONG         sc_AtOnce;     /* back before BeginInterfaceConfig returned */
} second;

static VOID p_second_caller(VOID)
{
    struct Library                  *sb;
    struct MsgPort                  *port;
    struct AddressAllocationMessage *aam = NULL;
    struct Message                  *got;
    struct TagItem                   tags[2];

    second.sc_Result  = AAMR_Ignored;
    second.sc_Replied = 0;

    sb = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (sb == NULL)
    {
        second.sc_OpenFailed = 1;
    }
    else
    {
        port = CreateMsgPort();
        if (port == NULL)
        {
            second.sc_PortFailed = 1;
        }
        else
        {
            tags[0].ti_Tag  = CAAMTA_ReplyPort;
            tags[0].ti_Data = (ULONG)port;
            tags[1].ti_Tag  = TAG_DONE;
            tags[1].ti_Data = 0;

            second.sc_CreateRc = p_create_aam(sb, AAM_VERSION, AAMP_DHCP,
                                              second.sc_Interface, &aam, tags);

            if (second.sc_CreateRc == CAAME_Success && aam != NULL)
            {
                aam->aam_Result = AAMR_Ignored;

                p_begin_config(sb, aam);

                /*
                 * This GetMsg() is the assertion -- see the note above the
                 * block.  A refusal is replied inside the call; a worker that
                 * was allowed to start cannot have got anywhere yet.
                 */
                got = GetMsg(port);
                second.sc_AtOnce = (got == &aam->aam_Message) ? 1 : 0;

                /* And then long enough for the other answer to arrive, so that
                   a build with no guard is reported rather than hung on. */
                if (got == NULL)
                {
                    ULONG waited;

                    for (waited = 0; got == NULL && waited < 15UL * 50UL;
                         waited += 5)
                    {
                        Delay(5);
                        got = GetMsg(port);
                    }
                }

                second.sc_Replied = (got == &aam->aam_Message) ? 1 : 0;
                second.sc_Result  = aam->aam_Result;

                /*
                 * A worker that really started is still holding this message.
                 * Abort it rather than free it under the worker's feet: the
                 * broken build is the one that gets here, and it must not take
                 * the machine down before the parent can report why.
                 */
                if (second.sc_Replied == 0)
                {
                    p_abort_config(sb, aam);
                    for (;;)
                    {
                        got = GetMsg(port);
                        if (got != NULL)
                            break;
                        Delay(10);
                    }
                }

                p_delete_aam(sb, aam);
            }

            DeleteMsgPort(port);
        }

        CloseLibrary(sb);
    }

    Signal(second.sc_Parent, second.sc_Signal);
}

/*
 * Run it, and say what it was told.  Called with a DHCP worker in flight on
 * the same interface: the only correct answer is AAMR_Busy, replied.
 */
static VOID p_ask_again_from_another_process(const char *ifname)
{
    struct Process *proc;
    struct TagItem  tags[6];
    BYTE            sig;
    ULONG           i;

    for (i = 0; i + 1 < sizeof(second.sc_Interface) && ifname[i] != '\0'; i++)
        second.sc_Interface[i] = ifname[i];
    second.sc_Interface[i] = '\0';

    sig = AllocSignal(-1);
    if (sig == -1)
    {
        Printf((CONST_STRPTR)"busy: no signal for the handshake -- NOT TESTED\n");
        return;
    }

    second.sc_Parent = FindTask(NULL);
    second.sc_Signal = 1UL << sig;
    SetSignal(0UL, second.sc_Signal);

    tags[0].ti_Tag  = NP_Entry;
    tags[0].ti_Data = (ULONG)p_second_caller;
    tags[1].ti_Tag  = NP_Name;
    tags[1].ti_Data = (ULONG)"AamProbe second caller";
    tags[2].ti_Tag  = NP_StackSize;
    tags[2].ti_Data = 8192;
    tags[3].ti_Tag  = NP_Priority;
    tags[3].ti_Data = 0;
    tags[4].ti_Tag  = NP_Cli;
    tags[4].ti_Data = FALSE;
    tags[5].ti_Tag  = TAG_DONE;
    tags[5].ti_Data = 0;

    proc = CreateNewProc(tags);
    if (proc == NULL)
    {
        Printf((CONST_STRPTR)"busy: CreateNewProc failed -- NOT TESTED\n");
        FreeSignal(sig);
        return;
    }

    Wait(second.sc_Signal);
    FreeSignal(sig);

    if (second.sc_OpenFailed != 0)
    {
        Printf((CONST_STRPTR)"busy: the second process got no base -- NOT TESTED\n");
        return;
    }
    if (second.sc_PortFailed != 0)
    {
        Printf((CONST_STRPTR)"busy: the second process got no port -- NOT TESTED\n");
        return;
    }
    if (second.sc_CreateRc != CAAME_Success)
    {
        Printf((CONST_STRPTR)"busy: the second process could not build a "
                             "message (%ld) -- NOT TESTED\n",
               second.sc_CreateRc);
        return;
    }

    Printf((CONST_STRPTR)"busy: a second process asked for %s while the first "
                         "allocation was running: result %ld, %s%s\n",
           (LONG)second.sc_Interface, second.sc_Result,
           (LONG)(second.sc_AtOnce  ? "replied before the call returned"
                : second.sc_Replied ? "REPLIED LATE, so a worker was started"
                                    : "NOT REPLIED"),
           (LONG)((second.sc_AtOnce && second.sc_Result == AAMR_Busy)
                      ? " -- refused at the door with AAMR_Busy, correctly"
                      : " -- WRONG, the second request was not refused where "
                        "it had to be"));
}

/* ------------------------------------------------------------------ main -- */

#define PROBE_ROUTERS       4
#define PROBE_DNS           3
#define PROBE_STATIC        2
#define PROBE_NAK           64
#define PROBE_HOSTNAME      32
#define PROBE_DOMAIN        48
#define PROBE_BOOTP         300

int main(void)
{
    struct Library                  *base;
    struct MsgPort                  *port;
    struct AddressAllocationMessage *aam;
    struct AddressAllocationMessage  by_hand;
    struct List                     *list;
    struct Node                     *node;
    struct TagItem                   tags[16];
    char                             ifname[16];
    char                             too_long[300];
    LONG                             rc;
    ULONG                            i;
    ULONG                            n;

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (base == NULL)
    {
        Printf((CONST_STRPTR)"AamProbe: no bsdsocket.library\n");
        return RETURN_FAIL;
    }

    port = CreateMsgPort();
    if (port == NULL)
    {
        Printf((CONST_STRPTR)"AamProbe: no message port\n");
        CloseLibrary(base);
        return RETURN_FAIL;
    }

    /* The name of the first interface, whatever this machine calls it. */
    ifname[0] = '\0';
    list = p_obtain_interface_list(base);
    if (list != NULL)
    {
        for (node = list->lh_Head; node->ln_Succ != NULL; node = node->ln_Succ)
        {
            if (node->ln_Name != NULL && ifname[0] == '\0')
            {
                for (i = 0; i + 1 < sizeof(ifname) && node->ln_Name[i] != '\0';
                     i++)
                    ifname[i] = node->ln_Name[i];
                ifname[i] = '\0';
            }
        }
        p_release_interface_list(base, list);
    }

    Printf((CONST_STRPTR)"interface: %s\n",
           (LONG)(ifname[0] != '\0' ? ifname : "(none)"));

    if (ifname[0] == '\0')
    {
        Printf((CONST_STRPTR)"AamProbe: no interface to work with\n");
        DeleteMsgPort(port);
        CloseLibrary(base);
        return RETURN_FAIL;
    }

    tags[0].ti_Tag = TAG_DONE;
    tags[0].ti_Data = 0;

    /* ---- every error the autodoc enumerates ------------------------------ */

    rc = p_create_aam(base, AAM_VERSION, AAMP_DHCP, ifname, NULL, tags);
    Printf((CONST_STRPTR)"create with no result ptr: %ld%s\n", rc,
           (LONG)((rc == CAAME_Invalid_result_ptr) ? " -- correctly" : " -- WRONG"));

    aam = (struct AddressAllocationMessage *)0x12345678UL;
    rc = p_create_aam(base, 99, AAMP_DHCP, ifname, &aam, tags);
    Printf((CONST_STRPTR)"create with version 99: %ld, ptr %s%s\n", rc,
           (LONG)((aam == NULL) ? "cleared" : "LEFT DIRTY"),
           (LONG)((rc == CAAME_Invalid_version && aam == NULL)
                      ? " -- correctly" : " -- WRONG"));

    rc = p_create_aam(base, AAM_VERSION, 77, ifname, &aam, tags);
    Printf((CONST_STRPTR)"create with protocol 77: %ld%s\n", rc,
           (LONG)((rc == CAAME_Invalid_protocol) ? " -- correctly" : " -- WRONG"));

    rc = p_create_aam(base, AAM_VERSION, AAMP_DHCP, "", &aam, tags);
    Printf((CONST_STRPTR)"create with an empty name: %ld%s\n", rc,
           (LONG)((rc == CAAME_Invalid_interface_name) ? " -- correctly"
                                                       : " -- WRONG"));

    rc = p_create_aam(base, AAM_VERSION, AAMP_DHCP, "nosuchif", &aam, tags);
    Printf((CONST_STRPTR)"create for an unknown interface: %ld%s\n", rc,
           (LONG)((rc == CAAME_Interface_not_found) ? " -- correctly"
                                                    : " -- WRONG"));

    tags[0].ti_Tag  = CAAMTA_ClientIdentifier;
    tags[0].ti_Data = (ULONG)"x";
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;
    rc = p_create_aam(base, AAM_VERSION, AAMP_DHCP, ifname, &aam, tags);
    Printf((CONST_STRPTR)"create with a 1-character client id: %ld%s\n", rc,
           (LONG)((rc == CAAME_Client_identifier_too_short) ? " -- correctly"
                                                           : " -- WRONG"));

    for (i = 0; i < sizeof(too_long) - 1; i++)
        too_long[i] = 'a';
    too_long[sizeof(too_long) - 1] = '\0';

    tags[0].ti_Data = (ULONG)too_long;
    rc = p_create_aam(base, AAM_VERSION, AAMP_DHCP, ifname, &aam, tags);
    Printf((CONST_STRPTR)"create with a 299-character client id: %ld%s\n", rc,
           (LONG)((rc == CAAME_Client_identifier_too_long) ? " -- correctly"
                                                          : " -- WRONG"));

    /* ---- and one that works, with every buffer asked for ------------------ */

    tags[0].ti_Tag  = CAAMTA_Timeout;
    tags[0].ti_Data = 3;                        /* below the minimum */
    tags[1].ti_Tag  = CAAMTA_ClientIdentifier;
    tags[1].ti_Data = (ULONG)"amiga-probe";
    tags[2].ti_Tag  = CAAMTA_NAKMessageSize;
    tags[2].ti_Data = PROBE_NAK;
    tags[3].ti_Tag  = CAAMTA_RouterTableSize;
    tags[3].ti_Data = PROBE_ROUTERS;
    tags[4].ti_Tag  = CAAMTA_DNSTableSize;
    tags[4].ti_Data = PROBE_DNS;
    tags[5].ti_Tag  = CAAMTA_StaticRouteTableSize;
    tags[5].ti_Data = PROBE_STATIC;
    tags[6].ti_Tag  = CAAMTA_HostNameSize;
    tags[6].ti_Data = PROBE_HOSTNAME;
    tags[7].ti_Tag  = CAAMTA_DomainNameSize;
    tags[7].ti_Data = PROBE_DOMAIN;
    tags[8].ti_Tag  = CAAMTA_BOOTPMessageSize;
    tags[8].ti_Data = PROBE_BOOTP;
    tags[9].ti_Tag  = CAAMTA_RecordLeaseExpiration;
    tags[9].ti_Data = TRUE;
    tags[10].ti_Tag  = CAAMTA_ReplyPort;
    tags[10].ti_Data = (ULONG)port;
    tags[11].ti_Tag  = CAAMTA_RequestUnicast;
    tags[11].ti_Data = TRUE;
    tags[12].ti_Tag  = TAG_DONE;
    tags[12].ti_Data = 0;

    aam = NULL;
    rc  = p_create_aam(base, AAM_VERSION, AAMP_DHCP, ifname, &aam, tags);
    Printf((CONST_STRPTR)"create with every buffer: %ld, message %s\n", rc,
           (LONG)((aam != NULL) ? "allocated" : "NULL"));

    if (rc != CAAME_Success || aam == NULL)
    {
        Printf((CONST_STRPTR)"AamProbe: cannot go on without a message\n");
        DeleteMsgPort(port);
        CloseLibrary(base);
        return RETURN_FAIL;
    }

    /* "the timeout must be at least 10 seconds long. If it is shorter, it is
       automatically extended to 10 seconds" -- extended, not refused. */
    Printf((CONST_STRPTR)"timeout asked 3, got %ld%s\n", aam->aam_Timeout,
           (LONG)((aam->aam_Timeout == AAM_TIMEOUT_MIN) ? " -- extended, correctly"
                                                        : " -- WRONG"));

    Printf((CONST_STRPTR)"reply port %s, mn_Length %ld%s\n",
           (LONG)((aam->aam_Message.mn_ReplyPort == port) ? "set" : "WRONG"),
           (LONG)aam->aam_Message.mn_Length,
           (LONG)((aam->aam_Message.mn_ReplyPort == port &&
                   aam->aam_Message.mn_Length == (UWORD)sizeof(*aam))
                      ? " -- correctly" : " -- WRONG"));

    Printf((CONST_STRPTR)"client id '%s'%s\n",
           (LONG)((aam->aam_ClientIdentifier != NULL)
                      ? (const char *)aam->aam_ClientIdentifier : "(none)"),
           (LONG)((aam->aam_ClientIdentifier != NULL &&
                   aam->aam_ClientIdentifier[0] == 'a')
                      ? " -- duplicated, correctly" : " -- WRONG"));

    Printf((CONST_STRPTR)"sizes: nak %ld routers %ld dns %ld static %ld "
                         "host %ld domain %ld bootp %ld\n",
           aam->aam_NAKMessageSize, aam->aam_RouterTableSize,
           aam->aam_DNSTableSize, aam->aam_StaticRouteTableSize,
           aam->aam_HostNameSize, aam->aam_DomainNameSize,
           aam->aam_BOOTPMessageSize);

    /*
     * Every buffer present, longword-aligned and distinct.  Two of them are
     * arrays of ULONG, and an m68k handed a misaligned one takes an address
     * error.
     */
    {
        APTR  bufs[9];
        BOOL  ok = TRUE;

        bufs[0] = aam->aam_NAKMessage;
        bufs[1] = aam->aam_RouterTable;
        bufs[2] = aam->aam_DNSTable;
        bufs[3] = aam->aam_StaticRouteTable;
        bufs[4] = aam->aam_HostName;
        bufs[5] = aam->aam_DomainName;
        bufs[6] = aam->aam_BOOTPMessage;
        bufs[7] = aam->aam_LeaseExpires;
        bufs[8] = aam->aam_ClientIdentifier;

        for (i = 0; i < 9; i++)
        {
            if (bufs[i] == NULL)
                ok = FALSE;
            if (((ULONG)bufs[i] & 3UL) != 0)
                ok = FALSE;

            for (n = 0; n < i; n++)
            {
                if (bufs[i] == bufs[n])
                    ok = FALSE;
            }
        }

        Printf((CONST_STRPTR)"buffers: %s\n",
               (LONG)(ok ? "all present, aligned and distinct -- correctly"
                         : "MISSING, MISALIGNED OR SHARED"));
    }

    /* Zeroed: a caller reads these after the reply and must not see rubbish. */
    {
        BOOL zeroed = TRUE;

        for (i = 0; i < (ULONG)aam->aam_RouterTableSize; i++)
        {
            if (aam->aam_RouterTable[i] != 0)
                zeroed = FALSE;
        }
        for (i = 0; i < (ULONG)aam->aam_BOOTPMessageSize; i++)
        {
            if (aam->aam_BOOTPMessage[i] != 0)
                zeroed = FALSE;
        }

        Printf((CONST_STRPTR)"buffers zeroed: %s\n",
               (LONG)(zeroed ? "yes -- correctly" : "NO"));
    }

    Printf((CONST_STRPTR)"unicast %ld%s\n", (LONG)aam->aam_Unicast,
           (LONG)((aam->aam_Unicast != 0) ? " -- honoured at version 2, correctly"
                                          : " -- WRONG"));

    /* ---- BeginInterfaceConfig, and the reply that has to come back -------- */

    /*
     * This interface already has an address, so the documented answer is
     * AAMR_AddressKnown.  The assertion is that the message came back: a stub
     * that returns -1 in d0 leaves a caller waiting on this port forever,
     * because the call returns VOID and nothing else can tell it.
     */
    p_begin_and_collect(base, port, aam, "on an addressed interface",
                        AAMR_AddressKnown);

    /* An interface name nothing answers to. */
    aam->aam_InterfaceName[0] = 'z';
    aam->aam_InterfaceName[1] = 'z';
    aam->aam_InterfaceName[2] = 'z';
    aam->aam_InterfaceName[3] = '\0';
    p_begin_and_collect(base, port, aam, "on an unknown interface",
                        AAMR_InterfaceNotKnown);

    /* A version this library does not support. */
    aam->aam_Version = 99;
    p_begin_and_collect(base, port, aam, "with a bad version",
                        AAMR_VersionUnknown);

    /* Documented to be safe whether or not anything is in flight. */
    p_abort_config(base, aam);
    Printf((CONST_STRPTR)"AbortInterfaceConfig: returned\n");

    p_abort_config(base, NULL);
    Printf((CONST_STRPTR)"AbortInterfaceConfig(NULL): returned\n");

    /* ---- and now a real allocation ----------------------------------------
     *
     * The interface this run is riding on already has an address, so the only
     * way to ask for one is to take it away first.  RemoveInterface() and
     * AddInterfaceTagList() do that -- "permitting it to be added again with
     * new parameters" -- and an interface added that way arrives with no
     * address at all, which is the state BeginInterfaceConfig() is for.
     *
     * SLIRP runs a DHCP server, so this is a real DISCOVER/OFFER/REQUEST/ACK
     * on the wire and the address that comes back is one a server chose.
     */
    {
        struct AddressAllocationMessage *live = NULL;
        struct Message                  *got;
        struct TagItem                   ltags[8];
        ULONG                            waited;

        rc = p_remove_interface(base, ifname, 0);
        Printf((CONST_STRPTR)"live: remove %s: rc %ld\n", (LONG)ifname, rc);

        rc = p_add_interface(base, ifname, "a2065.device", 0, NULL);
        Printf((CONST_STRPTR)"live: add %s: rc %ld\n", (LONG)ifname, rc);

        ltags[0].ti_Tag  = CAAMTA_ReplyPort;
        ltags[0].ti_Data = (ULONG)port;
        ltags[1].ti_Tag  = CAAMTA_RouterTableSize;
        ltags[1].ti_Data = PROBE_ROUTERS;
        ltags[2].ti_Tag  = CAAMTA_DNSTableSize;
        ltags[2].ti_Data = PROBE_DNS;
        ltags[3].ti_Tag  = CAAMTA_HostNameSize;
        ltags[3].ti_Data = PROBE_HOSTNAME;
        ltags[4].ti_Tag  = CAAMTA_DomainNameSize;
        ltags[4].ti_Data = PROBE_DOMAIN;
        ltags[5].ti_Tag  = CAAMTA_RecordLeaseExpiration;
        ltags[5].ti_Data = TRUE;
        ltags[6].ti_Tag  = TAG_DONE;
        ltags[6].ti_Data = 0;

        rc = p_create_aam(base, AAM_VERSION, AAMP_DHCP, ifname, &live, ltags);

        if (rc == CAAME_Success && live != NULL)
        {
            /*
             * The call must return promptly -- "This routine starts an
             * asynchronous operation, very much like exec.library/SendIO()".
             * A synchronous implementation would sit here for the whole
             * ten-second timeout, so the message must not be on the port yet.
             */
            p_begin_config(base, live);

            got = GetMsg(port);
            Printf((CONST_STRPTR)"live: begin returned with the message %s%s\n",
                   (LONG)((got == NULL) ? "still out" : "ALREADY BACK"),
                   (LONG)((got == NULL) ? " -- asynchronous, correctly"
                                        : " -- SYNCHRONOUS, WRONG"));

            /* Wait for it, with a bound of our own well past the timeout. */
            for (waited = 0; got == NULL && waited < 30UL * 50UL; waited += 10)
            {
                Delay(10);
                got = GetMsg(port);
            }

            if (got == &live->aam_Message)
            {
                Printf((CONST_STRPTR)"live: replied after ~%ld ticks, "
                                     "result %ld%s\n",
                       (LONG)waited, live->aam_Result,
                       (LONG)((live->aam_Result == AAMR_Success)
                                  ? " -- AAMR_Success, correctly" : " -- WRONG"));

                Printf((CONST_STRPTR)"live: address %ld.%ld.%ld.%ld "
                                     "mask %ld.%ld.%ld.%ld server %ld.%ld.%ld.%ld\n",
                       (LONG)((live->aam_Address >> 24) & 0xFF),
                       (LONG)((live->aam_Address >> 16) & 0xFF),
                       (LONG)((live->aam_Address >> 8) & 0xFF),
                       (LONG)(live->aam_Address & 0xFF),
                       (LONG)((live->aam_SubnetMask >> 24) & 0xFF),
                       (LONG)((live->aam_SubnetMask >> 16) & 0xFF),
                       (LONG)((live->aam_SubnetMask >> 8) & 0xFF),
                       (LONG)(live->aam_SubnetMask & 0xFF),
                       (LONG)((live->aam_ServerAddress >> 24) & 0xFF),
                       (LONG)((live->aam_ServerAddress >> 16) & 0xFF),
                       (LONG)((live->aam_ServerAddress >> 8) & 0xFF),
                       (LONG)(live->aam_ServerAddress & 0xFF));

                Printf((CONST_STRPTR)"live: address is %s\n",
                       (LONG)((live->aam_Address != 0)
                                  ? "not zero -- a server really answered"
                                  : "ZERO, WRONG"));

                if (live->aam_RouterTable != NULL)
                    Printf((CONST_STRPTR)"live: router[0] %ld.%ld.%ld.%ld%s\n",
                           (LONG)((live->aam_RouterTable[0] >> 24) & 0xFF),
                           (LONG)((live->aam_RouterTable[0] >> 16) & 0xFF),
                           (LONG)((live->aam_RouterTable[0] >> 8) & 0xFF),
                           (LONG)(live->aam_RouterTable[0] & 0xFF),
                           (LONG)((live->aam_RouterTable[0] != 0)
                                      ? " -- the server offered one"
                                      : " -- none offered"));

                if (live->aam_LeaseExpires != NULL)
                    Printf((CONST_STRPTR)"live: lease expires day %ld "
                                         "minute %ld\n",
                           live->aam_LeaseExpires->ds_Days,
                           live->aam_LeaseExpires->ds_Minute);
            }
            else
            {
                Printf((CONST_STRPTR)"live: NOT REPLIED after %ld ticks\n",
                       (LONG)waited);
            }

            /*
             * A second allocation on an interface that now has one: the
             * documented answer is AAMR_AddressKnown, which shows the first
             * one configured the interface rather than only reporting an
             * address.
             */
            live->aam_Result = AAMR_Ignored;
            p_begin_and_collect(base, port, live, "a second time",
                                AAMR_AddressKnown);

            p_delete_aam(base, live);
        }
        else
        {
            Printf((CONST_STRPTR)"live: could not build a message (%ld)\n", rc);
        }
    }

    /* ---- the two paths a server that answers can never reach ---------------
     *
     * AAMR_Timeout and AAMR_Aborted are the halves of BeginInterfaceConfig()
     * that a working DHCP server hides: SLIRP answers in about four tenths of
     * a second, so neither the deadline nor the abort window ever opens.
     *
     * Taking the interface down first opens both: nothing can leave the card,
     * so DISCOVER goes unanswered and the worker runs to its own deadline.
     * That is the only path that exercises the deadline, and the only one that
     * exercises releasing a lease that was never granted.
     */
    {
        struct AddressAllocationMessage *slow = NULL;
        struct Message                  *got;
        struct TagItem                   stags[4];
        ULONG                            waited;

        rc = p_remove_interface(base, ifname, 0);
        Printf((CONST_STRPTR)"slow: remove %s: rc %ld\n", (LONG)ifname, rc);

        rc = p_add_interface(base, ifname, "a2065.device", 0, NULL);
        Printf((CONST_STRPTR)"slow: add %s: rc %ld\n", (LONG)ifname, rc);

        stags[0].ti_Tag  = IFC_State;
        stags[0].ti_Data = SM_Down;
        stags[1].ti_Tag  = TAG_DONE;
        stags[1].ti_Data = 0;

        rc = p_configure_interface(base, ifname, stags);
        Printf((CONST_STRPTR)"slow: take %s down: rc %ld\n", (LONG)ifname, rc);

        /* ---- aborted ------------------------------------------------------ */

        stags[0].ti_Tag  = CAAMTA_ReplyPort;
        stags[0].ti_Data = (ULONG)port;
        stags[1].ti_Tag  = TAG_DONE;
        stags[1].ti_Data = 0;

        rc = p_create_aam(base, AAM_VERSION, AAMP_DHCP, ifname, &slow, stags);

        if (rc == CAAME_Success && slow != NULL)
        {
            p_begin_config(base, slow);

            /* Long enough that the worker is certainly in its poll loop, and
               far short of the ten-second floor. */
            Delay(50);

            got = GetMsg(port);
            Printf((CONST_STRPTR)"slow: still running after a second: %s%s\n",
                   (LONG)((got == NULL) ? "yes" : "NO, already replied"),
                   (LONG)((got == NULL) ? " -- correctly" : " -- WRONG"));

            /*
             * The only window in this file in which a worker is certainly
             * running: the interface is down, so DISCOVER goes unanswered and
             * the job sits in bsd_aam_jobs[] until the ten-second floor.  What
             * a second caller is told during it is the AAMR_Busy guard, and
             * nothing else in this probe reaches it.
             */
            if (got == NULL)
                p_ask_again_from_another_process(ifname);

            p_abort_config(base, slow);

            for (waited = 0; got == NULL && waited < 5UL * 50UL; waited += 5)
            {
                Delay(5);
                got = GetMsg(port);
            }

            Printf((CONST_STRPTR)"slow: abort replied after ~%ld ticks, "
                                 "result %ld%s\n",
                   (LONG)waited, slow->aam_Result,
                   (LONG)((got == &slow->aam_Message &&
                           slow->aam_Result == AAMR_Aborted)
                              ? " -- AAMR_Aborted, correctly" : " -- WRONG"));

            /* ---- and timed out -------------------------------------------- */

            slow->aam_Result = AAMR_Ignored;
            p_begin_config(base, slow);

            /* The floor is ten seconds; wait half again as long before
               calling it a failure to reply at all. */
            got = NULL;
            for (waited = 0; got == NULL && waited < 15UL * 50UL; waited += 10)
            {
                Delay(10);
                got = GetMsg(port);
            }

            Printf((CONST_STRPTR)"slow: timeout replied after ~%ld ticks, "
                                 "result %ld%s\n",
                   (LONG)waited, slow->aam_Result,
                   (LONG)((got == &slow->aam_Message &&
                           slow->aam_Result == AAMR_Timeout)
                              ? " -- AAMR_Timeout, correctly" : " -- WRONG"));

            /*
             * It waited at least the floor.  A worker that gave up early would
             * report AAMR_Timeout too and be wrong; the elapsed time is what
             * shows the deadline is the one the caller asked for.
             */
            Printf((CONST_STRPTR)"slow: waited %s the 10-second floor\n",
                   (LONG)((waited >= 10UL * 50UL) ? "at least -- correctly"
                                                  : "LESS THAN, WRONG"));

            p_delete_aam(base, slow);
        }
        else
        {
            Printf((CONST_STRPTR)"slow: could not build a message (%ld)\n", rc);
        }

        /* Put the machine back the way it was found. */
        stags[0].ti_Tag  = IFC_State;
        stags[0].ti_Data = SM_Online;
        stags[1].ti_Tag  = TAG_DONE;
        stags[1].ti_Data = 0;
        (VOID)p_configure_interface(base, ifname, stags);
    }

    /* ---- and give it back ------------------------------------------------- */

    p_delete_aam(base, aam);
    Printf((CONST_STRPTR)"DeleteAddrAllocMessage: returned\n");

    p_delete_aam(base, NULL);
    Printf((CONST_STRPTR)"DeleteAddrAllocMessage(NULL): returned\n");

    /*
     * A message this library did not allocate, on the stack.  "This routine
     * can only deallocate address allocation messages created by
     * CreateAddrAllocMessageA() and will not work with anything else" -- a
     * library that could not tell would free a stack frame, and the machine
     * would not survive the next allocation.
     */
    p_zero(&by_hand, sizeof(by_hand));
    by_hand.aam_Version  = AAM_VERSION;
    by_hand.aam_Protocol = AAMP_DHCP;

    p_delete_aam(base, &by_hand);
    Printf((CONST_STRPTR)"DeleteAddrAllocMessage on a stack message: "
                         "returned, version still %ld%s\n",
           by_hand.aam_Version,
           (LONG)((by_hand.aam_Version == AAM_VERSION)
                      ? " -- refused, correctly" : " -- WRONG"));

    DeleteMsgPort(port);
    CloseLibrary(base);

    return RETURN_OK;
}
