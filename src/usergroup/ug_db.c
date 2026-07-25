/*
 * AmiNetXDuo -- usergroup.library: the passwd and group databases.
 *
 * AmigaOS has no user database, so the no-file path is the one that matters
 * and it is the one that must be perfect: every real system takes it. It
 * yields exactly one user -- root, uid 0, gid 0, home SYS:, shell C:Shell --
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

#include "aminetxduo/compat.h"

#include <dos/dosextens.h>
#include <proto/dos.h>
#include <proto/exec.h>

/* ------------------------------------------------------------ defaults --- */

static char ug_def_name[]  = "root";
static char ug_def_empty[] = "";
static char ug_def_gecos[] = "AmigaOS user";
static char ug_def_dir[]   = "SYS:";
static char ug_def_shell[] = "C:Shell";

static char *ug_def_members[] = { ug_def_name, NULL };

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

/* -------------------------------------------------------------- parsing -- */

static char *ug_next_line(char **cursor)
{
    char *s = *cursor;
    char *line;

    if (s == NULL || *s == '\0')
    {
        *cursor = NULL;
        return NULL;
    }

    line = s;
    while (*s != '\0' && *s != '\n' && *s != '\r')
        s++;

    if (*s != '\0')
    {
        *s++ = '\0';
        while (*s == '\n' || *s == '\r')
            s++;
    }

    *cursor = s;

    return line;
}

/* In-place ':' or ',' split. Returns NULL once the line is exhausted. */
static char *ug_next_field(char **cursor, char sep)
{
    char *s = *cursor;
    char *field;

    if (s == NULL)
        return NULL;

    field = s;
    while (*s != '\0' && *s != sep)
        s++;

    if (*s == sep)
    {
        *s++ = '\0';
        *cursor = s;
    }
    else
    {
        *cursor = NULL;
    }

    return field;
}

static char *ug_field(char **cursor, char sep)
{
    char *f = ug_next_field(cursor, sep);

    return (f != NULL) ? f : ug_def_empty;
}

static LONG ug_atol(const char *s)
{
    LONG value = 0;
    BOOL negative = FALSE;

    if (s == NULL)
        return 0;

    while (*s == ' ' || *s == '\t')
        s++;

    if (*s == '-')
    {
        negative = TRUE;
        s++;
    }
    else if (*s == '+')
    {
        s++;
    }

    while (*s >= '0' && *s <= '9')
        value = value * 10 + (LONG)(*s++ - '0');

    return negative ? -value : value;
}

/* ---------------------------------------------------------- file access -- */

/*
 * Read a whole small file through dos.library. Returns a NUL-terminated
 * ami_alloc() buffer, or NULL if the file is absent, unreadable, or larger
 * than UG_MAX_FILE. A missing file is not an error -- it is the normal case.
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

/* --------------------------------------------------------- passwd table -- */

static void ug_db_default_passwd(struct UgDatabase *db)
{
    db->pw[0].pw_name   = ug_def_name;
    db->pw[0].pw_passwd = ug_def_empty;
    db->pw[0].pw_uid    = 0;
    db->pw[0].pw_gid    = 0;
    db->pw[0].pw_gecos  = ug_def_gecos;
    db->pw[0].pw_dir    = ug_def_dir;
    db->pw[0].pw_shell  = ug_def_shell;
    db->pw_count = 1;
}

static void ug_db_parse_passwd(struct UgDatabase *db, char *text)
{
    char *cursor = text;
    char *line;

    while (db->pw_count < UG_MAX_PASSWD && (line = ug_next_line(&cursor)) != NULL)
    {
        struct ug_passwd *pw;
        char *field = line;
        char *name;

        if (*line == '\0' || *line == '#')
            continue;

        name = ug_field(&field, ':');
        if (*name == '\0')
            continue;

        pw = &db->pw[db->pw_count];
        pw->pw_name   = name;
        pw->pw_passwd = ug_field(&field, ':');
        pw->pw_uid    = ug_atol(ug_field(&field, ':'));
        pw->pw_gid    = ug_atol(ug_field(&field, ':'));
        pw->pw_gecos  = ug_field(&field, ':');
        pw->pw_dir    = ug_field(&field, ':');
        pw->pw_shell  = ug_field(&field, ':');

        if (*pw->pw_dir == '\0')
            pw->pw_dir = ug_def_dir;
        if (*pw->pw_shell == '\0')
            pw->pw_shell = ug_def_shell;

        db->pw_count++;
    }

    if (db->pw_count == 0)
        ug_db_default_passwd(db);
}

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

/* ---------------------------------------------------------- group table -- */

static void ug_db_default_group(struct UgDatabase *db)
{
    db->gr[0].gr_name   = ug_def_name;
    db->gr[0].gr_passwd = ug_def_empty;
    db->gr[0].gr_gid    = 0;
    db->gr[0].gr_mem    = ug_def_members;
    db->gr_count = 1;
}

static void ug_db_parse_group(struct UserGroupBase *base,
                              struct UgDatabase *db, char *text, ULONG len)
{
    char  *cursor;
    char  *line;
    ULONG  commas = 0;
    ULONG  lines  = 1;
    ULONG  slot   = 0;
    ULONG  slots;
    ULONG  i;

    for (i = 0; i < len; i++)
    {
        if (text[i] == ',')
            commas++;
        else if (text[i] == '\n')
            lines++;
    }

    /* Worst case per group: (commas + 1) members plus one NULL terminator. */
    slots = commas + 2 * lines + 2;

    db->gr_members = ami_alloc(slots * (ULONG)sizeof(char *));
    if (db->gr_members == NULL)
    {
        AMI_WARN("usergroup: out of memory parsing group file");
        ug_db_default_group(db);
        return;
    }

    (void)base;

    cursor = text;
    while (db->gr_count < UG_MAX_GROUP && (line = ug_next_line(&cursor)) != NULL)
    {
        struct ug_group *gr;
        char *field = line;
        char *name;
        char *members;

        if (*line == '\0' || *line == '#')
            continue;

        name = ug_field(&field, ':');
        if (*name == '\0')
            continue;

        gr = &db->gr[db->gr_count];
        gr->gr_name   = name;
        gr->gr_passwd = ug_field(&field, ':');
        gr->gr_gid    = ug_atol(ug_field(&field, ':'));
        gr->gr_mem    = &db->gr_members[slot];

        members = field;    /* the whole remainder is the comma list */
        while (members != NULL && *members != '\0' && slot + 2 <= slots)
        {
            char *one = ug_next_field(&members, ',');

            if (one == NULL)
                break;
            if (*one != '\0')
                db->gr_members[slot++] = one;
        }

        db->gr_members[slot++] = NULL;
        db->gr_count++;
    }

    if (db->gr_count == 0)
        ug_db_default_group(db);
}

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
            ug_db_parse_group(base, &g->db, g->db.gr_text, len);
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
