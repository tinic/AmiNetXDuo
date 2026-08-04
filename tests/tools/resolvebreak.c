/*
 * ResolveBreak, how long a name lookup blocks, and whether Ctrl-C gets it
 * back.
 *
 * The Roadshow autodoc requires a blocking operation to be abortable:
 * SetSocketSignals says SIGINT "is the signal to send to the process which
 * owns the socket in order to abort a blocking operation".  recv() and its
 * neighbours honour that; gethostbyname() did not, and the wait it could not
 * be taken out of was minutes rather than the seconds the caller asked for.
 *
 * Neither half can be shown by a build or by a lookup against a working name
 * server.  It needs a server that answers nothing, a clock, and something to
 * press Ctrl-C in the middle, so this points the resolver at an address on a
 * network reserved for documentation (RFC 5737 TEST-NET-1, 192.0.2.0/24), which
 * nothing routes and nothing answers, and times three arms:
 *
 *   1  the whole lookup, uninterrupted.  What the caller asked for is the
 *      ceiling; anything above it is somebody else's retry loop.
 *   2  the same lookup with the break signal already set.  It must come back
 *      without querying anything.
 *   3  the same lookup with the break signal arriving in the middle, sent by a
 *      child process.  The gap between sending it and the lookup returning is
 *      the number that decides whether Ctrl-C feels like it works.
 *
 * Everything goes through the published LVOs.  Nothing of ours is linked, so
 * the same binary measures any bsdsocket.library, which is the point: the
 * before and after numbers come from one probe.
 *
 *   ResolveBreak [blackhole-address] [name] [seconds-before-break]
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <dos/dos.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>

/* ------------------------------------------------------------- vectors ---- */

static LONG p_errno(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)"     /* Errno -0x0a2 */
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static APTR p_gethostbyname(struct Library *base, const char *name)
{
    register struct Library *a6  __asm("a6") = base;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)name;
    register APTR            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-210:W)"     /* gethostbyname -0x0d2 */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_add_dns(struct Library *base, const char *address)
{
    register struct Library *a6  __asm("a6") = base;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)address;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-516:W)"     /* AddDomainNameServer -0x204 */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_remove_dns(struct Library *base, const char *address)
{
    register struct Library *a6  __asm("a6") = base;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)address;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-522:W)"     /* RemoveDomainNameServer -0x20a */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "cc", "memory");
    return res;
}

static struct List *p_obtain_dns_list(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register struct List    *res __asm("d0");

    __asm __volatile ("jsr a6@(-534:W)"     /* ObtainDomainNameServerList */
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static VOID p_release_dns_list(struct Library *base, struct List *list)
{
    register struct Library *a6 __asm("a6") = base;
    register APTR            a0 __asm("a0") = (APTR)list;
    register LONG _clob_d0 __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-528:W)"     /* ReleaseDomainNameServerList */
                      : "=r" (_clob_d0), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "cc", "memory");
}

/* ---------------------------------------------------------------- clock --- */

/*
 * DateStamp() rather than the VBlank counter: ds_Tick is 1/50 s and the
 * intervals here are seconds, and it is the same clock on every model.
 */
static ULONG p_ticks(VOID)
{
    struct DateStamp ds;

    DateStamp(&ds);

    return (ULONG)ds.ds_Minute * 3000UL + (ULONG)ds.ds_Tick;
}

static VOID p_report(const char *what, ULONG start, ULONG end)
{
    ULONG ticks = end - start;

    Printf((CONST_STRPTR)"  %s: %ld.%02ld s (%ld ticks)\n", (LONG)what,
           (LONG)(ticks / 50UL), (LONG)((ticks % 50UL) * 2UL), (LONG)ticks);
}

/* --------------------------------------------------------- the Ctrl-C ----- */

/*
 * A child process that waits and then signals the parent, which is what a user
 * pressing Ctrl-C does.  A process rather than a task: Delay() needs a
 * pr_MsgPort, and a bare task has none.
 */
static struct Task *p_parent;
static ULONG        p_delay;

static VOID p_breaker(VOID)
{
    Delay((LONG)p_delay);
    Signal(p_parent, SIGBREAKF_CTRL_C);
}

/* ----------------------------------------------------------------- main --- */

int main(int argc, char **argv)
{
    struct Library *base;
    const char     *blackhole = (argc > 1) ? argv[1] : "192.0.2.1";
    const char     *name      = (argc > 2) ? argv[2] : "probe.invalid";
    ULONG           delay     = 5UL;
    ULONG           start;
    ULONG           sent;
    APTR            he;
    LONG            failures = 0;

    if (argc > 3)
    {
        const char *p = argv[3];

        delay = 0;
        while (*p >= '0' && *p <= '9')
            delay = delay * 10UL + (ULONG)(*p++ - '0');
        if (delay == 0)
            delay = 5UL;
    }

    Printf((CONST_STRPTR)"ResolveBreak: %s through %s\n", (LONG)name, (LONG)blackhole);

    base = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    if (base == NULL)
    {
        Printf((CONST_STRPTR)"FAIL cannot open bsdsocket.library\n");
        return RETURN_FAIL;
    }

    /*
     * Leave exactly one name server, and let it be one that cannot answer.
     * The list has to be walked to a private copy first, removing from it
     * while holding it would be walking a list that is being changed.
     */
    {
        struct List *list = p_obtain_dns_list(base);
        char         found[8][32];
        LONG         n = 0;

        if (list != NULL)
        {
            struct MinNode *node = (struct MinNode *)list->lh_Head;

            while (node != NULL && node->mln_Succ != NULL && n < 8)
            {
                /* struct DomainNameServerNode, as libraries/bsdsocket.h has
                   it: a MinNode, so there is no ln_Name to read. */
                const struct
                {
                    struct MinNode  dnsn_MinNode;
                    LONG            dnsn_Size;
                    char           *dnsn_Address;
                    LONG            dnsn_UseCount;
                } *dns = (VOID *)node;

                if (dns->dnsn_Address != NULL)
                {
                    LONG i;

                    for (i = 0; i < 31 && dns->dnsn_Address[i] != '\0'; i++)
                        found[n][i] = dns->dnsn_Address[i];
                    found[n][i] = '\0';
                    n++;
                }

                node = node->mln_Succ;
            }

            p_release_dns_list(base, list);
        }

        /* 0 is success here; the autodoc's failures are -1 with errno set. */
        if (p_add_dns(base, (char *)blackhole) != 0)
            Printf((CONST_STRPTR)"  WARNING AddDomainNameServer(%s) refused, errno %ld\n",
                   (LONG)blackhole, (LONG)p_errno(base));

        while (n-- > 0)
        {
            Printf((CONST_STRPTR)"  dropping name server %s\n", (LONG)found[n]);
            (VOID)p_remove_dns(base, found[n]);
        }
    }

    /* ---- 1: the whole lookup ------------------------------------------- */

    Printf((CONST_STRPTR)"\n1  uninterrupted\n");

    (VOID)SetSignal(0UL, SIGBREAKF_CTRL_C);

    start = p_ticks();
    he    = p_gethostbyname(base, name);
    p_report("blocked for", start, p_ticks());
    Printf((CONST_STRPTR)"  answer %s, errno %ld\n",
           (LONG)((he != NULL) ? "found" : "none"), (LONG)p_errno(base));

    if (he != NULL)
    {
        Printf((CONST_STRPTR)"FAIL a black-holed server answered, pick another address\n");
        failures++;
    }

    /* ---- 2: the break already set --------------------------------------- */

    Printf((CONST_STRPTR)"\n2  break already pending\n");

    (VOID)SetSignal(SIGBREAKF_CTRL_C, SIGBREAKF_CTRL_C);

    start = p_ticks();
    he    = p_gethostbyname(base, name);
    p_report("blocked for", start, p_ticks());
    Printf((CONST_STRPTR)"  answer %s, errno %ld\n",
           (LONG)((he != NULL) ? "found" : "none"), (LONG)p_errno(base));

    if ((p_ticks() - start) > 100UL)
    {
        Printf((CONST_STRPTR)"FAIL a pending break should not have started a query\n");
        failures++;
    }

    /*
     * The signal is read and not consumed, so the program's own Ctrl-C
     * handling still sees it.  Clear it here or arm 3 measures nothing.
     */
    if ((SetSignal(0UL, 0UL) & SIGBREAKF_CTRL_C) == 0)
    {
        Printf((CONST_STRPTR)"FAIL the break signal was consumed by the lookup\n");
        failures++;
    }
    (VOID)SetSignal(0UL, SIGBREAKF_CTRL_C);

    /* ---- 3: the break arriving in the middle ---------------------------- */

    Printf((CONST_STRPTR)"\n3  break after %ld s\n", (LONG)delay);

    p_parent = FindTask(NULL);
    p_delay  = delay * 50UL;

    if (CreateNewProcTags(NP_Entry, (ULONG)p_breaker,
                          NP_Name,  (ULONG)"ResolveBreak helper",
                          NP_StackSize, 4096UL,
                          TAG_DONE) == NULL)
    {
        Printf((CONST_STRPTR)"  cannot start the helper, skipping\n");
    }
    else
    {
        start = p_ticks();
        sent  = start + p_delay;

        he = p_gethostbyname(base, name);

        {
            ULONG done = p_ticks();

            p_report("blocked for", start, done);
            p_report("after the break", sent, done);
            Printf((CONST_STRPTR)"  answer %s, errno %ld\n",
                   (LONG)((he != NULL) ? "found" : "none"),
                   (LONG)p_errno(base));

            if (done < sent)
            {
                Printf((CONST_STRPTR)"FAIL the lookup ended before the break was sent\n");
                failures++;
            }
        }

        (VOID)SetSignal(0UL, SIGBREAKF_CTRL_C);
    }

    CloseLibrary(base);

    Printf((CONST_STRPTR)"\n%ld failure(s)\n", (LONG)failures);

    return (failures == 0) ? RETURN_OK : RETURN_FAIL;
}
