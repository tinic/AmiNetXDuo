/*
 * AmiNetXDuo, usergroup.library: the passwd and group databases.
 *
 * AmigaOS has no user database, so the no-file path is the one that matters
 * and it is the one that must be perfect: every real system takes it. It
 * yields exactly one user, root, uid 0, gid 0, home SYS:, shell C:Shell,
 * and one group.
 *
 * If DEVS:Internet/passwd (or AmiTCP:db/passwd) does exist, it is parsed in
 * the ordinary /etc format and used instead. Reading is deliberately
 * self-contained: src/config/ owns netdb parsing, but this must not depend on
 * it, and it must never pull in newlib stdio.
 *
 * The parsed tables are shared by every opener and immutable once built; only
 * the iteration cursor and the returned record live in the per-opener base,
 * which is what makes the get*ent() iterators re-entrant across tasks.
 *
 * SPDX-License-Identifier: MIT
 */

#include "usergroup_vectors.h"

#include "ug_parse.h"

#include "aminetxduo/compat.h"

#include <dos/dosextens.h>
#include <proto/dos.h>
#include <proto/exec.h>

static const char *const ug_passwd_paths[] =
{
    "DEVS:Internet/passwd",
    "AmiTCP:db/passwd",
    NULL
};

static const char *const ug_group_paths[] =
{
    "DEVS:Internet/group",
    "AmiTCP:db/group",
    NULL
};

/* ---------------------------------------------------------- file access, */

/*
 * Read a whole small file through dos.library. Returns a NUL-terminated
 * ami_alloc() buffer, or NULL if the file is absent, unreadable, or larger
 * than UG_MAX_FILE. A missing file is not an error, it is the normal case.
 */
char *ug_db_read_file(struct UserGroupBase *base, const char *path, ULONG *len_out)
{
    struct DosLibrary *dos = ug_dos(base);
    struct Process *self;
    APTR   old_window;
    BPTR   fh;
    LONG   size;
    LONG   got;
    char  *buffer = NULL;

    if (len_out != NULL)
        *len_out = 0;

    if (dos == NULL)
        return NULL;

    /* Open() needs the pr_ fields; a plain Task has none. */
    self = (struct Process *)FindTask(NULL);
    if (self->pr_Task.tc_Node.ln_Type != NT_PROCESS)
        return NULL;

    /* No "please insert volume DEVS:" requester on the caller's screen. */
    old_window = self->pr_WindowPtr;
    self->pr_WindowPtr = (APTR)-1;

    fh = Open((STRPTR)path, MODE_OLDFILE);

    self->pr_WindowPtr = old_window;

    if (fh == 0)
        return NULL;

    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);   /* returns the old position */

    if (size > 0 && size <= UG_MAX_FILE)
    {
        buffer = ami_alloc((ULONG)size + 1);
        if (buffer != NULL)
        {
            got = Read(fh, buffer, size);
            if (got > 0)
            {
                buffer[got] = '\0';
                if (len_out != NULL)
                    *len_out = (ULONG)got;
            }
            else
            {
                ami_free(buffer);
                buffer = NULL;
            }
        }
    }
    else if (size > UG_MAX_FILE)
    {
        AMI_WARN("usergroup: %s too large (%ld bytes), ignored", path, size);
    }

    Close(fh);

    return buffer;
}

static char *ug_db_read_first(struct UserGroupBase *base,
                              const char *const *paths, ULONG *len_out)
{
    UWORD i;

    for (i = 0; paths[i] != NULL; i++)
    {
        char *text = ug_db_read_file(base, paths[i], len_out);

        if (text != NULL)
        {
            AMI_DEBUG("usergroup: using %s", paths[i]);
            return text;
        }
    }

    return NULL;
}

/* --------------------------------------------------------- passwd table, */

void ug_db_require_passwd(struct UserGroupBase *base)
{
    struct UgGlobal *g = base->ug_Global;

    ObtainSemaphore(&g->lock);

    if (!g->db.pw_loaded)
    {
        g->db.pw_loaded = TRUE;
        g->db.pw_text = ug_db_read_first(base, ug_passwd_paths, NULL);

        if (g->db.pw_text != NULL)
            ug_db_parse_passwd(&g->db, g->db.pw_text);
        else
            ug_db_default_passwd(&g->db);
    }

    ReleaseSemaphore(&g->lock);
}

/* ---------------------------------------------------------- group table, */

void ug_db_require_group(struct UserGroupBase *base)
{
    struct UgGlobal *g = base->ug_Global;

    ObtainSemaphore(&g->lock);

    if (!g->db.gr_loaded)
    {
        ULONG len = 0;

        g->db.gr_loaded = TRUE;
        g->db.gr_text = ug_db_read_first(base, ug_group_paths, &len);

        if (g->db.gr_text != NULL)
            ug_db_parse_group(&g->db, g->db.gr_text, len);
        else
            ug_db_default_group(&g->db);
    }

    ReleaseSemaphore(&g->lock);
}

/* -------------------------------------------------------- passwd vectors - */

/*
 * The record is copied into the opener's own base before being handed back:
 * callers routinely scribble on the struct they get, and the shared table
 * must stay pristine.
 */
static struct ug_passwd *ug_pw_return(struct UserGroupBase *base, UWORD index)
{
    base->ug_PwResult = base->ug_Global->db.pw[index];
    ug_set_err(base, 0);

    return &base->ug_PwResult;
}

struct ug_passwd *ugl_getpwnam(UG_A6, register STRPTR login __asm("a1"))
{
    struct UgDatabase *db;
    UWORD i;

    if (login == NULL)
    {
        ug_set_err(base, UG_EFAULT);
        return NULL;
    }

    ug_db_require_passwd(base);
    db = &base->ug_Global->db;

    for (i = 0; i < db->pw_count; i++)
    {
        if (ug_strcmp(db->pw[i].pw_name, (const char *)login) == 0)
            return ug_pw_return(base, i);
    }

    ug_set_err(base, UG_ENOENT);

    return NULL;
}

struct ug_passwd *ugl_getpwuid(UG_A6, register LONG uid __asm("d0"))
{
    struct UgDatabase *db;
    UWORD i;

    ug_db_require_passwd(base);
    db = &base->ug_Global->db;

    for (i = 0; i < db->pw_count; i++)
    {
        if (db->pw[i].pw_uid == uid)
            return ug_pw_return(base, i);
    }

    ug_set_err(base, UG_ENOENT);

    return NULL;
}

VOID ugl_setpwent(UG_A6)
{
    base->ug_PwCursor = 0;
}

struct ug_passwd *ugl_getpwent(UG_A6)
{
    ug_db_require_passwd(base);

    if (base->ug_PwCursor >= base->ug_Global->db.pw_count)
        return NULL;                    /* end of database */

    return ug_pw_return(base, base->ug_PwCursor++);
}

VOID ugl_endpwent(UG_A6)
{
    base->ug_PwCursor = 0;
}

/* --------------------------------------------------------- group vectors - */

static struct ug_group *ug_gr_return(struct UserGroupBase *base, UWORD index)
{
    base->ug_GrResult = base->ug_Global->db.gr[index];
    ug_set_err(base, 0);

    return &base->ug_GrResult;
}

struct ug_group *ugl_getgrnam(UG_A6, register STRPTR name __asm("a1"))
{
    struct UgDatabase *db;
    UWORD i;

    if (name == NULL)
    {
        ug_set_err(base, UG_EFAULT);
        return NULL;
    }

    ug_db_require_group(base);
    db = &base->ug_Global->db;

    for (i = 0; i < db->gr_count; i++)
    {
        if (ug_strcmp(db->gr[i].gr_name, (const char *)name) == 0)
            return ug_gr_return(base, i);
    }

    ug_set_err(base, UG_ENOENT);

    return NULL;
}

struct ug_group *ugl_getgrgid(UG_A6, register LONG gid __asm("d0"))
{
    struct UgDatabase *db;
    UWORD i;

    ug_db_require_group(base);
    db = &base->ug_Global->db;

    for (i = 0; i < db->gr_count; i++)
    {
        if (db->gr[i].gr_gid == gid)
            return ug_gr_return(base, i);
    }

    ug_set_err(base, UG_ENOENT);

    return NULL;
}

VOID ugl_setgrent(UG_A6)
{
    base->ug_GrCursor = 0;
}

struct ug_group *ugl_getgrent(UG_A6)
{
    ug_db_require_group(base);

    if (base->ug_GrCursor >= base->ug_Global->db.gr_count)
        return NULL;                    /* end of database */

    return ug_gr_return(base, base->ug_GrCursor++);
}

VOID ugl_endgrent(UG_A6)
{
    base->ug_GrCursor = 0;
}
