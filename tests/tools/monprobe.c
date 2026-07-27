/*
 * MonProbe -- the network monitoring hooks, and whether they can say no.
 *
 * "Monitoring hooks can be used both for inspecting and filtering data that
 * enters the stack, or for denying access to certain APIs." The denying half
 * is what this exercises, because it is the half with consequences: a hook
 * that returns an errno must make bind() or connect() fail with exactly that
 * errno, and must do it BEFORE the stack has done anything.
 *
 * Three things here cannot be checked by a build:
 *
 *   1. THE REGISTER CONVENTION. The hook is entered with the Hook in A0, NULL
 *      in A2 and the message in A1 -- not the A0/A1 pair a reader would
 *      guess, and not utility.library's usual "object in A2". A wrong guess
 *      passes the message in the wrong register and the hook reads rubbish.
 *
 *   2. THE WALK STOPS AT THE FIRST REFUSAL. "unless another hook denies
 *      this" -- so a hook that allows a call cannot overrule one that denied
 *      it. The probe installs two and counts invocations to prove the second
 *      is never consulted once the first has said no.
 *
 *   3. THE MESSAGE IS THE PUBLISHED SHAPE. bmm_Size, bmm_Socket and bmm_Name
 *      are checked against what was actually passed to bind().
 *
 * Vectors are called by hand at their LVOs, as in the other probes.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/lists.h>
#include <dos/dos.h>
#include <utility/hooks.h>
#include <utility/tagitem.h>

/* <libraries/bsdsocket.h> pulls in <sys/socket.h>, which uses size_t and
   ssize_t without declaring them. Same ordering note as ifprobe.c. */
#include <stddef.h>
#include <sys/types.h>
#include <libraries/bsdsocket.h>

#include <proto/exec.h>
#include <proto/dos.h>

/* ------------------------------------------------------------- vectors ---- */

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

/* ---------------------------------------------------------------- hooks --- */

/*
 * h_Entry is declared `ULONG (*)()` -- no parameters -- because a Hook
 * carries whatever shape the installer and the caller agreed on. Reaching the
 * agreed shape is a conversion between function types, which a plain cast
 * cannot express without tripping -Wcast-function-type. The union says the
 * same thing without claiming the two types are compatible.
 */
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

    /*
     * BindMonitorMsg and ConnectMonitorMsg have the same first four members
     * in the same order, so one reader serves both -- which is a property of
     * the published structs, not an assumption: size, caller, socket, name.
     */
    if (message != NULL)
    {
        const struct BindMonitorMsg *bmm =
            (const struct BindMonitorMsg *)message;

        st->ps_Size   = bmm->bmm_Size;
        st->ps_Socket = bmm->bmm_Socket;
        st->ps_Name   = (APTR)bmm->bmm_Name;
    }

    return st->ps_Answer;
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

    st->ps_Calls    = 0;
    st->ps_Answer   = 0;
    st->ps_Message  = NULL;
    st->ps_Reserved = (APTR)~0UL;   /* poisoned: NULL must be observed */
    st->ps_Hook     = NULL;
    st->ps_Size     = 0;
    st->ps_Socket   = -1;
    st->ps_Name     = NULL;
}

/* ------------------------------------------------------------------ main -- */

#define P_AF_INET       2
#define P_SOCK_STREAM   1
#define P_SOCK_DGRAM    2
#define PROBE_PORT      7788
#define PROBE_DENY      13          /* EACCES, and nothing else returns it */

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

    probe_hook_init(&hook_a, &state_a);
    probe_hook_init(&hook_b, &state_b);

    for (i = 0; i < sizeof(sa); i++)
        ((UBYTE *)&sa)[i] = 0;
    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = P_AF_INET;
    sa.sin_port   = PROBE_PORT;

    /* ---- the two documented errors --------------------------------------- */

    rc = p_add_hook(base, MHT_Bind, NULL, NULL);
    Printf((CONST_STRPTR)"add a NULL hook: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc == -1 && p_errno(base) == 14) ? " -- EFAULT, correctly"
                                                    : " -- WRONG"));

    rc = p_add_hook(base, 99, &hook_a, NULL);
    Printf((CONST_STRPTR)"add type 99: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc == -1 && p_errno(base) == 22) ? " -- EINVAL, correctly"
                                                    : " -- WRONG"));

    /*
     * A type the API defines but this library does not dispatch. Refusing it
     * is the point: a hook accepted for MHT_Packet and then never called
     * cannot be told apart from a network with no traffic on it.
     */
    rc = p_add_hook(base, MHT_Packet, &hook_a, NULL);
    Printf((CONST_STRPTR)"add MHT_Packet: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc == -1 && p_errno(base) == 22)
                      ? " -- refused rather than silently ignored, correctly"
                      : " -- WRONG"));

    /* ---- a hook that allows ---------------------------------------------- */

    rc = p_add_hook(base, MHT_Bind, &hook_a, NULL);
    Printf((CONST_STRPTR)"add MHT_Bind hook: rc %ld%s\n", rc,
           (LONG)((rc == 0) ? " -- installed, correctly" : " -- WRONG"));

    /* The same hook again must be refused: RemoveNetMonitorHook takes no
       type, so it could not say which of the two to take out. */
    rc = p_add_hook(base, MHT_Connect, &hook_a, NULL);
    Printf((CONST_STRPTR)"add the same hook twice: rc %ld%s\n", rc,
           (LONG)((rc == -1) ? " -- refused, correctly" : " -- ACCEPTED, WRONG"));

    state_a.ps_Answer = 0;
    s = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
    rc = p_bind(base, s, &sa);
    Printf((CONST_STRPTR)"bind with an allowing hook: rc %ld, called %ld%s\n",
           rc, state_a.ps_Calls,
           (LONG)((rc == 0 && state_a.ps_Calls == 1)
                      ? " -- allowed and seen, correctly" : " -- WRONG"));

    /* ---- and what it was handed ------------------------------------------ */

    Printf((CONST_STRPTR)"message: size %ld (want %ld), socket %ld (want %ld), "
                         "name %s\n",
           state_a.ps_Size, (LONG)sizeof(struct BindMonitorMsg),
           state_a.ps_Socket, s,
           (LONG)((state_a.ps_Name == (APTR)&sa) ? "ours" : "NOT OURS"));

    Printf((CONST_STRPTR)"message is the published shape: %s\n",
           (LONG)((state_a.ps_Size == (LONG)sizeof(struct BindMonitorMsg) &&
                   state_a.ps_Socket == s &&
                   state_a.ps_Name == (APTR)&sa)
                      ? "yes -- correctly" : "NO"));

    /* A2 must be NULL, and it was poisoned before the call, so this is the
       register convention itself under test. */
    Printf((CONST_STRPTR)"reserved was %s, hook was %s\n",
           (LONG)((state_a.ps_Reserved == NULL) ? "NULL -- correctly"
                                                : "NOT NULL, WRONG"),
           (LONG)((state_a.ps_Hook == &hook_a) ? "ours -- correctly"
                                               : "NOT OURS, WRONG"));

    (VOID)p_close(base, s);

    /* ---- a hook that denies ----------------------------------------------- */

    state_a.ps_Answer = PROBE_DENY;
    state_a.ps_Calls  = 0;

    s  = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
    rc = p_bind(base, s, &sa);
    Printf((CONST_STRPTR)"bind with a denying hook: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc == -1 && p_errno(base) == PROBE_DENY)
                      ? " -- denied with the hook's errno, correctly"
                      : " -- WRONG"));
    (VOID)p_close(base, s);

    /* ---- two hooks, and the walk that stops ------------------------------- */

    p_remove_hook(base, &hook_a);
    probe_hook_init(&hook_a, &state_a);
    probe_hook_init(&hook_b, &state_b);

    (VOID)p_add_hook(base, MHT_Bind, &hook_a, NULL);
    rc = p_add_hook(base, MHT_Bind, &hook_b, NULL);
    Printf((CONST_STRPTR)"two hooks on one type: rc %ld%s\n", rc,
           (LONG)((rc == 0) ? " -- both installed, correctly" : " -- WRONG"));

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
                      ? " -- one hook cannot overrule another, correctly"
                      : " -- WRONG"));
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
                      ? " -- the walk stopped, correctly" : " -- WRONG"));
    (VOID)p_close(base, s);

    p_remove_hook(base, &hook_a);
    p_remove_hook(base, &hook_b);

    /* ---- MHT_Connect ------------------------------------------------------ */

    probe_hook_init(&hook_a, &state_a);
    (VOID)p_add_hook(base, MHT_Connect, &hook_a, NULL);

    state_a.ps_Answer = PROBE_DENY;
    sa.sin_addr = 0x0A000202UL;         /* 10.0.2.2, which would answer */

    s  = p_socket(base, P_AF_INET, P_SOCK_STREAM, 0);
    rc = p_connect(base, s, &sa);
    Printf((CONST_STRPTR)"connect with a denying hook: rc %ld (errno %ld), "
                         "called %ld%s\n",
           rc, p_errno(base), state_a.ps_Calls,
           (LONG)((rc == -1 && p_errno(base) == PROBE_DENY &&
                   state_a.ps_Calls == 1)
                      ? " -- denied before the connect, correctly"
                      : " -- WRONG"));
    (VOID)p_close(base, s);

    /* ---- removed means not consulted -------------------------------------- */

    p_remove_hook(base, &hook_a);
    state_a.ps_Calls = 0;

    s  = p_socket(base, P_AF_INET, P_SOCK_DGRAM, 0);
    sa.sin_addr = 0;
    sa.sin_port = PROBE_PORT + 1;
    rc = p_bind(base, s, &sa);
    Printf((CONST_STRPTR)"after removal: bind rc %ld, called %ld%s\n",
           rc, state_a.ps_Calls,
           (LONG)((rc == 0 && state_a.ps_Calls == 0)
                      ? " -- no longer consulted, correctly" : " -- WRONG"));
    (VOID)p_close(base, s);

    /* Documented to do nothing rather than to fault, and a second removal of
       a hook already out is the mistake an API with no type invites. */
    p_remove_hook(base, NULL);
    p_remove_hook(base, &hook_a);
    Printf((CONST_STRPTR)"RemoveNetMonitorHook(NULL) and twice: returned\n");

    CloseLibrary(base);

    return RETURN_OK;
}
