/*
 * AmiNetXDuo, usergroup.library: parsing the passwd and group files.
 *
 * Split out of ug_db.c, which keeps the file read and the library vectors.
 * Nothing here calls dos.library or exec.library, so the source that runs on
 * the Amiga is the source that tests/fuzz drives under ASan. src/config has
 * the same arrangement, for the same reason. A sizing pass that disagrees with
 * the parse pass is a heap overrun on a machine with no MMU.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ug_parse.h"

#include "aminetxduo/compat.h"

/* ------------------------------------------------------------ defaults --- */

static char ug_def_name[]  = "root";
static char ug_def_empty[] = "";
static char ug_def_gecos[] = "AmigaOS user";
static char ug_def_dir[]   = "SYS:";
static char ug_def_shell[] = "C:Shell";

static char *ug_def_members[] = { ug_def_name, NULL };

/* -------------------------------------------------------------- parsing, */

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

/*
 * Accumulates unsigned and saturates. A multiply of a signed LONG is undefined
 * past 2^31, and a uid field in DEVS: can hold any number of digits. UBSan
 * found it through tests/fuzz/fuzz_usergroup.c.
 */
static LONG ug_atol(const char *s)
{
    ULONG value = 0;
    BOOL  negative = FALSE;

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
    {
        ULONG digit = (ULONG)(*s++ - '0');

        if (value > (2147483647UL - digit) / 10UL)
        {
            value = 2147483647UL;
            while (*s >= '0' && *s <= '9')
                s++;
            break;
        }

        value = value * 10UL + digit;
    }

    return negative ? -(LONG)value : (LONG)value;
}

/* --------------------------------------------------------- passwd table, */

void ug_db_default_passwd(struct UgDatabase *db)
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

void ug_db_parse_passwd(struct UgDatabase *db, char *text)
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

/* ---------------------------------------------------------- group table, */

void ug_db_default_group(struct UgDatabase *db)
{
    db->gr[0].gr_name   = ug_def_name;
    db->gr[0].gr_passwd = ug_def_empty;
    db->gr[0].gr_gid    = 0;
    db->gr[0].gr_mem    = ug_def_members;
    db->gr_count = 1;
}

void ug_db_parse_group(struct UgDatabase *db, char *text, ULONG len)
{
    char  *cursor;
    char  *line;
    ULONG  commas = 0;
    ULONG  lines  = 1;
    ULONG  slot   = 0;
    ULONG  slots;
    ULONG  i;

    /*
     * ug_next_line() ends a line on '\n' or '\r', so both count. A file with
     * no '\n' in it used to size the arena for one line and parse dozens. A
     * CRLF counted twice only over-allocates.
     */
    for (i = 0; i < len; i++)
    {
        if (text[i] == ',')
            commas++;
        else if (text[i] == '\n' || text[i] == '\r')
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

        /*
         * Checked independently of the sizing above, so a future change there
         * cannot reach past the arena. gr_mem is walked to its NULL, so the
         * group is counted only after a NULL is written.
         */
        if (slot >= slots)
        {
            AMI_WARN("usergroup: group file needs more than %ld member slots,"
                     " %ld groups kept", (long)slots, (long)db->gr_count);
            break;
        }

        db->gr_members[slot++] = NULL;
        db->gr_count++;
    }

    if (db->gr_count == 0)
        ug_db_default_group(db);
}
