/*
 * McastRace, two openers racing for the same row of the multicast table.
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
            st->rs_Stolen++;
            st->rs_LastDropErr = r_errno(sb);
        }
    }
}

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

    sniper.rc_Stop = 1;
    Signal(sniper.rc_Parent, sniper.rc_Signal);
}

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

    oldpri = (LONG)SetTaskPri(FindTask(NULL), ppri);

    r_run(sb, s, R_GROUP_HAMMER, &hammer, rounds, 0, 0, &sniper.rc_Stop);

    Wait(sniper.rc_Signal);

    (VOID)SetTaskPri(FindTask(NULL), oldpri);

    (VOID)r_close(sb, s);
    CloseLibrary(sb);
    FreeSignal(sig);

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

    Printf((CONST_STRPTR)"did: %ld hammer joins, %ld sniper joins\n",
           (LONG)hammer.rs_Joined, (LONG)sniper.rc_Stats.rs_Joined);

    if (hammer.rs_Stolen != 0)
    {
        Printf((CONST_STRPTR)"FAIL: the hammer held %ld memberships it could "
                             "not drop, its row was taken by the other "
                             "base\n", (LONG)hammer.rs_Stolen);
        failed++;
    }

    if (sniper.rc_Stats.rs_Stolen != 0)
    {
        Printf((CONST_STRPTR)"FAIL: the sniper held %ld memberships it could "
                             "not drop, its row was taken by the other "
                             "base\n", (LONG)sniper.rc_Stats.rs_Stolen);
        failed++;
    }

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
