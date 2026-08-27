/*
 * src/bsdsocket/library_runtime.c on the host: the usergroup.library hold.
 *
 * WHY THIS EXISTS.  bsd_usergroup_open() opens usergroup.library and never
 * closes it until the library is expunged (library_runtime.c:46).  Nothing in
 * this tree calls into that library, so nothing here needs the hold and every
 * reader of the file sees an open with no matching use.  The clients that need
 * it are ixemul's ixnet, which finds bsdsocket.library already holding
 * usergroup.library and calls it through its own base; if this tree tidied the
 * open away, those clients would stop working and nothing in this repository
 * would go red.  So the hold is asserted here.
 *
 * The whole translation unit is compiled, not a copy of the functions, so what
 * runs is the shipping bsd_usergroup_open(), bsd_runtime_open() and
 * bsd_runtime_close().  Every Exec and DOS call they make is scripted below
 * and counted; a call the file does not make today fails to link rather than
 * being answered by a default.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_internal.h"

#include <dos/dos.h>
#include <dos/dosextens.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long h_checks;
static unsigned long h_failures;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        h_checks++;                                                           \
        if (!(cond)) {                                                        \
            h_failures++;                                                     \
            printf("  FAIL %s\n", (what));                                    \
        }                                                                     \
    } while (0)

#define H_MAX_OPENS   8
#define H_MAX_CLOSES  8
#define H_MAX_LOCKS   8

/* Distinct non-NULL bases, so "which library is held" is answerable. */
static struct Library h_dos_lib;
static struct Library h_ug_lib;
static struct Library h_ug_amitcp_lib;

static struct Process h_proc;

static struct
{
    /* OpenLibrary() */
    const char     *open_name[H_MAX_OPENS];
    ULONG           open_version[H_MAX_OPENS];
    unsigned        opens;
    BOOL            have_usergroup;     /* LIBS:usergroup.library answers    */
    BOOL            have_amitcp_ug;     /* AmiTCP:libs/usergroup.library     */
    BOOL            have_dos;

    /* CloseLibrary() */
    struct Library *close_base[H_MAX_CLOSES];
    unsigned        closes;

    /* Lock()/UnLock()/AssignLock() */
    const char     *lock_name[H_MAX_LOCKS];
    APTR            lock_window[H_MAX_LOCKS];  /* pr_WindowPtr during Lock() */
    unsigned        locks;
    BOOL            have_amitcp_assign;  /* "AmiTCP:" already resolves       */
    BOOL            have_sys;            /* "SYS:" resolves                  */
    unsigned        unlocks;
    unsigned        assigns;
    const char     *assign_name;
    BPTR            assign_lock;
    BOOL            assign_fails;

    /* pr_WindowPtr as seen inside the fallback OpenLibrary() */
    APTR            open_window[H_MAX_OPENS];

    unsigned        timer_closes;
    unsigned        random_inits;

    BOOL            not_a_process;
} h;

static void h_reset(void)
{
    memset(&h, 0, sizeof(h));
    memset(&h_proc, 0, sizeof(h_proc));

    h_proc.pr_Task.tc_Node.ln_Type = NT_PROCESS;
    h_proc.pr_WindowPtr            = (APTR)0x5EA1ED;

    h.have_dos        = TRUE;
    h.have_usergroup  = TRUE;
    h.have_sys        = TRUE;
}

/* --------------------------------------------------------------- stubs -- */

struct Library *OpenLibrary(const UBYTE *libName, ULONG version)
{
    const char *name = (const char *)libName;

    if (h.opens < H_MAX_OPENS)
    {
        h.open_name[h.opens]    = name;
        h.open_version[h.opens] = version;
        h.open_window[h.opens]  = h_proc.pr_WindowPtr;
    }
    h.opens++;

    if (strcmp(name, "dos.library") == 0)
        return h.have_dos ? &h_dos_lib : NULL;

    if (strcmp(name, "usergroup.library") == 0)
        return h.have_usergroup ? &h_ug_lib : NULL;

    if (strcmp(name, "AmiTCP:libs/usergroup.library") == 0)
        return h.have_amitcp_ug ? &h_ug_amitcp_lib : NULL;

    return NULL;
}

VOID CloseLibrary(struct Library *library)
{
    if (h.closes < H_MAX_CLOSES)
        h.close_base[h.closes] = library;
    h.closes++;
}

struct Task *FindTask(const char *name)
{
    (VOID)name;

    if (h.not_a_process)
        h_proc.pr_Task.tc_Node.ln_Type = NT_TASK;

    return &h_proc.pr_Task;
}

BPTR Lock(const UBYTE *name, LONG type)
{
    const char *s = (const char *)name;

    (VOID)type;

    if (h.locks < H_MAX_LOCKS)
    {
        h.lock_name[h.locks]   = s;
        h.lock_window[h.locks] = h_proc.pr_WindowPtr;
    }
    h.locks++;

    if (strcmp(s, "AmiTCP:") == 0)
        return h.have_amitcp_assign ? (BPTR)0x1111 : (BPTR)0;

    if (strcmp(s, "SYS:") == 0)
        return h.have_sys ? (BPTR)0x2222 : (BPTR)0;

    return (BPTR)0;
}

VOID UnLock(BPTR lock)
{
    (VOID)lock;
    h.unlocks++;
}

LONG AssignLock(const UBYTE *name, BPTR lock)
{
    h.assigns++;
    h.assign_name = (const char *)name;
    h.assign_lock = lock;

    return h.assign_fails ? DOSFALSE : DOSTRUE;
}

VOID ami_timer_close(VOID)
{
    h.timer_closes++;
}

VOID ami_random_init(VOID)
{
    h.random_inits++;
}

VOID ami_random_srand(unsigned int seed)
{
    (VOID)seed;
}

int ami_random_rand(void)
{
    return 0;
}

/* ---------------------------------------------------------------- help -- */

static unsigned h_open_count(const char *name)
{
    unsigned i, n = 0;

    for (i = 0; i < h.opens && i < H_MAX_OPENS; i++)
    {
        if (strcmp(h.open_name[i], name) == 0)
            n++;
    }

    return n;
}

static unsigned h_close_count(struct Library *base)
{
    unsigned i, n = 0;

    for (i = 0; i < h.closes && i < H_MAX_CLOSES; i++)
    {
        if (h.close_base[i] == base)
            n++;
    }

    return n;
}

static int h_open_index(const char *name)
{
    unsigned i;

    for (i = 0; i < h.opens && i < H_MAX_OPENS; i++)
    {
        if (strcmp(h.open_name[i], name) == 0)
            return (int)i;
    }

    return -1;
}

static int h_lock_index(const char *name)
{
    unsigned i;

    for (i = 0; i < h.locks && i < H_MAX_LOCKS; i++)
    {
        if (strcmp(h.lock_name[i], name) == 0)
            return (int)i;
    }

    return -1;
}

/* --------------------------------------------------------------- tests -- */

/*
 * The hold itself.  One open, no close, and the base is still open after any
 * number of further calls: this is what an ixnet client finds.
 */
static void t_hold(void)
{
    printf("usergroup: the hold\n");

    h_reset();

    bsd_usergroup_open();

    CHECK(h_open_count("usergroup.library") == 1,
          "usergroup.library is opened once");
    CHECK(h.open_version[h_open_index("usergroup.library")] == 0,
          "opened at version 0, so any usergroup.library answers");
    CHECK(h.closes == 0,
          "the hold is not released by the open that took it");

    bsd_usergroup_open();
    bsd_usergroup_open();

    CHECK(h_open_count("usergroup.library") == 1,
          "a second open does not open the library again");
    CHECK(h_open_count("AmiTCP:libs/usergroup.library") == 0,
          "and does not reach for the fallback while the hold is live");
    CHECK(h.closes == 0, "and does not release the hold");
}

/*
 * Only expunge lets it go, and it lets go of exactly what it took.
 */
static void t_release_on_expunge(void)
{
    printf("usergroup: released only by bsd_runtime_close()\n");

    h_reset();
    h.have_dos = TRUE;

    (VOID)bsd_runtime_open();
    bsd_usergroup_open();

    CHECK(h.closes == 0, "still held after a library open");

    bsd_runtime_close();

    CHECK(h_close_count(&h_ug_lib) == 1,
          "expunge closes the usergroup base it opened");
    CHECK(h_close_count(&h_dos_lib) == 1,
          "and the dos.library base as well");
    CHECK(h.timer_closes == 1, "and timer.device before either");

    /* The pointer is cleared, so a second expunge cannot close it twice. */
    bsd_runtime_close();

    CHECK(h_close_count(&h_ug_lib) == 1,
          "a second close does not close the usergroup base again");
}

/*
 * A reload takes the hold again: bsd_amitcp_tried and the base pointer are
 * both file-scope statics, and a stale TRUE would skip the assign forever.
 */
static void t_reload_retakes(void)
{
    printf("usergroup: a reload takes it again\n");

    h_reset();

    bsd_usergroup_open();
    bsd_runtime_close();

    h.opens  = 0;
    h.closes = 0;
    h.locks  = 0;
    memset(h.open_name, 0, sizeof(h.open_name));
    memset(h.lock_name, 0, sizeof(h.lock_name));

    bsd_usergroup_open();

    CHECK(h_open_count("usergroup.library") == 1,
          "the second load opens usergroup.library again");
    CHECK(h_lock_index("AmiTCP:") >= 0,
          "and tries the AmiTCP: assign again");

    bsd_runtime_close();
}

/*
 * LIBS: first, AmiTCP:libs/ second.  A Roadshow machine has the first; an
 * AmiTCP 4.x installation has only the second, behind its own assign.
 */
static void t_fallback(void)
{
    printf("usergroup: the AmiTCP:libs fallback\n");

    h_reset();
    h.have_usergroup = FALSE;
    h.have_amitcp_ug = TRUE;

    bsd_usergroup_open();

    CHECK(h_open_count("usergroup.library") == 1,
          "LIBS: is tried first");
    CHECK(h_open_count("AmiTCP:libs/usergroup.library") == 1,
          "and AmiTCP:libs/ only after it fails");

    {
        int i = h_open_index("AmiTCP:libs/usergroup.library");

        CHECK(i >= 0 && h.open_window[i] == (APTR)-1L,
              "the fallback open runs with requesters off");
    }

    CHECK(h_proc.pr_WindowPtr == (APTR)0x5EA1ED,
          "and pr_WindowPtr is put back afterwards");
    CHECK(h.closes == 0, "the fallback base is held too");

    bsd_runtime_close();

    CHECK(h_close_count(&h_ug_amitcp_lib) == 1,
          "and expunge closes that one");
}

/*
 * Neither present.  Nothing is held, and expunge must not close NULL.
 */
static void t_absent(void)
{
    printf("usergroup: absent on both paths\n");

    h_reset();
    h.have_usergroup = FALSE;
    h.have_amitcp_ug = FALSE;

    bsd_usergroup_open();

    CHECK(h.closes == 0, "nothing was closed");
    CHECK(h_proc.pr_WindowPtr == (APTR)0x5EA1ED,
          "pr_WindowPtr is restored on the failing path too");

    bsd_runtime_close();

    CHECK(h_close_count(NULL) == 0, "expunge does not CloseLibrary(NULL)");

    /* A later load still finds it, so the failure is not remembered. */
    h.have_usergroup = TRUE;
    bsd_usergroup_open();
    CHECK(h_open_count("usergroup.library") == 2,
          "a later open retries LIBS:");

    bsd_runtime_close();
}

/*
 * The fallback needs pr_WindowPtr, which only a Process has.  On a plain Task
 * the second open is not attempted at all rather than written through a
 * structure that is not there.
 */
static void t_not_a_process(void)
{
    printf("usergroup: called from a plain Task\n");

    h_reset();
    h.have_usergroup = FALSE;
    h.have_amitcp_ug = TRUE;
    h.not_a_process  = TRUE;

    bsd_usergroup_open();

    CHECK(h_open_count("usergroup.library") == 1,
          "LIBS: is still tried");
    CHECK(h_open_count("AmiTCP:libs/usergroup.library") == 0,
          "the fallback is skipped without a Process");
    CHECK(h.assigns == 0, "and so is the assign");

    bsd_runtime_close();
}

/*
 * The AmiTCP: assign, which is what makes the fallback path reachable at all.
 */
static void t_amitcp_assign(void)
{
    printf("usergroup: the AmiTCP: assign\n");

    /* Nothing has claimed the name: assign it to SYS:. */
    h_reset();
    h.have_amitcp_assign = FALSE;

    bsd_usergroup_open();

    CHECK(h.assigns == 1, "AmiTCP: is assigned when nothing holds the name");
    CHECK(h.assign_name != NULL && strcmp(h.assign_name, "AmiTCP") == 0,
          "assigned without the colon, as AssignLock() wants it");
    CHECK(h.assign_lock == (BPTR)0x2222, "assigned to the SYS: lock");
    CHECK(h.unlocks == 0,
          "AssignLock() took the lock, so it is not unlocked as well");

    {
        int i = h_lock_index("AmiTCP:");

        CHECK(i >= 0 && h.lock_window[i] == (APTR)-1L,
              "the probe runs with requesters off, so no volume request");
    }
    CHECK(h_proc.pr_WindowPtr == (APTR)0x5EA1ED,
          "and pr_WindowPtr is put back");

    bsd_runtime_close();

    /* Somebody else already has it: leave it alone. */
    h_reset();
    h.have_amitcp_assign = TRUE;

    bsd_usergroup_open();

    CHECK(h.assigns == 0, "an existing AmiTCP: is not reassigned");
    CHECK(h.unlocks == 1, "and the probe lock is released");
    CHECK(h_lock_index("SYS:") < 0, "SYS: is not locked at all");

    bsd_runtime_close();

    /* AssignLock() refused: the lock is ours again and must be released. */
    h_reset();
    h.have_amitcp_assign = FALSE;
    h.assign_fails       = TRUE;

    bsd_usergroup_open();

    CHECK(h.assigns == 1, "the assign was attempted");
    CHECK(h.unlocks == 1, "and a refused AssignLock() leaves the lock to us");

    bsd_runtime_close();

    /* No SYS: at all: nothing to assign, and nothing leaks. */
    h_reset();
    h.have_amitcp_assign = FALSE;
    h.have_sys           = FALSE;

    bsd_usergroup_open();

    CHECK(h.assigns == 0, "no SYS: means no assign");
    CHECK(h.unlocks == 0, "and no lock to release");

    bsd_runtime_close();

    /* Once per load, however many bases open. */
    h_reset();
    h.have_amitcp_assign = FALSE;

    bsd_usergroup_open();
    bsd_usergroup_open();
    bsd_usergroup_open();

    CHECK(h.assigns == 1, "the assign is attempted once per load");

    bsd_runtime_close();
}

int main(void)
{
    printf("library_runtime.c host checks\n\n");

    t_hold();
    t_release_on_expunge();
    t_reload_retakes();
    t_fallback();
    t_absent();
    t_not_a_process();
    t_amitcp_assign();

    printf("\n%lu checks, %lu failures\n", h_checks, h_failures);

    return (h_failures == 0) ? 0 : 1;
}
