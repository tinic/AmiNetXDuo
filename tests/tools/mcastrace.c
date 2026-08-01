/*
 * McastRace -- two openers racing for the same row of the multicast table.
 *
 * bsd_mcast_table[16] is one table for the machine, keyed by AmiSocket *, and
 * a row is free precisely because bm_Sock is NULL.  bsd_mcast_join() picks a
 * free row inside the bsd_nx_enter() bracket; if it marks the row used only
 * AFTER bsd_nx_leave(), a second base joining in that window picks the same
 * row.  One of the two memberships then exists in NetX Duo and nowhere else:
 * IP_DROP_MEMBERSHIP cannot find it, and CloseSocket() never leaves it.
 *
 * THE INVARIANT
 *
 *   A join that returned 0 can be dropped.
 *
 * That is all this asserts, and it is enough: the loser of the race is the
 * opener whose row was overwritten, and the only thing it can observe is its
 * own IP_DROP_MEMBERSHIP failing with EADDRNOTAVAIL on a group it holds.
 *
 * WHY A DATAGRAM IS NOT THE ASSERTION
 *
 *   "Both receive, one closes, the other must still receive" does not fail on
 *   this defect when the two joins are for the SAME group: NetX Duo refcounts
 *   membership per NX_IP, so the two joins leave the count at 2 and one
 *   spurious leave still leaves the group live for the other.  What is
 *   destroyed is the library's own socket-to-group mapping, and the drop above
 *   is where that shows.
 *
 * HOW THE WINDOW IS FORCED
 *
 *   The window is a handful of instructions, so it is not waited for -- it is
 *   aimed at.
 *
 *   bsd_nx_leave() drops the ThreadX baton and pokes the scheduler Task, which
 *   runs at Exec priority 1 (TX_AMIGA_TASK_PRIORITY) and therefore preempts a
 *   priority-0 caller AT THAT INSTRUCTION.  The scheduler then dispatches
 *   whichever adopted Task was parked waiting for the baton, on an Exec Task of
 *   its own.  So if a second Process is parked on the baton when the first one
 *   leaves, the switch to it happens INSIDE the window and not after it.
 *
 *   HAMMER (this Process, priority 0) joins and drops one group as fast as it
 *   can, so it is inside a bracket nearly all the time.
 *
 *   SNIPER (the child, priority 2) sleeps, wakes on the VBlank interrupt --
 *   asynchronously, wherever HAMMER happens to be -- and joins a different
 *   group.  A wake that lands while HAMMER holds the baton parks SNIPER on it,
 *   and HAMMER's next bsd_nx_leave() hands over inside the window.
 *
 *   SNIPER then sleeps once more before dropping, so that HAMMER's late write
 *   to the row lands first.  Dropping immediately would let SNIPER take its own
 *   row back before HAMMER overwrote it, and the run would see nothing.
 *
 * TWO PROCESSES AND TWO BASES, NOT TWO SOCKETS
 *
 *   A base belongs to one Task, and the row is claimed on behalf of a base's
 *   bsd_nx_enter() bracket.  Two sockets on one base are serialised by that
 *   base's nesting counter and cannot reach the window at all.
 *
 * Vectors are called by hand at their LVOs, as in the other probes: the NDK
 * inlines assume one global SocketBase and there are two here.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/dos.h>

/* ------------------------------------------------------------- vectors ---- */

static LONG r_socket(struct Library *base, LONG domain, LONG type, LONG proto)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = domain;
    register LONG            d1  __asm("d1") = type;
    register LONG            d2  __asm("d2") = proto;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-30:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2)
                      : "a0", "a1", "cc", "memory");
    return res;
}

static LONG r_setsockopt(struct Library *base, LONG s, LONG level, LONG name,
                         const void *val, LONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            d1  __asm("d1") = level;
    register LONG            d2  __asm("d2") = name;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)val;
    register LONG            d3  __asm("d3") = len;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-90:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2), "r" (a0),
                        "r" (d3)
                      : "a1", "cc", "memory");
    return res;
}

static LONG r_close(struct Library *base, LONG s)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-120:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static LONG r_errno(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)"
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

/* --------------------------------------------------------------- shapes --- */

typedef struct RaceMreq
{
    ULONG   imr_multiaddr;
    ULONG   imr_interface;
} RaceMreq;

#define R_AF_INET            2
#define R_SOCK_DGRAM         2
#define R_IPPROTO_IP         0
#define R_IP_ADD_MEMBERSHIP  12
#define R_IP_DROP_MEMBERSHIP 13

#define R_EADDRINUSE        48
#define R_EADDRNOTAVAIL     49

/* Two administratively scoped groups (RFC 2365), so that a torn row can be
   seen to name the wrong one.  Nothing else on a test LAN uses them. */
#define R_GROUP_HAMMER  ((239UL << 24) | (255UL << 16) | (77UL << 8) | 1UL)
#define R_GROUP_SNIPER  ((239UL << 24) | (255UL << 16) | (77UL << 8) | 2UL)

typedef struct RaceStats
{
    ULONG   rs_Rounds;
    ULONG   rs_Joined;      /* joins that returned 0                        */
    ULONG   rs_Dropped;     /* drops that returned 0                        */
    ULONG   rs_Stolen;      /* drops of a held group that were refused      */
    ULONG   rs_JoinFail;    /* joins that were refused                      */
    LONG    rs_LastDropErr;
    LONG    rs_LastJoinErr;
} RaceStats;

/* ---------------------------------------------------------------- worker -- */

static LONG r_join(struct Library *sb, LONG s, ULONG group)
{
    RaceMreq mreq;

    mreq.imr_multiaddr = group;
    mreq.imr_interface = 0;

    return r_setsockopt(sb, s, R_IPPROTO_IP, R_IP_ADD_MEMBERSHIP,
                        &mreq, (LONG)sizeof(mreq));
}

static LONG r_drop(struct Library *sb, LONG s, ULONG group)
{
    RaceMreq mreq;

    mreq.imr_multiaddr = group;
    mreq.imr_interface = 0;

    return r_setsockopt(sb, s, R_IPPROTO_IP, R_IP_DROP_MEMBERSHIP,
                        &mreq, (LONG)sizeof(mreq));
}

/*
 * One side of the race.  nap is the sleep before each join and hold the sleep
 * between the join and the drop, both in ticks; zero for neither, which is
 * what makes HAMMER a hammer.
 */
static VOID r_run(struct Library *sb, LONG s, ULONG group, RaceStats *st,
                  ULONG rounds, ULONG nap, ULONG hold,
                  const volatile LONG *stop)
{
    ULONG i;

    for (i = 0; i < rounds; i++)
    {
        if (stop != NULL && *stop != 0)
            break;

        if (nap != 0)
            Delay(nap);

        st->rs_Rounds++;

        if (r_join(sb, s, group) != 0)
        {
            st->rs_JoinFail++;
            st->rs_LastJoinErr = r_errno(sb);
            continue;
        }

        st->rs_Joined++;

        if (hold != 0)
            Delay(hold);

        if (r_drop(sb, s, group) == 0)
        {
            st->rs_Dropped++;
        }
        else
        {
            /* The membership is held -- the join above returned 0 and nothing
               since has dropped it -- so the only way here is a row that now
               belongs to somebody else. */
            st->rs_Stolen++;
            st->rs_LastDropErr = r_errno(sb);
        }
    }
}

/* ----------------------------------------------------------- the sniper --- */

/*
 * Everything the child needs, in the segment both Processes share.  It opens
 * its OWN bsdsocket.library base: the defect is between two bases, and a
 * second socket on this one could not reach it.
 */
static struct
{
    struct Task    *rc_Parent;
    ULONG           rc_Signal;
    ULONG           rc_Rounds;
    ULONG           rc_Nap;
    ULONG           rc_Hold;
    volatile LONG   rc_Stop;
    volatile LONG   rc_Started;
    LONG            rc_OpenFailed;
    LONG            rc_SocketFailed;
    RaceStats       rc_Stats;
} sniper;

static VOID r_sniper(VOID)
{
    struct Library *sb;
    LONG            s;

    sb = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (sb == NULL)
    {
        sniper.rc_OpenFailed = 1;
    }
    else
    {
        s = r_socket(sb, R_AF_INET, R_SOCK_DGRAM, 0);
        if (s < 0)
        {
            sniper.rc_SocketFailed = 1;
        }
        else
        {
            sniper.rc_Started = 1;
            r_run(sb, s, R_GROUP_SNIPER, &sniper.rc_Stats,
                  sniper.rc_Rounds, sniper.rc_Nap, sniper.rc_Hold, NULL);
            (VOID)r_close(sb, s);
        }

        CloseLibrary(sb);
    }

    /*
     * The shots are spent, so HAMMER stops too: its own ceiling is only there
     * so that a sniper that died on its first call cannot leave it running for
     * ever.  Then the handshake, and nothing after it -- this Process exits
     * next and the parent is already free to look at the counters.
     */
    sniper.rc_Stop = 1;
    Signal(sniper.rc_Parent, sniper.rc_Signal);
}

/* ------------------------------------------------------------------ main -- */

#define TEMPLATE    "ROUNDS/N,SHOTS/N,NAP/N,HOLD/N,CPRI/N,PPRI/N"

enum
{
    ARG_ROUNDS = 0,
    ARG_SHOTS,
    ARG_NAP,
    ARG_HOLD,
    ARG_CPRI,
    ARG_PPRI,
    ARG_COUNT
};

/*
 * HAMMER rounds are cheap and SNIPER rounds cost two sleeps each, so they are
 * counted separately.  The defaults put the run at about twenty seconds:
 * SHOTS * (NAP + HOLD) ticks is the floor, and HAMMER is bounded only by
 * ROUNDS so that it is still hammering when the last shot goes off.
 */
#define DEF_ROUNDS  60000UL
#define DEF_SHOTS   400UL
#define DEF_NAP     1UL
#define DEF_HOLD    1UL
#define DEF_CPRI    2L
#define DEF_PPRI    0L

int main(void)
{
    struct Library  *sb;
    struct RDArgs   *rda;
    struct Process  *child;
    struct TagItem   tags[6];
    LONG             args[ARG_COUNT];
    RaceStats        hammer;
    LONG             s;
    BYTE             sig;
    ULONG            rounds = DEF_ROUNDS;
    ULONG            shots  = DEF_SHOTS;
    ULONG            nap    = DEF_NAP;
    ULONG            hold   = DEF_HOLD;
    LONG             cpri   = DEF_CPRI;
    LONG             ppri   = DEF_PPRI;
    LONG             oldpri;
    LONG             failed = 0;
    ULONG            i;

    for (i = 0; i < ARG_COUNT; i++)
        args[i] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda != NULL)
    {
        if (args[ARG_ROUNDS]) rounds = (ULONG)*(LONG *)args[ARG_ROUNDS];
        if (args[ARG_SHOTS])  shots  = (ULONG)*(LONG *)args[ARG_SHOTS];
        if (args[ARG_NAP])    nap    = (ULONG)*(LONG *)args[ARG_NAP];
        if (args[ARG_HOLD])   hold   = (ULONG)*(LONG *)args[ARG_HOLD];
        if (args[ARG_CPRI])   cpri   = *(LONG *)args[ARG_CPRI];
        if (args[ARG_PPRI])   ppri   = *(LONG *)args[ARG_PPRI];
        FreeArgs(rda);
    }

    for (i = 0; i < sizeof(hammer); i++)
        ((UBYTE *)&hammer)[i] = 0;
    for (i = 0; i < sizeof(sniper); i++)
        ((UBYTE *)&sniper)[i] = 0;

    Printf((CONST_STRPTR)"McastRace: %ld hammer rounds, %ld shots, "
                         "nap %ld hold %ld, priorities %ld/%ld\n",
           (LONG)rounds, (LONG)shots, (LONG)nap, (LONG)hold, ppri, cpri);

    sig = AllocSignal(-1);
    if (sig == -1)
    {
        Printf((CONST_STRPTR)"FAIL: no signal for the handshake\n");
        return RETURN_FAIL;
    }

    sb = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (sb == NULL)
    {
        Printf((CONST_STRPTR)"FAIL: no bsdsocket.library\n");
        FreeSignal(sig);
        return RETURN_FAIL;
    }

    s = r_socket(sb, R_AF_INET, R_SOCK_DGRAM, 0);
    if (s < 0)
    {
        Printf((CONST_STRPTR)"FAIL: socket() %ld\n", r_errno(sb));
        CloseLibrary(sb);
        FreeSignal(sig);
        return RETURN_FAIL;
    }

    sniper.rc_Parent = FindTask(NULL);
    sniper.rc_Signal = 1UL << sig;
    sniper.rc_Rounds = shots;
    sniper.rc_Nap    = nap;
    sniper.rc_Hold   = hold;

    SetSignal(0UL, sniper.rc_Signal);

    tags[0].ti_Tag  = NP_Entry;
    tags[0].ti_Data = (ULONG)r_sniper;
    tags[1].ti_Tag  = NP_Name;
    tags[1].ti_Data = (ULONG)"McastRace sniper";
    tags[2].ti_Tag  = NP_StackSize;
    tags[2].ti_Data = 8192;
    tags[3].ti_Tag  = NP_Priority;
    tags[3].ti_Data = (ULONG)cpri;
    tags[4].ti_Tag  = NP_Cli;
    tags[4].ti_Data = FALSE;
    tags[5].ti_Tag  = TAG_DONE;
    tags[5].ti_Data = 0;

    child = CreateNewProc(tags);
    if (child == NULL)
    {
        Printf((CONST_STRPTR)"FAIL: CreateNewProc\n");
        (VOID)r_close(sb, s);
        CloseLibrary(sb);
        FreeSignal(sig);
        return RETURN_FAIL;
    }

    /*
     * Below the child and below the ThreadX scheduler Task, which is the whole
     * mechanism: see the top.
     */
    oldpri = (LONG)SetTaskPri(FindTask(NULL), ppri);

    r_run(sb, s, R_GROUP_HAMMER, &hammer, rounds, 0, 0, &sniper.rc_Stop);

    /* HAMMER is done; let the child finish the shots it has left. */
    Wait(sniper.rc_Signal);

    (VOID)SetTaskPri(FindTask(NULL), oldpri);

    (VOID)r_close(sb, s);
    CloseLibrary(sb);
    FreeSignal(sig);

    /* ---- what happened --------------------------------------------------- */

    if (sniper.rc_OpenFailed != 0)
        Printf((CONST_STRPTR)"FAIL: the sniper could not open a second base\n");
    if (sniper.rc_SocketFailed != 0)
        Printf((CONST_STRPTR)"FAIL: the sniper could not make a socket\n");

    Printf((CONST_STRPTR)"hammer: %ld rounds, %ld joined, %ld dropped, "
                         "%ld stolen, %ld join failures (last errno %ld/%ld)\n",
           (LONG)hammer.rs_Rounds, (LONG)hammer.rs_Joined,
           (LONG)hammer.rs_Dropped, (LONG)hammer.rs_Stolen,
           (LONG)hammer.rs_JoinFail, hammer.rs_LastJoinErr,
           hammer.rs_LastDropErr);

    Printf((CONST_STRPTR)"sniper: %ld rounds, %ld joined, %ld dropped, "
                         "%ld stolen, %ld join failures (last errno %ld/%ld)\n",
           (LONG)sniper.rc_Stats.rs_Rounds, (LONG)sniper.rc_Stats.rs_Joined,
           (LONG)sniper.rc_Stats.rs_Dropped, (LONG)sniper.rc_Stats.rs_Stolen,
           (LONG)sniper.rc_Stats.rs_JoinFail, sniper.rc_Stats.rs_LastJoinErr,
           sniper.rc_Stats.rs_LastDropErr);

    /*
     * The line the harness gates on.  "did" is how much racing really
     * happened, so that a run which did nothing cannot pass by having found
     * nothing wrong.
     */
    Printf((CONST_STRPTR)"did: %ld hammer joins, %ld sniper joins\n",
           (LONG)hammer.rs_Joined, (LONG)sniper.rc_Stats.rs_Joined);

    if (hammer.rs_Stolen != 0)
    {
        Printf((CONST_STRPTR)"FAIL: the hammer held %ld memberships it could "
                             "not drop -- its row was taken by the other "
                             "base\n", (LONG)hammer.rs_Stolen);
        failed++;
    }

    if (sniper.rc_Stats.rs_Stolen != 0)
    {
        Printf((CONST_STRPTR)"FAIL: the sniper held %ld memberships it could "
                             "not drop -- its row was taken by the other "
                             "base\n", (LONG)sniper.rc_Stats.rs_Stolen);
        failed++;
    }

    /*
     * A join refused on a table with at most two rows in use is the same
     * defect seen from the other end: a row marked used by nobody, or an
     * EADDRINUSE against a membership this side already lost.
     */
    if (hammer.rs_JoinFail != 0 || sniper.rc_Stats.rs_JoinFail != 0)
    {
        Printf((CONST_STRPTR)"FAIL: %ld joins were refused on a table with "
                             "two rows in use\n",
               (LONG)(hammer.rs_JoinFail + sniper.rc_Stats.rs_JoinFail));
        failed++;
    }

    if (sniper.rc_OpenFailed != 0 || sniper.rc_SocketFailed != 0)
        failed++;

    Printf((CONST_STRPTR)"McastRace: %ld check(s), %ld failed\n",
           (LONG)3, (LONG)failed);

    return (failed == 0) ? RETURN_OK : RETURN_FAIL;
}
