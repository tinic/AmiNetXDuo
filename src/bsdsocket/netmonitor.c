/*
 * bsdsocket.library -- the network monitoring hooks.
 *
 *   AddNetMonitorHookTagList()  install a callback
 *   RemoveNetMonitorHook()      take it out again
 *
 * "Monitoring hooks can be used both for inspecting and filtering data that
 * enters the stack, or for denying access to certain APIs. More than one hook
 * can be installed for each monitoring task."
 *
 * WRITTEN FROM THE AUTODOC
 *
 * Same primary source as interfaces.c, routing.c and netstats.c: NDK 3.2's
 * SANA+RoadshowTCP-IP/doc/bsdsocket.doc, plus libraries/bsdsocket.h from the
 * same NDK, used as an ABI reference only. No Roadshow, AmiTCP, AROSTCP or
 * Miami code was consulted or is present.
 *
 * WHAT THE DOCUMENT SETTLED
 *
 *   The hook is called with the STANDARD Amiga hook register convention and
 *   the middle argument is NOT the usual object: "action = hookfunc(hook,
 *   reserved, imm)" in A0, A2, A1, with "the 'reserved' parameter will be set
 *   to NULL for future expansion". A0/A1 alone would have been a guess, and
 *   passing anything but NULL in A2 would become an ABI promise.
 *
 *   The two families answer with DIFFERENT vocabularies, and the difference
 *   is the whole design:
 *
 *     the in-stack types  return MA_Continue / MA_Ignore / MA_Drop /
 *     (ICMP, UDP,         MA_DropWithReset -- what to do with a packet.
 *      TCP_Connect,       "Any positive return value will cause the data to
 *      Packet)            be dropped and the corresponding errno value to be
 *                        set to the result."
 *
 *     the call-site types return an ERRNO -- "An error value of 0 will allow
 *     (Connect, Bind,     the call to proceed (unless another hook denies
 *      Send)              this), and any error value > 0 will cause the call
 *                        to be aborted and the errno variable to be set to
 *                        this value."
 *
 *   So a hook that returns 0 means "continue" in both, and everything else
 *   means something different in each. MA_Continue is 0, which is what makes
 *   one dispatcher able to serve both.
 *
 *   RemoveNetMonitorHook() takes ONLY the hook -- no type -- so removal
 *   searches every list. That is why one hook cannot be installed for two
 *   types at once: there would be no way to say which one to remove.
 *
 * WHAT IS AND IS NOT ACCEPTED
 *
 * MHT_Connect and MHT_Bind. The other five are refused with EINVAL -- the
 * documented error for a type this library does not support -- because
 * nothing dispatches them yet and a hook that is installed but never called
 * is indistinguishable from a quiet network. The reasons are at the check
 * itself, in bsd_AddNetMonitorHookTagList().
 *
 * WHY AN INSTALLED HOOK KEEPS THE LIBRARY RESIDENT
 *
 * "It must be called before the library is closed, or the library will stay
 * in memory indefinitely." That is not a warning about a leak, it is a
 * description of the behaviour, and bsd_lib_expunge() implements it by
 * declining while bsd_netmon_busy() is true. The alternative -- expunging
 * with hooks installed -- unloads the segment out from under a caller that
 * still believes its hook is live.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "netmonitor.h"

#include <proto/exec.h>

/*
 * One list per type. MHT_ICMP is 0 and MHT_Bind is 6, so the type doubles as
 * the index -- checked by the _Static_asserts below rather than assumed,
 * because a renumbering would otherwise silently file hooks under the wrong
 * kind.
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
 * any task, and mutated by whoever installs a hook. Forbid() rather than a
 * semaphore: the in-stack dispatch runs where blocking is not allowed, the
 * critical sections are a few instructions, and a semaphore that one side
 * cannot take is not protection.
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
    /* "tags -- Pointer to a 'struct TagItem' list. No tags are defined yet."
       Accepted and ignored, which is what a tag list with no tags in it is
       for; refusing a non-NULL one would break the first caller that passed
       TAG_DONE. */
    (VOID)tags;

    /* "[EFAULT] The hook parameter is not valid." */
    if (hook == NULL || hook->h_Entry == NULL)
        return bsd_fail(SocketBase, AMI_EFAULT);

    /*
     * "[EINVAL] The monitor type is not supported."
     *
     * AND FIVE OF THE SEVEN ARE NOT, HERE. The registry below serves any
     * type, but a hook is only ever called from a dispatch point, and only
     * connect() and bind() have one. Accepting a hook for a type nothing
     * dispatches would return success to a caller that then never hears
     * anything -- which it cannot distinguish from a quiet network, and which
     * is the worst answer this API can give. EINVAL is the documented one,
     * and it is true: this library does not support monitoring those.
     *
     *   MHT_Send           needs the hook in send()/sendto()/sendmsg(), which
     *                      live in transfer.c.
     *   MHT_ICMP           all four are invoked "from within the TCP/IP stack
     *   MHT_UDP            itself" -- the IP receive path, which reaches this
     *   MHT_TCP_Connect    library only through the NX_IP packet filter that
     *   MHT_Packet         netstack_capture.c already installs for src/bpf/.
     *                      Dispatching them means chaining that filter, which
     *                      is a netstack change and not this one.
     *
     * MA_DropWithReset is the other reason to leave MHT_TCP_Connect alone for
     * now: honouring it means emitting an RST from inside a receive filter,
     * and a hook told it can reject a connection gracefully, whose rejection
     * silently became a plain drop, has been given a worse answer than a
     * refusal to install.
     */
    if (type < 0 || type >= BSD_MHT_COUNT)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (type != MHT_Connect && type != MHT_Bind)
        return bsd_fail(SocketBase, AMI_EINVAL);

    Forbid();

    bsd_mon_setup();

    /*
     * The same Hook twice would be two nodes sharing one MinNode, which
     * corrupts both lists on the first walk -- and RemoveNetMonitorHook()
     * takes no type, so it could not have said which to take out anyway.
     * EBUSY is not one of the two documented errors, and neither documented
     * error fits: the hook is valid and so is the type.
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
     * Searched rather than trusted: Remove() on a node that is not in a list
     * writes through mln_Succ and mln_Pred whatever they happen to hold, and
     * a caller removing the same hook twice is exactly the mistake to expect
     * from an API whose remove takes no type.
     */
    if (bsd_mon_ready && bsd_mon_installed(hook))
    {
        Remove((struct Node *)&hook->h_MinNode);
        bsd_mon_count--;
    }

    Permit();
}

/* ---------------------------------------------------------- the dispatch -- */

/*
 * The hook itself, called the way the autodoc lays it out: the Hook in A0,
 * NULL in A2, the message in A1. Declared with the register annotations
 * rather than called through inline assembly, so the compiler checks the
 * shape at every call site -- the same thing bsdsocket_vectors.h does for the
 * LVOs.
 */
typedef LONG (*BsdMonitorFn)(register struct Hook *hook __asm("a0"),
                             register APTR reserved __asm("a2"),
                             register APTR message __asm("a1"));

/*
 * utility/hooks.h declares h_Entry as `ULONG (*)()` -- no parameters at all,
 * because a Hook carries whatever shape the thing that installed it agreed
 * on. Reaching the real shape is a conversion between two function types,
 * which is what -Wcast-function-type is for and which a plain cast cannot
 * express without tripping it.
 *
 * The union says the same thing without pretending the two types are
 * compatible: the bits are one pointer, and which declaration is correct is
 * decided by the API, not by the compiler.
 */
typedef union BsdMonitorEntry
{
    ULONG        (*bme_Raw)(VOID);
    BsdMonitorFn   bme_Fn;
} BsdMonitorEntry;

/*
 * Every hook of one type, in the order they were installed, stopping at the
 * first that does not answer 0.
 *
 * "unless another hook denies this" is the whole reason the walk stops: one
 * hook allowing a call cannot overrule another that denied it, so the first
 * refusal is the answer and the rest are not consulted. MA_Continue is 0 and
 * so is "no error", which is what lets the two vocabularies share this.
 *
 * The list is walked under Forbid() because a hook may be removed by another
 * task at any moment. That means a hook runs with task switching disabled,
 * which is severe -- and it is also what the autodoc asks for: "the hook code
 * may be invoked frequently, your code should perform its tasks swiftly and
 * without delay".
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
 * field. SBTC_LOGTAGPTR is the only place this library lets an application
 * say what it is called, so that is what "if it chose to be identified"
 * means here; a program that never set one is reported as NULL rather than
 * as a made-up name.
 */
STRPTR bsd_netmon_caller(struct AmiSocketBase *base)
{
    return (base != NULL) ? (STRPTR)base->sb_LogTag : NULL;
}
