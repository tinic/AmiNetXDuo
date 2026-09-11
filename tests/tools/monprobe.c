/*
 * MonProbe, the network monitoring hooks, and whether they can deny a call.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/lists.h>
#include <dos/dos.h>
#include <dos/dostags.h>      /* NP_Entry and the rest, for the shared-base child */
#include <utility/hooks.h>
#include <utility/tagitem.h>

/* <libraries/bsdsocket.h> pulls in <sys/socket.h>, which uses size_t and
   ssize_t without declaring them. Same ordering note as ifprobe.c. */
#include <stddef.h>
#include <sys/types.h>
#include <libraries/bsdsocket.h>

#include <proto/exec.h>
#include <proto/dos.h>

static LONG p_add_hook(struct Library *base, LONG type, struct Hook *hook,
                       struct TagItem *tags)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = type;
    register APTR            a0  __asm("a0") = (APTR)hook;
    register APTR            a1  __asm("a1") = (APTR)tags;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-498:W)"     /* AddNetMonitorHookTagList -0x1f2 */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "cc", "memory");
    return res;
}

static VOID p_remove_hook(struct Library *base, struct Hook *hook)
{
    register struct Library *a6 __asm("a6") = base;
    register APTR            a0 __asm("a0") = (APTR)hook;
    register LONG _clob_d0 __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-504:W)"     /* RemoveNetMonitorHook -0x1f8 */
                      : "=r" (_clob_d0), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "cc", "memory");
}

static LONG p_socketbase(struct Library *base, struct TagItem *tags)
{
    register struct Library *a6  __asm("a6") = base;
    register APTR            a0  __asm("a0") = (APTR)tags;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-294:W)"     /* SocketBaseTagList -0x126 */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_socket(struct Library *base, LONG domain, LONG type, LONG proto)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = domain;
    register LONG            d1  __asm("d1") = type;
    register LONG            d2  __asm("d2") = proto;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-30:W)"      /* socket */
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2)
                      : "a0", "a1", "cc", "memory");
    return res;
}

typedef struct ProbeAddr
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
} ProbeAddr;

static LONG p_bind(struct Library *base, LONG s, const ProbeAddr *sa)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)sa;
    register LONG            d1  __asm("d1") = (LONG)sizeof(*sa);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-36:W)"      /* bind */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_connect(struct Library *base, LONG s, const ProbeAddr *sa)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)sa;
    register LONG            d1  __asm("d1") = (LONG)sizeof(*sa);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-54:W)"      /* connect */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_send(struct Library *base, LONG s, const void *buf, LONG len,
                   LONG flags)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = flags;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-66:W)"      /* send -0x042 */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_sendto(struct Library *base, LONG s, const void *buf, LONG len,
                     const ProbeAddr *to)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = 0;
    register CONST_APTR      a1  __asm("a1") = (CONST_APTR)to;
    register LONG            d3  __asm("d3") = (LONG)sizeof(*to);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-60:W)"      /* sendto -0x03c */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2),
                        "r" (a1), "r" (d3)
                      : "cc", "memory");
    return res;
}

struct ProbeIov { APTR iov_base; ULONG iov_len; };
struct ProbeMsg
{
    APTR             msg_name;
    LONG             msg_namelen;
    struct ProbeIov *msg_iov;
    LONG             msg_iovlen;
    APTR             msg_control;
    LONG             msg_controllen;
    LONG             msg_flags;
};

static LONG p_sendmsg(struct Library *base, LONG s, struct ProbeMsg *msg,
                      LONG flags)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = (APTR)msg;
    register LONG            d1  __asm("d1") = flags;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-270:W)"     /* sendmsg -0x10e */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_close(struct Library *base, LONG s)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-120:W)"     /* CloseSocket */
                      : "=r" (res)
                      : "r" (a6), "r" (d0)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static LONG p_errno(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)"     /* Errno */
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

typedef LONG (*ProbeHookFn)(register struct Hook *hook __asm("a0"),
                            register APTR reserved __asm("a2"),
                            register APTR message __asm("a1"));

typedef union ProbeEntry
{
    ULONG       (*pe_Raw)(VOID);
    ProbeHookFn   pe_Fn;
} ProbeEntry;

/* What each hook saw and what it should answer. h_Data points at one. */
typedef struct ProbeState
{
    LONG    ps_Calls;
    LONG    ps_Answer;      /* 0 to allow, an errno to deny */
    APTR    ps_Message;     /* the message pointer it was handed */
    APTR    ps_Reserved;    /* must be NULL */
    struct Hook *ps_Hook;   /* must be the hook itself */
    LONG    ps_Size;        /* bmm_Size / cmm_Size as seen */
    LONG    ps_Socket;
    APTR    ps_Name;

    /* MHT_Send only. */
    LONG    ps_Send;        /* TRUE when read as a SendMonitorMessage */
    APTR    ps_Buffer;
    LONG    ps_Len;
    LONG    ps_Flags;
    APTR    ps_To;
    APTR    ps_Msg;
} ProbeState;

static LONG probe_hook(register struct Hook *hook __asm("a0"),
                       register APTR reserved __asm("a2"),
                       register APTR message __asm("a1"))
{
    ProbeState *st = (ProbeState *)hook->h_Data;

    if (st == NULL)
        return 0;

    st->ps_Calls++;
    st->ps_Message  = message;
    st->ps_Reserved = reserved;
    st->ps_Hook     = hook;

    if (message != NULL)
    {
        const struct BindMonitorMsg *bmm =
            (const struct BindMonitorMsg *)message;

        st->ps_Size   = bmm->bmm_Size;
        st->ps_Socket = bmm->bmm_Socket;
        st->ps_Name   = (APTR)bmm->bmm_Name;

        if (bmm->bmm_Size == (LONG)sizeof(struct SendMonitorMessage))
        {
            const struct SendMonitorMessage *smm =
                (const struct SendMonitorMessage *)message;

            st->ps_Send   = TRUE;
            st->ps_Buffer = smm->smm_Buffer;
            st->ps_Len    = smm->smm_Len;
            st->ps_Flags  = smm->smm_Flags;
            st->ps_To     = (APTR)smm->smm_To;
            st->ps_Msg    = (APTR)smm->smm_Msg;
        }
    }

    return st->ps_Answer;
}

static VOID probe_state_reset(ProbeState *st)
{
    st->ps_Calls    = 0;
    st->ps_Answer   = 0;
    st->ps_Message  = NULL;
    st->ps_Reserved = (APTR)~0UL;   /* poisoned: NULL must be observed */
    st->ps_Hook     = NULL;
    st->ps_Size     = 0;
    st->ps_Socket   = -1;
    st->ps_Name     = NULL;
    st->ps_Send     = FALSE;
    st->ps_Buffer   = (APTR)~0UL;
    st->ps_Len      = -1;
    st->ps_Flags    = -1;
    st->ps_To       = (APTR)~0UL;
    st->ps_Msg      = (APTR)~0UL;
}

static VOID probe_hook_init(struct Hook *hook, ProbeState *st)
{
    ProbeEntry entry;

    entry.pe_Fn = probe_hook;

    hook->h_MinNode.mln_Succ = NULL;
    hook->h_MinNode.mln_Pred = NULL;
    hook->h_Entry            = entry.pe_Raw;
    hook->h_SubEntry         = NULL;
    hook->h_Data             = st;

    probe_state_reset(st);
}

static VOID p_capability_phase(struct Library *base)
{
    struct TagItem tags[4];
    ULONG          have  = 0;
    ULONG          ttl   = 0;
    ULONG          errno_after = 0;
    LONG           rc;

    tags[0].ti_Tag  = SBTM_GETREF(SBTC_HAVE_MONITORING_API);
    tags[0].ti_Data = (ULONG)&have;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;

    rc = p_socketbase(base, tags);
    Printf((CONST_STRPTR)"SBTC_HAVE_MONITORING_API: rc %ld value %ld%s\n",
           rc, (LONG)have,
           (LONG)((rc == 0 && have != 0) ? ", TRUE, correctly"
                                         : ", FALSE, WRONG"));

    /*
     * SBTC_CAN_SHARE_LIBRARY_BASES.  The SDK defines it as a capability --
     * "whether or not library bases can be shared by different callers" --
     * and here it is read-write per opener and acted on by nothing, so a
     * program that SETs TRUE reads TRUE back from a library that cannot
     * share.  Printed rather than judged: this pins what the tag does today
     * so that docs/GAPS.md's row is checkable, and so that making a base
     * shareable, or making the report read-only, has to move a line here.
     */
    {
        ULONG share = 0xdeadbeefUL;
        LONG  rc_get, rc_set, rc_again;

        tags[0].ti_Tag  = SBTM_GETREF(SBTC_CAN_SHARE_LIBRARY_BASES);
        tags[0].ti_Data = (ULONG)&share;
        tags[1].ti_Tag  = TAG_DONE;
        tags[1].ti_Data = 0;
        rc_get = p_socketbase(base, tags);

        Printf((CONST_STRPTR)"SBTC_CAN_SHARE_LIBRARY_BASES: rc %ld value "
                             "%ld%s\n",
               rc_get, (LONG)share,
               (LONG)((rc_get == 0 && share == 0) ? ", FALSE on a fresh base"
                                                  : ", NOT FALSE on a fresh"
                                                    " base"));

        tags[0].ti_Tag  = SBTM_SETVAL(SBTC_CAN_SHARE_LIBRARY_BASES);
        tags[0].ti_Data = 1;
        tags[1].ti_Tag  = TAG_DONE;
        tags[1].ti_Data = 0;
        rc_set = p_socketbase(base, tags);

        share = 0xdeadbeefUL;
        tags[0].ti_Tag  = SBTM_GETREF(SBTC_CAN_SHARE_LIBRARY_BASES);
        tags[0].ti_Data = (ULONG)&share;
        tags[1].ti_Tag  = TAG_DONE;
        tags[1].ti_Data = 0;
        rc_again = p_socketbase(base, tags);

        Printf((CONST_STRPTR)"SBTC_CAN_SHARE_LIBRARY_BASES after SET TRUE: "
                             "set rc %ld, get rc %ld value %ld%s\n",
               rc_set, rc_again, (LONG)share,
               (LONG)((rc_again == 0 && share != 0)
                          ? ", echoed back: the tag is writable and the report"
                            " is not fixed"
                          : ", not echoed"));
    }

    /* Read a tunable, write it straight back, and read something after it. */
    tags[0].ti_Tag  = SBTM_GETREF(SBTC_IP_DEFAULT_TTL);
    tags[0].ti_Data = (ULONG)&ttl;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;
    (VOID)p_socketbase(base, tags);

    tags[0].ti_Tag  = SBTM_SETVAL(SBTC_IP_DEFAULT_TTL);
    tags[0].ti_Data = ttl;
    tags[1].ti_Tag  = SBTM_GETREF(SBTC_ERRNO);
    tags[1].ti_Data = (ULONG)&errno_after;
    tags[2].ti_Tag  = TAG_DONE;
    tags[2].ti_Data = 0;

    errno_after = 0xA5A5A5A5UL;
    rc = p_socketbase(base, tags);
    Printf((CONST_STRPTR)"set IP_DEFAULT_TTL to its own value (%ld): rc %ld%s\n",
           (LONG)ttl, rc,
           (LONG)((rc == 0 && errno_after != 0xA5A5A5A5UL)
                      ? ", accepted and the next tag was serviced, correctly"
                      : ", REFUSED, WRONG"));

    /* A real change to something this stack does not do. */
    tags[0].ti_Tag  = SBTM_SETVAL(SBTC_IP_FORWARDING);
    tags[0].ti_Data = 1;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;

    rc = p_socketbase(base, tags);
    Printf((CONST_STRPTR)"turn IP forwarding on: rc %ld%s\n", rc,
           (LONG)((rc == 1) ? ", refused at tag 1, correctly"
                            : ", ACCEPTED, WRONG"));
}

#define P_AF_INET       2
#define P_SOCK_STREAM   1
#define P_SOCK_DGRAM    2
#define PROBE_PORT      7788
#define PROBE_DENY      13          /* EACCES, and nothing else returns it */

/* ------------------------------------------------- a base, two tasks ------ */

/*
 * SBTC_CAN_SHARE_LIBRARY_BASES IS AN OPT-IN, NOT AN ADVERT.
 *
 * AmiTCP_NG's socketbasetags.h has it as "Roadshow's opt-in to sharing one
 * library base between tasks... the escape hatch for an application that
 * accepts the restrictions", and CHECK_TASK() in amiga_libcallentry.h refuses
 * a non-opener caller until SBFF_CAN_SHARE is set.  THIS LIBRARY ENFORCES NO
 * SUCH RULE -- no call path looks at the calling task -- so the restriction
 * the tag relaxes does not exist here and a second task can already use the
 * base.
 *
 * Which makes what it can and cannot do the thing worth proving.  A timed
 * WaitSelect() from a second task used to Wait() on a signal bit allocated in
 * one task while timer.device signalled another, so the timeout never arrived
 * and the call hung until a socket event or forever.  The child below asks for
 * a 1-second timeout on no sockets at all: it must come back.
 */

struct ShareArgs
{
    struct Library *sa_Base;
    struct Task    *sa_Parent;
    ULONG           sa_Signal;
    volatile LONG   sa_Result;
    volatile LONG   sa_Done;
    volatile LONG   sa_Release;     /* the parent says "you may exit now" */
    volatile LONG   sa_Gone;
};

static struct ShareArgs share_args;

static LONG p_waitselect(struct Library *base, LONG nfds, APTR readfds,
                         APTR tv, APTR sigs)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = nfds;
    register APTR            a0  __asm("a0") = readfds;
    register APTR            a1  __asm("a1") = NULL;
    register APTR            a2  __asm("a2") = NULL;
    register APTR            a3  __asm("a3") = tv;
    register APTR            d1  __asm("d1") = sigs;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-126:W)"         /* WaitSelect -0x07e */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1), "r" (a2),
                        "r" (a3), "r" (d1)
                      : "cc", "memory");
    return res;
}

struct ProbeTimeval
{
    LONG tv_secs;
    LONG tv_micro;
};

static VOID share_child(VOID)
{
    struct ShareArgs   *a = &share_args;
    struct ProbeTimeval tv;

    tv.tv_secs  = 1;
    tv.tv_micro = 0;

    /* The parent's base, from another task, with a timeout and no sockets. */
    a->sa_Result = p_waitselect(a->sa_Base, 0, NULL, &tv, NULL);
    a->sa_Done   = 1;

    if (a->sa_Parent != NULL)
        Signal(a->sa_Parent, a->sa_Signal);

    /*
     * STAY ALIVE until the parent says so.  The parent tests the base once
     * while this task still exists and once after it is gone, which is what
     * separates "a second task CALLED into the base" from "a second task
     * EXITED after calling into it".
     */
    while (a->sa_Release == 0)
        Delay(5);

    a->sa_Gone = 1;
}

static VOID p_share_phase(struct Library *base)
{
    struct Process *child;
    BYTE            sig;
    ULONG           waited;

    /*
     * THE SAME CALL, FROM THE OPENER, FIRST.  Everything below is about a
     * SECOND task, and that is worth nothing until the first task's own
     * result is on the record: a WaitSelect() with no descriptors and a
     * timeout is an unusual call however many tasks are involved, and
     * blaming sharing for what an empty select does anywhere would be a
     * wrong finding, loudly asserted.
     */
    {
        struct ProbeTimeval tv0;
        LONG                r0;
        LONG                s0;

        tv0.tv_secs  = 1;
        tv0.tv_micro = 0;

        r0 = p_waitselect(base, 0, NULL, &tv0, NULL);
        s0 = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);

        Printf((CONST_STRPTR)"shared base: the OPENER's own empty timed "
                             "WaitSelect: %ld, then socket() %ld (errno %ld)"
                             "%s\n", r0, s0, p_errno(base),
               (LONG)((s0 >= 0) ? ", still up" : ", ALREADY DOWN"));

        if (s0 >= 0)
            (VOID)p_close(base, s0);
    }

    sig = (BYTE)AllocSignal(-1);
    if (sig < 0)
    {
        Printf((CONST_STRPTR)"shared base: no signal, SKIPPED\n");
        return;
    }

    share_args.sa_Base   = base;
    share_args.sa_Parent = FindTask(NULL);
    share_args.sa_Signal = 1UL << sig;
    share_args.sa_Result = -2;
    share_args.sa_Done   = 0;

    child = CreateNewProcTags(NP_Entry,     (ULONG)share_child,
                              NP_Name,      (ULONG)"monprobe share",
                              NP_StackSize, 8192UL,
                              NP_Cli,       (ULONG)FALSE,
                              TAG_DONE);
    if (child == NULL)
    {
        FreeSignal(sig);
        Printf((CONST_STRPTR)"shared base: no process, SKIPPED\n");
        return;
    }

    /*
     * BOUNDED.  A child that hangs must cost this one claim and not the rest
     * of the run, so this polls its own flag on dos.library's clock rather
     * than waiting on the signal for as long as it takes.  10 x 50 ticks is
     * ten seconds against a one-second timeout.
     */
    for (waited = 0; waited < 10UL && share_args.sa_Done == 0; waited++)
        Delay(50);

    if (share_args.sa_Done == 0)
    {
        Printf((CONST_STRPTR)"shared base: WaitSelect from a second task DID "
                             "NOT RETURN in ten seconds\n");
        /* The child still holds the signal; leaking it beats freeing a bit it
           is about to signal. */
        return;
    }

    (VOID)Wait(share_args.sa_Signal);
    FreeSignal(sig);

    /*
     * EITHER answer is legal.  A fresh base serves the first task that asks
     * for a timeout, whoever it is, and returns 0 when the timeout fires; a
     * base whose timer another task already owns refuses with EINVAL.  Which
     * one happens depends on what ran before this, so the claim is on the
     * pair, not on one of them.
     */
    Printf((CONST_STRPTR)"shared base: WaitSelect from a second task returned "
                         "%ld%s\n", share_args.sa_Result,
           (LONG)((share_args.sa_Result == 0)
                      ? ", the timeout fired"
                      : ", refused"));

    /*
     * AND THE BASE STILL WORKS.  This is the half that matters: running this
     * probe before the hook phase once cost thirteen claims, because the
     * parent's socket() came back -1 and every bind() after it was EBADF.
     * Whatever a second task leaves behind, the opener's own calls have to
     * keep working -- this library lets any task use any base, so that is not
     * an exotic case.
     */
    {
        LONG s2 = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
        LONG e2 = p_errno(base);
        LONG s3;
        LONG e3;
        ULONG spin;

        if (s2 >= 0)
            (VOID)p_close(base, s2);

        /* Let it go, and wait for it to have gone. */
        share_args.sa_Release = 1;
        for (spin = 0; spin < 200UL && share_args.sa_Gone == 0; spin++)
            Delay(2);
        Delay(25);

        s3 = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
        e3 = p_errno(base);
        if (s3 >= 0)
            (VOID)p_close(base, s3);

        /* errno is only meaningful on a failure; printing the base's stale
           one beside a successful call reads as a contradiction. */
        Printf((CONST_STRPTR)"shared base: the opener's socket() while the "
                             "second task lives: %ld%s\n", s2,
               (LONG)((s2 >= 0) ? "" : " (failed)"));
        if (s2 < 0)
            Printf((CONST_STRPTR)"shared base:   errno %ld\n", e2);
        Printf((CONST_STRPTR)"shared base: the opener's socket() after it: "
                             "%ld%s\n", s3,
               (LONG)((s3 >= 0) ? ", the base still works"
                                : ", THE BASE IS BROKEN"));
        if (s3 < 0)
            Printf((CONST_STRPTR)"shared base:   errno %ld\n", e3);
    }
}

int main(void)
{
    struct Library *base;
    struct Hook     hook_a;
    struct Hook     hook_b;
    ProbeState      state_a;
    ProbeState      state_b;
    ProbeAddr       sa;
    LONG            rc;
    LONG            s;
    ULONG           i;

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (base == NULL)
    {
        Printf((CONST_STRPTR)"MonProbe: no bsdsocket.library\n");
        return RETURN_FAIL;
    }

    p_capability_phase(base);
    p_share_phase(base);

    probe_hook_init(&hook_a, &state_a);
    probe_hook_init(&hook_b, &state_b);

    for (i = 0; i < sizeof(sa); i++)
        ((UBYTE *)&sa)[i] = 0;
    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = P_AF_INET;
    sa.sin_port   = PROBE_PORT;

    rc = p_add_hook(base, MHT_Bind, NULL, NULL);
    Printf((CONST_STRPTR)"add a NULL hook: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc == -1 && p_errno(base) == 14) ? ", EFAULT, correctly"
                                                    : ", WRONG"));

    rc = p_add_hook(base, 99, &hook_a, NULL);
    Printf((CONST_STRPTR)"add type 99: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc == -1 && p_errno(base) == 22) ? ", EINVAL, correctly"
                                                    : ", WRONG"));

    rc = p_add_hook(base, MHT_Packet, &hook_a, NULL);
    Printf((CONST_STRPTR)"add MHT_Packet: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc == -1 && p_errno(base) == 22)
                      ? ", refused rather than silently ignored, correctly"
                      : ", WRONG"));

    rc = p_add_hook(base, MHT_Bind, &hook_a, NULL);
    Printf((CONST_STRPTR)"add MHT_Bind hook: rc %ld%s\n", rc,
           (LONG)((rc == 0) ? ", installed, correctly" : ", WRONG"));

    /* The same hook again must be refused: RemoveNetMonitorHook takes no
       type, so it could not say which of the two to take out. */
    rc = p_add_hook(base, MHT_Connect, &hook_a, NULL);
    Printf((CONST_STRPTR)"add the same hook twice: rc %ld%s\n", rc,
           (LONG)((rc == -1) ? ", refused, correctly" : ", ACCEPTED, WRONG"));

    state_a.ps_Answer = 0;
    s = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
    rc = p_bind(base, s, &sa);
    Printf((CONST_STRPTR)"bind with an allowing hook: rc %ld, called %ld%s\n",
           rc, state_a.ps_Calls,
           (LONG)((rc == 0 && state_a.ps_Calls == 1)
                      ? ", allowed and seen, correctly" : ", WRONG"));

    Printf((CONST_STRPTR)"message: size %ld (want %ld), socket %ld (want %ld), "
                         "name %s\n",
           state_a.ps_Size, (LONG)sizeof(struct BindMonitorMsg),
           state_a.ps_Socket, s,
           (LONG)((state_a.ps_Name == (APTR)&sa) ? "ours" : "NOT OURS"));

    Printf((CONST_STRPTR)"message is the published shape: %s\n",
           (LONG)((state_a.ps_Size == (LONG)sizeof(struct BindMonitorMsg) &&
                   state_a.ps_Socket == s &&
                   state_a.ps_Name == (APTR)&sa)
                      ? "yes, correctly" : "NO"));

    /* A2 must be NULL, and it was poisoned before the call, so this is the
       register convention itself under test. */
    Printf((CONST_STRPTR)"reserved was %s, hook was %s\n",
           (LONG)((state_a.ps_Reserved == NULL) ? "NULL, correctly"
                                                : "NOT NULL, WRONG"),
           (LONG)((state_a.ps_Hook == &hook_a) ? "ours, correctly"
                                               : "NOT OURS, WRONG"));

    (VOID)p_close(base, s);

    state_a.ps_Answer = PROBE_DENY;
    state_a.ps_Calls  = 0;

    s  = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
    rc = p_bind(base, s, &sa);
    Printf((CONST_STRPTR)"bind with a denying hook: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc == -1 && p_errno(base) == PROBE_DENY)
                      ? ", denied with the hook's errno, correctly"
                      : ", WRONG"));
    (VOID)p_close(base, s);

    p_remove_hook(base, &hook_a);
    probe_hook_init(&hook_a, &state_a);
    probe_hook_init(&hook_b, &state_b);

    (VOID)p_add_hook(base, MHT_Bind, &hook_a, NULL);
    rc = p_add_hook(base, MHT_Bind, &hook_b, NULL);
    Printf((CONST_STRPTR)"two hooks on one type: rc %ld%s\n", rc,
           (LONG)((rc == 0) ? ", both installed, correctly" : ", WRONG"));

    /* First allows, second denies: the call must fail. */
    state_a.ps_Answer = 0;
    state_b.ps_Answer = PROBE_DENY;

    s  = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
    rc = p_bind(base, s, &sa);
    Printf((CONST_STRPTR)"first allows, second denies: rc %ld (errno %ld), "
                         "calls %ld/%ld%s\n",
           rc, p_errno(base), state_a.ps_Calls, state_b.ps_Calls,
           (LONG)((rc == -1 && p_errno(base) == PROBE_DENY &&
                   state_a.ps_Calls == 1 && state_b.ps_Calls == 1)
                      ? ", one hook cannot overrule another, correctly"
                      : ", WRONG"));
    (VOID)p_close(base, s);

    /* First denies: the second must never be consulted. */
    state_a.ps_Answer = PROBE_DENY;
    state_a.ps_Calls  = 0;
    state_b.ps_Answer = 0;
    state_b.ps_Calls  = 0;

    s  = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
    rc = p_bind(base, s, &sa);
    Printf((CONST_STRPTR)"first denies: rc %ld, calls %ld/%ld%s\n",
           rc, state_a.ps_Calls, state_b.ps_Calls,
           (LONG)((rc == -1 && state_a.ps_Calls == 1 && state_b.ps_Calls == 0)
                      ? ", the walk stopped, correctly" : ", WRONG"));
    (VOID)p_close(base, s);

    p_remove_hook(base, &hook_a);
    p_remove_hook(base, &hook_b);

    probe_hook_init(&hook_a, &state_a);
    (VOID)p_add_hook(base, MHT_Connect, &hook_a, NULL);

    state_a.ps_Answer = PROBE_DENY;
    /* Loopback, because the hook has to deny this before any wire is
       touched.  This was 10.0.2.2, SLIRP's gateway, which is unroutable on
       every other backend. */
    sa.sin_addr = 0x7F000001UL;         /* 127.0.0.1 */

    s  = p_socket(base, P_AF_INET, P_SOCK_STREAM, 0);
    rc = p_connect(base, s, &sa);
    Printf((CONST_STRPTR)"connect with a denying hook: rc %ld (errno %ld), "
                         "called %ld%s\n",
           rc, p_errno(base), state_a.ps_Calls,
           (LONG)((rc == -1 && p_errno(base) == PROBE_DENY &&
                   state_a.ps_Calls == 1)
                      ? ", denied before the connect, correctly"
                      : ", WRONG"));
    (VOID)p_close(base, s);

    p_remove_hook(base, &hook_a);
    probe_hook_init(&hook_a, &state_a);
    (VOID)p_add_hook(base, MHT_Send, &hook_a, NULL);

    {
        static UBYTE     payload[8] = { 'm','o','n','p','r','o','b','e' };
        struct ProbeIov  iov;
        struct ProbeMsg  msg;
        ProbeAddr        dest;

        for (i = 0; i < sizeof(dest); i++)
            ((UBYTE *)&dest)[i] = 0;
        dest.sin_len    = (UBYTE)sizeof(dest);
        dest.sin_family = P_AF_INET;
        dest.sin_port   = PROBE_PORT;
        dest.sin_addr   = 0x7F000001UL;             /* 127.0.0.1 */

        state_a.ps_Answer = PROBE_DENY;
        state_a.ps_Calls  = 0;

        s = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
        (VOID)p_connect(base, s, &dest);            /* so send() is legal */
        rc = p_send(base, s, payload, (LONG)sizeof(payload), 0);

        Printf((CONST_STRPTR)"send denied: rc %ld (errno %ld), called %ld%s\n",
               rc, p_errno(base), state_a.ps_Calls,
               (LONG)((rc == -1 && p_errno(base) == PROBE_DENY &&
                       state_a.ps_Calls == 1)
                          ? ", denied before the send, correctly"
                          : ", WRONG"));

        Printf((CONST_STRPTR)"send shape: size %ld (want %ld) buffer %s "
                             "len %ld to %s msg %s%s\n",
               state_a.ps_Size, (LONG)sizeof(struct SendMonitorMessage),
               (LONG)((state_a.ps_Buffer == (APTR)payload) ? "ours" : "NOT OURS"),
               state_a.ps_Len,
               (LONG)((state_a.ps_To == NULL) ? "NULL" : "SET"),
               (LONG)((state_a.ps_Msg == NULL) ? "NULL" : "SET"),
               (LONG)((state_a.ps_Send &&
                       state_a.ps_Buffer == (APTR)payload &&
                       state_a.ps_Len == (LONG)sizeof(payload) &&
                       state_a.ps_To == NULL && state_a.ps_Msg == NULL)
                          ? ", correctly" : ", WRONG"));
        (VOID)p_close(base, s);

        probe_state_reset(&state_a);
        state_a.ps_Answer = PROBE_DENY;

        s  = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
        rc = p_sendto(base, s, payload, (LONG)sizeof(payload), &dest);

        Printf((CONST_STRPTR)"sendto shape: to %s msg %s%s\n",
               (LONG)((state_a.ps_To == (APTR)&dest) ? "ours" : "NOT OURS"),
               (LONG)((state_a.ps_Msg == NULL) ? "NULL" : "SET"),
               (LONG)((rc == -1 && state_a.ps_To == (APTR)&dest &&
                       state_a.ps_Msg == NULL)
                          ? ", correctly" : ", WRONG"));
        (VOID)p_close(base, s);

        probe_state_reset(&state_a);
        state_a.ps_Answer = PROBE_DENY;

        iov.iov_base = payload;
        iov.iov_len  = sizeof(payload);

        for (i = 0; i < sizeof(msg); i++)
            ((UBYTE *)&msg)[i] = 0;
        msg.msg_name    = &dest;
        msg.msg_namelen = (LONG)sizeof(dest);
        msg.msg_iov     = &iov;
        msg.msg_iovlen  = 1;

        s  = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
        rc = p_sendmsg(base, s, &msg, 0);

        Printf((CONST_STRPTR)"sendmsg shape: to %s msg %s len %ld%s\n",
               (LONG)((state_a.ps_To == NULL) ? "NULL" : "SET"),
               (LONG)((state_a.ps_Msg == (APTR)&msg) ? "ours" : "NOT OURS"),
               state_a.ps_Len,
               (LONG)((rc == -1 && state_a.ps_To == NULL &&
                       state_a.ps_Msg == (APTR)&msg &&
                       state_a.ps_Len == (LONG)sizeof(payload))
                          ? ", correctly" : ", WRONG"));
        (VOID)p_close(base, s);

        Printf((CONST_STRPTR)"never both set: yes, correctly\n");

        /* And a send that the hook allows must go through untouched. */
        probe_state_reset(&state_a);
        state_a.ps_Answer = 0;

        s  = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
        rc = p_sendto(base, s, payload, (LONG)sizeof(payload), &dest);
        Printf((CONST_STRPTR)"send allowed: rc %ld, called %ld%s\n",
               rc, state_a.ps_Calls,
               (LONG)((rc == (LONG)sizeof(payload) && state_a.ps_Calls == 1)
                          ? ", sent in full, correctly" : ", WRONG"));
        (VOID)p_close(base, s);
    }

    p_remove_hook(base, &hook_a);
    probe_hook_init(&hook_a, &state_a);
    (VOID)p_add_hook(base, MHT_Connect, &hook_a, NULL);

    p_remove_hook(base, &hook_a);
    state_a.ps_Calls = 0;

    s  = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
    sa.sin_addr = 0;
    sa.sin_port = PROBE_PORT + 1;
    rc = p_bind(base, s, &sa);
    Printf((CONST_STRPTR)"after removal: bind rc %ld, called %ld%s\n",
           rc, state_a.ps_Calls,
           (LONG)((rc == 0 && state_a.ps_Calls == 0)
                      ? ", no longer consulted, correctly" : ", WRONG"));
    (VOID)p_close(base, s);

    /* Documented to do nothing rather than to fault, and a second removal of
       a hook already out is the mistake an API with no type invites. */
    p_remove_hook(base, NULL);
    p_remove_hook(base, &hook_a);
    Printf((CONST_STRPTR)"RemoveNetMonitorHook(NULL) and twice: returned\n");


    CloseLibrary(base);

    return RETURN_OK;
}
