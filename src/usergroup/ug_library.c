/*
 * AmiNetXDuo, usergroup.library: romtag, library structure, vector table.
 *
 * ug_LibEntry() must be the first thing in the linked image so that running
 * the file as a program returns -1 instead of executing the library. Keep
 * this object first on the link line.
 *
 * SPDX-License-Identifier: MIT
 */

#include "usergroup_vectors.h"

#include "aminetxduo/compat.h"

#include <exec/execbase.h>
#include <exec/initializers.h>
#include <exec/resident.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stddef.h>

/*
 * The first code in the image, ahead of everything the compiler emits below.
 * A .library run as a program must fail with a return code, and must not fall
 * into the romtag. Written as top-level asm because GCC is free to order the
 * functions it generates in any way, and does.
 */
__asm__ (
    "   .text                   \n"
    "   .globl  _ug_LibEntry    \n"
    "_ug_LibEntry:              \n"
    "   moveq   #-1,d0          \n"
    "   rts                     \n"
);

/*
 * The library owns these: a shared library gets no C startup code, so nothing
 * else fills them in. They are per-image, not per-opener.
 */
struct ExecBase   *SysBase;
struct DosLibrary *DOSBase;

#define UG_VERSION   4
#define UG_REVISION  0

static char ug_lib_name[] = "usergroup.library";
static char ug_lib_id[]   = "usergroup.library 4.0 (AmiNetXDuo)\r\n";

/* ------------------------------------------------------- ABI assertions, */

/*
 * If any of these fire, the records handed back are not the ones that AmiTCP
 * and Roadshow clients were compiled against. See the ABI note in
 * usergroup_internal.h for where the reference layouts come from.
 */
_Static_assert(sizeof(struct ug_passwd) == 28,      "struct passwd ABI");
_Static_assert(offsetof(struct ug_passwd, pw_uid)   ==  8, "pw_uid");
_Static_assert(offsetof(struct ug_passwd, pw_gecos) == 16, "pw_gecos");
_Static_assert(offsetof(struct ug_passwd, pw_shell) == 24, "pw_shell");

_Static_assert(sizeof(struct ug_group) == 16,       "struct group ABI");
_Static_assert(offsetof(struct ug_group, gr_gid) ==  8, "gr_gid");
_Static_assert(offsetof(struct ug_group, gr_mem) == 12, "gr_mem");

_Static_assert(sizeof(struct ug_utmp)    == 104, "struct utmp ABI");
_Static_assert(sizeof(struct ug_lastlog) == 104, "struct lastlog ABI");

_Static_assert(offsetof(struct ug_credentials, cr_umask)   ==   8, "cr_umask");
_Static_assert(offsetof(struct ug_credentials, cr_euid)    ==  10, "cr_euid");
_Static_assert(offsetof(struct ug_credentials, cr_ngroups) ==  14, "cr_ngroups");
_Static_assert(offsetof(struct ug_credentials, cr_groups)  ==  16, "cr_groups");
_Static_assert(offsetof(struct ug_credentials, cr_session) == 144, "cr_session");
_Static_assert(offsetof(struct ug_credentials, cr_login)   == 148, "cr_login");

/* --------------------------------------------------------- tiny string, */

ULONG ug_strlen(const char *s)
{
    const char *p = s;

    if (s == NULL)
        return 0;
    while (*p != '\0')
        p++;

    return (ULONG)(p - s);
}

void ug_strncpy(char *dst, const char *src, ULONG size)
{
    ULONG i = 0;

    if (dst == NULL || size == 0)
        return;
    if (src != NULL)
    {
        while (i + 1 < size && src[i] != '\0')
        {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

int ug_strcmp(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
        return (a == b) ? 0 : 1;

    while (*a != '\0' && *a == *b)
    {
        a++;
        b++;
    }

    return (int)((unsigned char)*a) - (int)((unsigned char)*b);
}

/* NewList() lives in amiga.lib. A shared library open-codes it. */
void ug_newlist(struct MinList *list)
{
    list->mlh_Head     = (struct MinNode *)&list->mlh_Tail;
    list->mlh_Tail     = NULL;
    list->mlh_TailPred = (struct MinNode *)&list->mlh_Head;
}

/* ------------------------------------------------------------- dos base, */

/*
 * Opened on first use and not at init: the database files are optional, and a
 * caller that never touches them never pays for dos.library. Safe to call with
 * the global lock already held, because Exec semaphores nest per task.
 */
struct DosLibrary *ug_dos(struct UserGroupBase *base)
{
    struct UgGlobal *g = base->ug_Global;

    ObtainSemaphore(&g->lock);
    if (!g->dos_tried)
    {
        g->dos_tried = TRUE;
        g->dosbase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 36);
        DOSBase = g->dosbase;
        if (g->dosbase == NULL)
            AMI_WARN("usergroup: no dos.library, database files unavailable");
    }
    ReleaseSemaphore(&g->lock);

    return g->dosbase;
}

void ug_db_free(struct UgGlobal *g)
{
    if (g->db.pw_text != NULL)
    {
        ami_free(g->db.pw_text);
        g->db.pw_text = NULL;
    }
    if (g->db.gr_text != NULL)
    {
        ami_free(g->db.gr_text);
        g->db.gr_text = NULL;
    }
    if (g->db.gr_members != NULL)
    {
        ami_free(g->db.gr_members);
        g->db.gr_members = NULL;
    }
    g->db.pw_count  = 0;
    g->db.gr_count  = 0;
    g->db.pw_loaded = FALSE;
    g->db.gr_loaded = FALSE;
}

/* ----------------------------------------------------- standard vectors, */

static struct UserGroupBase *ug_LibOpen(UG_A6);
static BPTR ug_LibClose(UG_A6);
static BPTR ug_LibExpunge(UG_A6);
static LONG ug_LibReserved(UG_A6);

static struct UserGroupBase *ug_LibOpen(UG_A6)
{
    struct UserGroupBase *master = base;
    struct UserGroupBase *child;
    ULONG neg, pos;
    UBYTE *mem;

    /* Exec holds a Forbid() across this, so the counters need no lock. */
    master->lib.lib_Flags &= ~LIBF_DELEXP;

    if (master->ug_Global == NULL)
        return NULL;

    neg = master->lib.lib_NegSize;
    pos = master->lib.lib_PosSize;

    mem = ami_alloc(neg + pos);
    if (mem == NULL)
    {
        AMI_ERROR("usergroup: out of memory opening library");
        return NULL;
    }

    /*
     * Copy the jump table plus the Library header. Everything past the header
     * is the private state of this opener, and stays as ami_alloc() left it:
     * zero.
     */
    CopyMem((APTR)((UBYTE *)master - neg), mem, neg + sizeof(struct Library));

    child = (struct UserGroupBase *)(mem + neg);
    child->lib.lib_Node.ln_Succ = NULL;
    child->lib.lib_Node.ln_Pred = NULL;
    child->lib.lib_Flags        = 0;
    child->lib.lib_OpenCnt      = 1;
    child->lib.lib_Sum          = 0;

    child->ug_SysBase = master->ug_SysBase;
    child->ug_SegList = 0;
    child->ug_Parent  = master;
    child->ug_Global  = master->ug_Global;

    ug_context_init(child);

    /*
     * Forbid(), not ug_Global->lock. Exec calls this vector with a Forbid()
     * already held, and that lock is the database lock: ug_db.c holds it
     * across file reads and ug_dos() across an OpenLibrary(). A wait for it
     * here breaks the exec Forbid for the length of a disk access, while the
     * library list is meant to be frozen. The list this guards is short, and
     * ugl_getcredentials() walks it under the same Forbid().
     */
    Forbid();
    AddTail((struct List *)&master->ug_Global->children,
            (struct Node *)&child->ug_Node);
    Permit();

    master->lib.lib_OpenCnt++;

    return child;
}

static BPTR ug_LibClose(UG_A6)
{
    struct UserGroupBase *master = base->ug_Parent;

    if (master != NULL)
    {
        /* Forbid(), for the reason that ug_LibOpen() gives. */
        Forbid();
        Remove((struct Node *)&base->ug_Node);
        Permit();

        ami_free((UBYTE *)base - base->lib.lib_NegSize);
    }
    else
    {
        /* A caller closed the master base directly. Tolerate it. */
        master = base;
    }

    if (master->lib.lib_OpenCnt > 0)
        master->lib.lib_OpenCnt--;

    if (master->lib.lib_OpenCnt == 0 && (master->lib.lib_Flags & LIBF_DELEXP))
        return ug_LibExpunge(master);

    return 0;
}

static BPTR ug_LibExpunge(UG_A6)
{
    struct UserGroupBase *master = base->ug_Parent ? base->ug_Parent : base;
    struct UgGlobal *g;
    BPTR seglist;

    if (master->lib.lib_OpenCnt > 0)
    {
        master->lib.lib_Flags |= LIBF_DELEXP;
        return 0;
    }

    Remove((struct Node *)&master->lib.lib_Node);

    seglist = master->ug_SegList;

    g = master->ug_Global;
    if (g != NULL)
    {
        ug_db_free(g);
        if (g->dosbase != NULL)
        {
            CloseLibrary((struct Library *)g->dosbase);
            g->dosbase = NULL;
            DOSBase = NULL;
        }
        master->ug_Global = NULL;
        ami_free(g);
    }

    FreeMem((APTR)((UBYTE *)master - master->lib.lib_NegSize),
            master->lib.lib_NegSize + master->lib.lib_PosSize);

    return seglist;
}

static LONG ug_LibReserved(UG_A6)
{
    (void)base;
    return 0;
}

/* --------------------------------------------------------- vector table,
 *
 * Dense: slot 4 lands on bias 30 (ug_SetupContextTagList) and slot 42 on
 * -258 (getcredentials), exactly as the .fd and .sfd require. There is no gap
 * in the range, so no slot needs a failure stub. Every one of the 39 public
 * vectors below is a real implementation.
 */
static const APTR ug_func_table[] =
{
    (APTR)ug_LibOpen,                   /*  -6 */
    (APTR)ug_LibClose,                  /* -12 */
    (APTR)ug_LibExpunge,                /* -18 */
    (APTR)ug_LibReserved,               /* -24 */

    (APTR)ugl_SetupContextTagList,      /* -30  0x01e */
    (APTR)ugl_GetErr,                   /* -36  0x024 */
    (APTR)ugl_StrError,                 /* -42  0x02a */
    (APTR)ugl_getuid,                   /* -48  0x030 */
    (APTR)ugl_geteuid,                  /* -54  0x036 */
    (APTR)ugl_setreuid,                 /* -60  0x03c */
    (APTR)ugl_setuid,                   /* -66  0x042 */
    (APTR)ugl_getgid,                   /* -72  0x048 */
    (APTR)ugl_getegid,                  /* -78  0x04e */
    (APTR)ugl_setregid,                 /* -84  0x054 */
    (APTR)ugl_setgid,                   /* -90  0x05a */
    (APTR)ugl_getgroups,                /* -96  0x060 */
    (APTR)ugl_setgroups,                /* -102 0x066 */
    (APTR)ugl_initgroups,               /* -108 0x06c */
    (APTR)ugl_getpwnam,                 /* -114 0x072 */
    (APTR)ugl_getpwuid,                 /* -120 0x078 */
    (APTR)ugl_setpwent,                 /* -126 0x07e */
    (APTR)ugl_getpwent,                 /* -132 0x084 */
    (APTR)ugl_endpwent,                 /* -138 0x08a */
    (APTR)ugl_getgrnam,                 /* -144 0x090 */
    (APTR)ugl_getgrgid,                 /* -150 0x096 */
    (APTR)ugl_setgrent,                 /* -156 0x09c */
    (APTR)ugl_getgrent,                 /* -162 0x0a2 */
    (APTR)ugl_endgrent,                 /* -168 0x0a8 */
    (APTR)ugl_crypt,                    /* -174 0x0ae */
    (APTR)ugl_GetSalt,                  /* -180 0x0b4 */
    (APTR)ugl_getpass,                  /* -186 0x0ba */
    (APTR)ugl_umask,                    /* -192 0x0c0 */
    (APTR)ugl_getumask,                 /* -198 0x0c6 */
    (APTR)ugl_setsid,                   /* -204 0x0cc */
    (APTR)ugl_getpgrp,                  /* -210 0x0d2 */
    (APTR)ugl_getlogin,                 /* -216 0x0d8 */
    (APTR)ugl_setlogin,                 /* -222 0x0de */
    (APTR)ugl_setutent,                 /* -228 0x0e4 */
    (APTR)ugl_getutent,                 /* -234 0x0ea */
    (APTR)ugl_endutent,                 /* -240 0x0f0 */
    (APTR)ugl_getlastlog,               /* -246 0x0f6 */
    (APTR)ugl_setlastlog,               /* -252 0x0fc */
    (APTR)ugl_getcredentials,           /* -258 0x102 */

    (APTR)-1
};

/* 4 standard + 39 public + terminator. */
_Static_assert(sizeof(ug_func_table) / sizeof(ug_func_table[0]) == 44,
               "usergroup.library vector count (4 + 39 + terminator)");

/* ---------------------------------------------------------------- init --- */

static struct Library *ug_LibInit(register struct UserGroupBase *lib __asm("d0"),
                                  register BPTR seglist __asm("a0"),
                                  register struct ExecBase *sysbase __asm("a6"));

static struct Library *ug_LibInit(register struct UserGroupBase *lib __asm("d0"),
                                  register BPTR seglist __asm("a0"),
                                  register struct ExecBase *sysbase __asm("a6"))
{
    struct UgGlobal *g;

    /* Nothing else sets this, and every Exec call below depends on it. */
    SysBase = sysbase;

    lib->ug_SysBase = sysbase;
    lib->ug_SegList = seglist;
    lib->ug_Parent  = NULL;

    g = ami_alloc(sizeof(struct UgGlobal));
    if (g == NULL)
        return NULL;

    InitSemaphore(&g->lock);
    ug_newlist(&g->children);
    lib->ug_Global = g;

    AMI_INFO("usergroup.library %ld.%ld init", (LONG)UG_VERSION, (LONG)UG_REVISION);

    return (struct Library *)lib;
}

struct ug_init_table
{
    ULONG            it_DataSize;
    const APTR      *it_FuncTable;
    APTR             it_StructTable;
    APTR             it_InitFunc;
};

static const struct ug_init_table ug_init_table =
{
    (ULONG)sizeof(struct UserGroupBase),
    ug_func_table,
    NULL,
    (APTR)ug_LibInit
};

/* --------------------------------------------------------------- romtag, */

/* const so it lands in the code hunk, right behind ug_LibEntry(). */
extern const struct Resident ug_romtag;

const struct Resident ug_romtag =
{
    RTC_MATCHWORD,
    (struct Resident *)&ug_romtag,      /* rt_MatchTag: points at itself */
    (APTR)(&ug_romtag + 1),             /* rt_EndSkip:  one past the end */
    RTF_AUTOINIT,
    UG_VERSION,
    NT_LIBRARY,
    0,                          /* priority */
    ug_lib_name,
    ug_lib_id,
    (APTR)&ug_init_table
};
