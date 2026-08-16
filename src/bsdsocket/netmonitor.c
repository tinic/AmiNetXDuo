/*
 * bsdsocket.library, the network monitoring hooks.
 *
 *   AddNetMonitorHookTagList()  install a callback
 * "Monitoring hooks can be used both for inspecting and filtering data that
 * enters the stack, or for denying access to certain APIs. More than one hook
 * can be installed for each monitoring task."
 *
 * Same primary source as interfaces.c, routing.c and netstats.c: NDK 3.2's
 * SANA+RoadshowTCP-IP/doc/bsdsocket.doc, plus libraries/bsdsocket.h from the
 * same NDK, used as an ABI reference only. No Roadshow, AmiTCP, AROSTCP or
 * Miami code was consulted or is present.
 *
 *   The hook uses the standard Amiga hook register convention, but the middle
 *   argument is not the usual object: "action = hookfunc(hook, reserved, imm)"
 *   in A0, A2, A1, with "the 'reserved' parameter will be set to NULL for
 *   future expansion". A0/A1 alone is a guess, and anything but NULL in A2
 *   becomes an ABI promise.
 *
 *   The two families answer with different vocabularies:
 *
 *     the in-stack types  return MA_Continue / MA_Ignore / MA_Drop /
 *     (ICMP, UDP,         MA_DropWithReset, what to do with a packet.
 *      TCP_Connect,       "Any positive return value will cause the data to
 *      Packet)            be dropped and the corresponding errno value to be
 *                        set to the result."
 *
 *     the call-site types return an ERRNO, "An error value of 0 will allow
 *     (Connect, Bind,     the call to proceed (unless another hook denies
 *      Send)              this), and any error value > 0 will cause the call
 *                        to be aborted and the errno variable to be set to
 *                        this value."
 *
 *   So 0 means "continue" in both, and everything else means something
 *   different in each. MA_Continue is 0, which is what lets one dispatcher
 *   serve both.
 *
 *   RemoveNetMonitorHook() takes only the hook, no type, so removal
 *   searches every list. One hook cannot be installed for two types at once,
 *   because nothing then says which one to remove.
 *
 * The three call-site types are implemented: MHT_Connect, MHT_Bind and
 * MHT_Send. The four in-stack types are refused with EINVAL, the documented
 * error for an unsupported type. Nothing dispatches them, and a hook that is
 * installed but never called looks the same as a quiet network. The details
 * are at the check in bsd_AddNetMonitorHookTagList().
 *
 * "It must be called before the library is closed, or the library will stay
 * in memory indefinitely." bsd_lib_expunge() implements that by declining
 * while bsd_netmon_busy() is true. An expunge with hooks installed unloads the
 * segment under a caller that still believes its hook is live.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "netmonitor.h"

#include <proto/exec.h>

/*
 * One list per type. MHT_ICMP is 0 and MHT_Bind is 6, so the type doubles as
 * the index. The _Static_asserts below check that, because a renumbering
 * otherwise files hooks under the wrong kind.
 */
_Static_assert(MHT_ICMP        == 0, "MHT_* numbering is the list index");
_Static_assert(MHT_UDP         == 1, "MHT_* numbering is the list index");
_Static_assert(MHT_TCP_Connect == 2, "MHT_* numbering is the list index");
_Static_assert(MHT_Connect     == 3, "MHT_* numbering is the list index");
_Static_assert(MHT_Send        == 4, "MHT_* numbering is the list index");
_Static_assert(MHT_Packet      == 5, "MHT_* numbering is the list index");
_Static_assert(MHT_Bind        == 6, "MHT_* numbering is the list index");

#define BSD_MHT_COUNT   (MHT_Bind + 1)

static struct MinList bsd_mon_list[BSD_MHT_COUNT];
static BOOL           bsd_mon_ready;
static LONG           bsd_mon_count;

/*
 * The lists are walked from the SANA-II receive path and from socket calls on
 * any task. A hook install mutates them. Forbid() rather than a semaphore: the
 * in-stack dispatch runs where blocking is not allowed, and the critical
 * sections are a few instructions.
 */
static VOID bsd_mon_setup(VOID)
{
    UWORD i;

    if (bsd_mon_ready)
        return;

    for (i = 0; i < (UWORD)BSD_MHT_COUNT; i++)
    {
        /* NewMinList(), open-coded: amiga.lib is not available here. */
        bsd_mon_list[i].mlh_Head     = (struct MinNode *)&bsd_mon_list[i].mlh_Tail;
        bsd_mon_list[i].mlh_Tail     = NULL;
        bsd_mon_list[i].mlh_TailPred = (struct MinNode *)&bsd_mon_list[i];
    }

    bsd_mon_ready = TRUE;
}

BOOL bsd_netmon_busy(VOID)
{
    return (bsd_mon_count > 0) ? TRUE : FALSE;
}

/* Is this hook already in any list? Called under Forbid(). */
static BOOL bsd_mon_installed(const struct Hook *hook)
{
    UWORD i;

    for (i = 0; i < (UWORD)BSD_MHT_COUNT; i++)
    {
        struct MinNode *node;

        for (node = bsd_mon_list[i].mlh_Head; node->mln_Succ != NULL;
             node = node->mln_Succ)
        {
            if ((const struct Hook *)node == hook)
                return TRUE;
        }
    }

    return FALSE;
}

LONG bsd_AddNetMonitorHookTagList(register LONG type __asm("d0"),
                                  register struct Hook *hook __asm("a0"),
                                  register struct TagItem *tags __asm("a1"),
                                  register struct AmiSocketBase *SocketBase __asm("a6"))
{
    /* "tags, Pointer to a 'struct TagItem' list. No tags are defined yet."
       Accepted and ignored. A refusal of a non-NULL list breaks the first
       caller that passes TAG_DONE. */
    (VOID)tags;

    /* "[EFAULT] The hook parameter is not valid." */
    if (hook == NULL || hook->h_Entry == NULL)
        return bsd_fail(SocketBase, AMI_EFAULT);

    /*
     * "[EINVAL] The monitor type is not supported."
     *
     * Four of the seven are not supported here. The registry below serves any
     * type, but a hook is only called from a dispatch point, and only
     * connect(), bind() and send() have one. A hook accepted for a type that
     * nothing dispatches returns success to a caller. That caller then never
     * hears anything, and cannot tell that from a quiet network.
     *
     *   MHT_ICMP           all four are invoked "from within the TCP/IP stack
     *   MHT_UDP            itself", the IP receive path, which reaches this
     *   MHT_TCP_Connect    library only through the NX_IP packet filter that
     *   MHT_Packet         netstack_capture.c already installs for src/bpf/.
     *                      A dispatch of them has to chain that filter, which
     *                      is a netstack change and not this one.
     *
     * MHT_TCP_Connect has a second obstacle: MA_DropWithReset needs an RST
     * sent from inside a receive filter. A silent degrade to a plain drop
     * hides that from the caller, so the hook is refused instead.
     */
    if (type < 0 || type >= BSD_MHT_COUNT)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (type != MHT_Connect && type != MHT_Bind && type != MHT_Send)
        return bsd_fail(SocketBase, AMI_EINVAL);

    Forbid();

    bsd_mon_setup();

    /*
     * The same Hook twice is two nodes that share one MinNode, which corrupts
     * both lists on the first walk. RemoveNetMonitorHook() takes no type, so
     * it cannot say which to take out. EBUSY is not one of the two documented
     * errors, but neither of those fits: the hook is valid and so is the type.
     */
    if (bsd_mon_installed(hook))
    {
        Permit();
        return bsd_fail(SocketBase, AMI_EBUSY);
    }

    AddTail((struct List *)&bsd_mon_list[type], (struct Node *)&hook->h_MinNode);
    bsd_mon_count++;

    Permit();

    return 0;
}

VOID bsd_RemoveNetMonitorHook(register struct Hook *hook __asm("a0"),
                              register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)SocketBase;

    /* "this may be NULL in which case this function does nothing." */
    if (hook == NULL)
        return;

    Forbid();

    /*
     * Searched rather than trusted. Remove() on a node that is not in a list
     * writes through whatever mln_Succ and mln_Pred happen to hold. A remove
     * that takes no type invites a caller to remove the same hook twice.
     */
    if (bsd_mon_ready && bsd_mon_installed(hook))
    {
        Remove((struct Node *)&hook->h_MinNode);
        bsd_mon_count--;
    }

    Permit();
}

/* ---------------------------------------------------------- the dispatch, */

/*
 * The hook itself, called the way the autodoc lays it out: the Hook in A0,
 * NULL in A2, the message in A1. Declared with register annotations rather
 * than called through inline assembly. The compiler then checks the shape at
 * every call site, as bsdsocket_vectors.h does for the LVOs.
 */
typedef LONG (*BsdMonitorFn)(register struct Hook *hook __asm("a0"),
                             register APTR reserved __asm("a2"),
                             register APTR message __asm("a1"));

/*
 * utility/hooks.h declares h_Entry as `ULONG (*)()`, no parameters, because
 * a Hook carries whatever shape the installer agreed on. The real shape is a
 * conversion between two function types, and a plain cast cannot express that
 * without -Wcast-function-type. The union does the conversion and makes no
 * claim that the two types are compatible. The API decides which declaration
 * is correct, not the compiler.
 */
typedef union BsdMonitorEntry
{
    ULONG        (*bme_Raw)(VOID);
    BsdMonitorFn   bme_Fn;
} BsdMonitorEntry;

/*
 * Every hook of one type, in install order. Stops at the first that does not
 * answer 0.
 *
 * The walk stops because of "unless another hook denies this": one hook that
 * allows a call cannot overrule another that denied it, so the first refusal
 * is the answer. MA_Continue is 0 and so is "no error", which lets the two
 * vocabularies share this path.
 *
 * The list is walked under Forbid() because another task can remove a hook at
 * any moment, so a hook runs with task switching disabled. The
 * autodoc expects that: "the hook code may be invoked frequently, your code
 * should perform its tasks swiftly and without delay".
 */
LONG bsd_netmon_dispatch(LONG type, APTR message)
{
    struct MinNode *node;
    LONG            result = 0;

    if (!bsd_mon_ready || bsd_mon_count == 0 ||
        type < 0 || type >= BSD_MHT_COUNT)
        return 0;

    Forbid();

    for (node = bsd_mon_list[type].mlh_Head; node->mln_Succ != NULL;
         node = node->mln_Succ)
    {
        struct Hook     *hook = (struct Hook *)node;
        BsdMonitorEntry  entry;

        entry.bme_Raw = hook->h_Entry;

        result = entry.bme_Fn(hook, NULL, message);
        if (result != 0)
            break;
    }

    Permit();

    return result;
}

BOOL bsd_netmon_have(LONG type)
{
    BOOL any;

    if (!bsd_mon_ready || bsd_mon_count == 0 ||
        type < 0 || type >= BSD_MHT_COUNT)
        return FALSE;

    Forbid();
    any = (bsd_mon_list[type].mlh_Head->mln_Succ != NULL) ? TRUE : FALSE;
    Permit();

    return any;
}

/*
 * The name a program chose to be known by, for the cmm_Caller/bmm_Caller
 * field. SBTC_LOGTAGPTR is the only place an application can say what it is
 * called. That is what "if it chose to be identified" means here. A program
 * that never set one is reported as NULL.
 */
STRPTR bsd_netmon_caller(struct AmiSocketBase *base)
{
    return (base != NULL) ? (STRPTR)base->sb_LogTag : NULL;
}
