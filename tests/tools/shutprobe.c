/*
 * ShutProbe [command] [grace-seconds] -- measures what a network shutdown does
 * to processes holding bsdsocket.library open.  Prints key=value lines; exits
 * non-zero only when the measurement could not be taken at all.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <dos/dos.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>

extern struct ExecBase *SysBase;

/* ------------------------------------------------------------- vectors ---- */

static LONG call_socket(struct Library *base, LONG domain, LONG type, LONG proto)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = domain;
    register LONG            d1  __asm("d1") = type;
    register LONG            d2  __asm("d2") = proto;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-30:W)"          /* socket -0x01e */
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2)
                      : "a0", "a1", "cc", "memory");
    return res;
}

static LONG call_closesocket(struct Library *base, LONG s)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-120:W)"         /* CloseSocket -0x078 */
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0)
                      : "a0", "a1", "d2", "cc", "memory");
    return res;
}

static LONG call_errno(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)"         /* Errno -0x0a2 */
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static LONG call_waitselect(struct Library *base, LONG nfds, ULONG *readfds,
                            APTR tv, ULONG *sigs)
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

/* ---------------------------------------------------------------- clock --- */

static ULONG p_ticks(VOID)
{
    struct DateStamp ds;

    DateStamp(&ds);

    return (ULONG)ds.ds_Minute * 3000UL + (ULONG)ds.ds_Tick;
}

static ULONG p_ms(ULONG start, ULONG end)
{
    return (end - start) * 20UL;                /* one DateStamp tick is 20 ms */
}

/* -------------------------------------------------------------- holders --- */

/* Written by the holder process, read by the parent. */
struct Holder
{
    volatile LONG   sh_Started;     /* the process is running               */
    volatile LONG   sh_Opened;      /* it has bsdsocket.library open        */
    volatile LONG   sh_Blocked;     /* and is now where it means to wait    */
    volatile LONG   sh_Woke;        /* something got it out of that wait    */
    volatile LONG   sh_Break;       /* and that something was SIGBREAKF_CTRL_C */
    volatile LONG   sh_Closed;      /* the library has been given back      */
    volatile LONG   sh_Exited;      /* the process is on its way out        */
    volatile ULONG  sh_WokeAt;      /* p_ticks() when it came back          */
    volatile LONG   sh_Result;      /* what the blocking call returned      */
    volatile LONG   sh_Errno;
    struct Task    *sh_Task;        /* for the break of last resort         */
};

static struct Holder holder_select;
static struct Holder holder_wait;
static struct Holder holder_deaf;

static LONG holder_socket(struct Library *base, struct Holder *h)
{
    LONG s = call_socket(base, 2 /* AF_INET */, 2 /* SOCK_DGRAM */, 0);

    if (s < 0)
        h->sh_Errno = call_errno(base);

    return s;
}

/* Blocks inside the library: WaitSelect(), no timeout, break mask in signals. */
static VOID holder_select_entry(VOID)
{
    struct Holder  *h = &holder_select;
    struct Library *base;
    ULONG           readfds = 0;
    ULONG           sigs    = SIGBREAKF_CTRL_C;
    LONG            s;

    h->sh_Task    = FindTask(NULL);
    h->sh_Started = 1;

    base = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    if (base == NULL)
    {
        h->sh_Exited = 1;
        return;
    }
    h->sh_Opened = 1;

    s = holder_socket(base, h);
    if (s >= 0)
    {
        readfds = 1UL << (ULONG)s;

        h->sh_Blocked = 1;
        h->sh_Result  = call_waitselect(base, s + 1, &readfds, NULL, &sigs);
        h->sh_Errno   = call_errno(base);
        h->sh_WokeAt  = p_ticks();
        h->sh_Break   = ((sigs & SIGBREAKF_CTRL_C) != 0) ? 1 : 0;
        h->sh_Woke    = 1;

        (VOID)call_closesocket(base, s);
    }
    else
    {
        h->sh_Blocked = 1;
        h->sh_Woke    = 1;
    }

    CloseLibrary(base);
    h->sh_Closed = 1;
    h->sh_Exited = 1;
}

/* Blocks outside the library: holds a socket, waits in Wait(). */
static VOID holder_wait_entry(VOID)
{
    struct Holder  *h = &holder_wait;
    struct Library *base;
    LONG            s;
    ULONG           got;

    h->sh_Task    = FindTask(NULL);
    h->sh_Started = 1;

    base = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    if (base == NULL)
    {
        h->sh_Exited = 1;
        return;
    }
    h->sh_Opened = 1;

    s = holder_socket(base, h);

    h->sh_Blocked = 1;
    got           = Wait(SIGBREAKF_CTRL_C);
    h->sh_WokeAt  = p_ticks();
    h->sh_Break   = ((got & SIGBREAKF_CTRL_C) != 0) ? 1 : 0;
    h->sh_Woke    = 1;

    if (s >= 0)
        (VOID)call_closesocket(base, s);

    CloseLibrary(base);
    h->sh_Closed = 1;
    h->sh_Exited = 1;
}

/* Uncooperative holder: waits on SIGF_SINGLE, so SIGBREAKF_CTRL_C does nothing. */
static VOID holder_deaf_entry(VOID)
{
    struct Holder  *h = &holder_deaf;
    struct Library *base;
    LONG            s;

    h->sh_Task    = FindTask(NULL);
    h->sh_Started = 1;

    base = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    if (base == NULL)
    {
        h->sh_Exited = 1;
        return;
    }
    h->sh_Opened = 1;

    s = holder_socket(base, h);

    h->sh_Blocked = 1;
    (VOID)Wait(SIGF_SINGLE);            /* the probe's own release, at the end */
    h->sh_WokeAt  = p_ticks();
    h->sh_Woke    = 1;

    if (s >= 0)
        (VOID)call_closesocket(base, s);

    CloseLibrary(base);
    h->sh_Closed = 1;
    h->sh_Exited = 1;
}

/* ------------------------------------------------------- the open count --- */

/* Must read the master base via LibList: OpenLibrary() here would itself bump
   lib_OpenCnt and restart the stack, destroying the measurement. */
static struct Library *find_bsdsocket(VOID)
{
    struct Library *lib;

    Forbid();
    lib = (struct Library *)FindName(&SysBase->LibList, (STRPTR)"bsdsocket.library");
    Permit();

    return lib;
}

static LONG open_count(struct Library *lib)
{
    return (lib != NULL) ? (LONG)lib->lib_OpenCnt : -1;
}

/* ----------------------------------------------------------------- main --- */

static LONG both_reached(const struct Holder *a, const struct Holder *b,
                         LONG offset)
{
    const volatile LONG *pa = (const volatile LONG *)((const UBYTE *)a + offset);
    const volatile LONG *pb = (const volatile LONG *)((const UBYTE *)b + offset);

    return (*pa != 0 && *pb != 0) ? 1 : 0;
}

static LONG wait_for(LONG offset, ULONG ticks)
{
    ULONG spent = 0;

    while (!both_reached(&holder_select, &holder_wait, offset))
    {
        if (spent >= ticks)
            return 0;
        Delay(5);
        spent += 5;
    }

    return 1;
}

static VOID kv(const char *key, LONG value)
{
    Printf((CONST_STRPTR)"%s=%ld\n", (LONG)key, value);
}

/* Must use GetArgStr(): a process started by SystemTagList() gets its
   arguments there, and argv is empty. */
#define P_MAX_WORDS 4

static char  p_line[128];
static char *p_word[P_MAX_WORDS];

static LONG p_split(VOID)
{
    const char *src = (const char *)GetArgStr();
    LONG        n   = 0;
    LONG        i   = 0;
    LONG        len = 0;

    if (src == NULL)
        return 0;

    while (src[len] != '\0' && src[len] != '\n' &&
           len < (LONG)sizeof(p_line) - 1)
    {
        p_line[len] = src[len];
        len++;
    }
    p_line[len] = '\0';

    while (i < len && n < P_MAX_WORDS)
    {
        while (i < len && (p_line[i] == ' ' || p_line[i] == '\t'))
            p_line[i++] = '\0';

        if (i >= len)
            break;

        p_word[n++] = &p_line[i];

        while (i < len && p_line[i] != ' ' && p_line[i] != '\t')
            i++;
    }

    return n;
}

int main(int argc, char **argv)
{
    LONG            words   = p_split();
    const char     *command = (words > 0) ? p_word[0] : "SYS:NetShutdown";
    ULONG           grace   = 10UL;
    BOOL            deaf    = (words > 2) ? TRUE : FALSE;
    struct Library *lib;
    LONG            before;
    LONG            after;
    LONG            rc;
    ULONG           start;
    ULONG           ran;

    (VOID)argc;
    (VOID)argv;

    if (words > 1)
    {
        const char *p = p_word[1];

        grace = 0;
        while (*p >= '0' && *p <= '9')
            grace = grace * 10UL + (ULONG)(*p++ - '0');
        if (grace == 0)
            grace = 10UL;
    }

    lib = find_bsdsocket();
    if (lib == NULL)
    {
        Printf((CONST_STRPTR)"error=bsdsocket.library is not loaded\n");
        return RETURN_FAIL;
    }

    /* ---- 1: two holders, both of them waiting -------------------------- */

    if (CreateNewProcTags(NP_Entry,     (ULONG)holder_select_entry,
                          NP_Name,      (ULONG)"ShutProbe holder (WaitSelect)",
                          NP_StackSize, 8192UL,
                          TAG_DONE) == NULL ||
        CreateNewProcTags(NP_Entry,     (ULONG)holder_wait_entry,
                          NP_Name,      (ULONG)"ShutProbe holder (Wait)",
                          NP_StackSize, 8192UL,
                          TAG_DONE) == NULL)
    {
        Printf((CONST_STRPTR)"error=cannot start the holders\n");
        return RETURN_FAIL;
    }

    if (deaf &&
        CreateNewProcTags(NP_Entry,     (ULONG)holder_deaf_entry,
                          NP_Name,      (ULONG)"ShutProbe holder (deaf)",
                          NP_StackSize, 8192UL,
                          TAG_DONE) == NULL)
    {
        Printf((CONST_STRPTR)"error=cannot start the deaf holder\n");
        return RETURN_FAIL;
    }

    if (!wait_for((LONG)offsetof(struct Holder, sh_Blocked), 250UL))
    {
        Printf((CONST_STRPTR)"error=the holders never reached their wait\n");
        kv("select_opened", holder_select.sh_Opened);
        kv("wait_opened",   holder_wait.sh_Opened);
        return RETURN_FAIL;
    }

    if (deaf)
    {
        ULONG spent = 0;

        while (holder_deaf.sh_Blocked == 0 && spent < 250UL)
        {
            Delay(5);
            spent += 5;
        }

        if (holder_deaf.sh_Blocked == 0)
        {
            Printf((CONST_STRPTR)"error=the deaf holder never reached its wait\n");
            return RETURN_FAIL;
        }
    }

    /* Cover the gap between setting sh_Blocked and actually entering the wait. */
    Delay(10);

    before = open_count(lib);

    kv("holders", deaf ? 3 : 2);
    kv("deaf_holder", deaf ? 1 : 0);
    kv("opencnt_before", before);

    /* ---- 2: the command ------------------------------------------------ */

    Printf((CONST_STRPTR)"command=%s\n", (LONG)command);
    Flush(Output());

    start = p_ticks();
    rc    = SystemTagList((CONST_STRPTR)command, NULL);
    ran   = p_ticks();

    kv("command_rc", rc);
    kv("command_ms", (LONG)p_ms(start, ran));

    /* ---- 3: the grace period ------------------------------------------- */

    (VOID)wait_for((LONG)offsetof(struct Holder, sh_Closed), grace * 50UL);

    kv("grace_s",             (LONG)grace);
    kv("select_broken",       holder_select.sh_Break);
    kv("select_woke",         holder_select.sh_Woke);
    kv("select_closed",       holder_select.sh_Closed);
    kv("select_result",       holder_select.sh_Result);
    kv("select_errno",        holder_select.sh_Errno);
    kv("wait_broken",         holder_wait.sh_Break);
    kv("wait_woke",           holder_wait.sh_Woke);
    kv("wait_closed",         holder_wait.sh_Closed);

    if (holder_select.sh_Woke != 0)
        kv("select_broken_after_ms",
           (LONG)p_ms(start, holder_select.sh_WokeAt));
    if (holder_wait.sh_Woke != 0)
        kv("wait_broken_after_ms", (LONG)p_ms(start, holder_wait.sh_WokeAt));

    if (deaf)
    {
        kv("deaf_still_holding", (holder_deaf.sh_Closed == 0) ? 1 : 0);
    }

    after = open_count(lib);
    kv("opencnt_after", after);
    kv("opencnt_dropped", (before >= 0 && after >= 0) ? (before - after) : -1);

    /* ---- 4: clean up after the measurement ----------------------------- */

    {
        LONG manual = 0;

        if (deaf && holder_deaf.sh_Task != NULL)
            Signal(holder_deaf.sh_Task, SIGF_SINGLE);

        if (holder_select.sh_Closed == 0 && holder_select.sh_Task != NULL)
        {
            Signal(holder_select.sh_Task, SIGBREAKF_CTRL_C);
            manual++;
        }
        if (holder_wait.sh_Closed == 0 && holder_wait.sh_Task != NULL)
        {
            Signal(holder_wait.sh_Task, SIGBREAKF_CTRL_C);
            manual++;
        }

        kv("holders_needing_manual_break", manual);

        if (manual != 0)
        {
            LONG freed = wait_for((LONG)offsetof(struct Holder, sh_Closed),
                                  250UL);

            kv("holders_freed_by_hand", freed);
        }
    }

    kv("opencnt_final", open_count(lib));

    /* The holders run out of this program's segment; the Shell unloads it the
       moment main() returns, so they must all be gone first. */
    (VOID)wait_for((LONG)offsetof(struct Holder, sh_Exited), 250UL);

    if (deaf)
    {
        ULONG spent = 0;

        while (holder_deaf.sh_Exited == 0 && spent < 250UL)
        {
            Delay(5);
            spent += 5;
        }
    }

    Delay(50);

    Printf((CONST_STRPTR)"done=1\n");

    return RETURN_OK;
}
