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

/* In-place field or member split. Returns NULL once the line is exhausted. */
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
 * DEVS:Internet files traditionally use Unix ':' records.  AmiTCP 4's
 * shipped db/passwd and db/group examples use '|' instead, because Amiga
 * paths routinely contain ':'.  Select per record so either database can be
 * used, including a hand-migrated file under either assign.
 */
static char ug_record_separator(const char *line)
{
    const char *p;

    for (p = line; *p != '\0'; p++)
    {
        if (*p == '|')
            return '|';
    }

    return ':';
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

/* ReadArgs /N rejects a field which is not entirely one signed decimal
   number.  Keep that property for Roadshow's native files: accepting a typo
   as uid or gid zero would silently grant the wrong identity. */
static BOOL ug_number(const char *s, LONG *out)
{
    const char *p = s;
    BOOL negative = FALSE;
    ULONG value = 0;
    ULONG limit;

    if (p == NULL || out == NULL)
        return FALSE;

    if (*p == '-' || *p == '+')
    {
        negative = (BOOL)(*p == '-');
        p++;
    }
    if (*p < '0' || *p > '9')
        return FALSE;

    limit = negative ? 2147483648UL : 2147483647UL;

    while (*p >= '0' && *p <= '9')
    {
        ULONG digit = (ULONG)(*p++ - '0');

        if (value > (limit - digit) / 10UL)
            return FALSE;
        value = value * 10UL + digit;
    }
    if (*p != '\0')
        return FALSE;

    if (negative && value == 2147483648UL)
        *out = (-2147483647L - 1L);
    else
        *out = negative ? -(LONG)value : (LONG)value;
    return TRUE;
}

static BOOL ug_iequal(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        char ca = *a++;
        char cb = *b++;

        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + ('a' - 'A'));
        if (ca != cb)
            return FALSE;
    }

    return (BOOL)(*a == '\0' && *b == '\0');
}

/* One AmigaDOS argument, compacted in place.  Quotes group whitespace and
   quoted *N, *E, *" and ** use the ReadItem escape rules.  This covers the
   Roadshow files without allocations whose strings would die at FreeArgs(). */
static char *ug_next_arg(char **cursor)
{
    char *read;
    char *write;
    char *start;
    BOOL  quoted = FALSE;

    if (cursor == NULL || *cursor == NULL)
        return NULL;

    read = *cursor;
    while (*read == ' ' || *read == '\t')
        read++;
    if (*read == '\0' || *read == ';')
    {
        *cursor = NULL;
        return NULL;
    }

    start = write = read;
    while (*read != '\0')
    {
        char c = *read++;

        if (!quoted && c == ';')
        {
            while (*read != '\0')
                read++;
            break;
        }

        if (c == '*' && quoted)
        {
            if (*read == '"' || *read == '*')
                *write++ = *read++;
            else if (*read == 'E')
            {
                *write++ = 27;
                read++;
            }
            else if (*read == 'N')
            {
                *write++ = '\n';
                read++;
            }
            else
                *write++ = c;
            continue;
        }
        if (c == '"')
        {
            quoted = (BOOL)!quoted;
            continue;
        }
        if (!quoted && (c == ' ' || c == '\t'))
            break;

        *write++ = c;
    }

    *write = '\0';
    while (*read == ' ' || *read == '\t')
        read++;
    *cursor = (*read != '\0') ? read : NULL;

    return start;
}

static char *ug_arg_value(char *arg, char **key_out)
{
    char *p = arg;

    while (*p != '\0' && *p != '=')
        p++;

    if (*p == '=')
    {
        *p++ = '\0';
        *key_out = arg;
        return p;
    }

    *key_out = NULL;
    return arg;
}

static BOOL ug_users_keyword(const char *arg)
{
    return (BOOL)(ug_iequal(arg, "name") || ug_iequal(arg, "password") ||
                  ug_iequal(arg, "uid") || ug_iequal(arg, "gid") ||
                  ug_iequal(arg, "gecos") || ug_iequal(arg, "dir") ||
                  ug_iequal(arg, "shell"));
}

static BOOL ug_groups_keyword(const char *arg)
{
    return (BOOL)(ug_iequal(arg, "name") || ug_iequal(arg, "id") ||
                  ug_iequal(arg, "users"));
}

/* ReadArgs accepts KEY=value, KEY value and whitespace around '='. */
static char *ug_keyword_value(char **scan)
{
    char *value = ug_next_arg(scan);

    if (value != NULL && value[0] == '=')
    {
        value++;
        if (*value == '\0')
            value = ug_next_arg(scan);
    }

    return value;
}

static BOOL ug_quotes_valid(const char *line)
{
    BOOL quoted = FALSE;

    while (*line != '\0')
    {
        if (quoted && *line == '*' &&
            (line[1] == '"' || line[1] == '*' ||
             line[1] == 'E' || line[1] == 'N'))
        {
            line += 2;
            continue;
        }
        if (*line++ == '"')
            quoted = (BOOL)!quoted;
    }

    return (BOOL)!quoted;
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
        char sep;

        if (*line == '\0' || *line == '#')
            continue;

        sep = ug_record_separator(line);
        name = ug_field(&field, sep);
        if (*name == '\0')
            continue;

        pw = &db->pw[db->pw_count];
        pw->pw_name   = name;
        pw->pw_passwd = ug_field(&field, sep);
        pw->pw_uid    = ug_atol(ug_field(&field, sep));
        pw->pw_gid    = ug_atol(ug_field(&field, sep));
        pw->pw_gecos  = ug_field(&field, sep);
        pw->pw_dir    = ug_field(&field, sep);
        pw->pw_shell  = ug_field(&field, sep);

        if (*pw->pw_dir == '\0')
            pw->pw_dir = ug_def_dir;
        if (*pw->pw_shell == '\0')
            pw->pw_shell = ug_def_shell;

        db->pw_count++;
    }

    if (db->pw_count == 0)
        ug_db_default_passwd(db);
}

/* Roadshow's DEVS:Internet/users, one ReadArgs command line per user:
   NAME/A,PASSWORD/K,UID/A/N,GID/A/N,GECOS,DIR,SHELL. */
void ug_db_parse_users(struct UgDatabase *db, char *text)
{
    char *cursor = text;
    char *line;

    while (db->pw_count < UG_MAX_PASSWD &&
           (line = ug_next_line(&cursor)) != NULL)
    {
        struct ug_passwd *pw;
        char *scan = line;
        char *arg;
        char *name = NULL;
        char *password = ug_def_empty;
        char *uid = NULL;
        char *gid = NULL;
        char *gecos = ug_def_empty;
        char *dir = ug_def_dir;
        char *shell = ug_def_shell;
        UWORD positional = 0;
        BOOL valid = TRUE;

        while (*scan == ' ' || *scan == '\t')
            scan++;
        if (*scan == '\0' || *scan == '#')
            continue;
        if (!ug_quotes_valid(scan))
            continue;

        while ((arg = ug_next_arg(&scan)) != NULL)
        {
            char *key;
            char *value = ug_arg_value(arg, &key);

            if (key == NULL && ug_users_keyword(value))
            {
                key = value;
                value = ug_keyword_value(&scan);
                if (value == NULL)
                {
                    valid = FALSE;
                    break;
                }
            }

            if (key != NULL)
            {
                if (ug_iequal(key, "name"))          name = value;
                else if (ug_iequal(key, "password")) password = value;
                else if (ug_iequal(key, "uid"))      uid = value;
                else if (ug_iequal(key, "gid"))      gid = value;
                else if (ug_iequal(key, "gecos"))    gecos = value;
                else if (ug_iequal(key, "dir"))      dir = value;
                else if (ug_iequal(key, "shell"))    shell = value;
                else                                  valid = FALSE;
                continue;
            }

            /* PASSWORD is /K and therefore not in the positional sequence. */
            switch (positional++)
            {
                case 0: name  = value; break;
                case 1: uid   = value; break;
                case 2: gid   = value; break;
                case 3: gecos = value; break;
                case 4: dir   = value; break;
                case 5: shell = value; break;
                default: valid = FALSE; break;
            }
        }

        if (!valid || name == NULL || *name == '\0' ||
            uid == NULL || gid == NULL)
            continue;

        pw = &db->pw[db->pw_count];
        if (!ug_number(uid, &pw->pw_uid) || !ug_number(gid, &pw->pw_gid))
            continue;

        pw->pw_name   = name;
        pw->pw_passwd = password;
        pw->pw_gecos  = gecos;
        pw->pw_dir    = (*dir != '\0') ? dir : ug_def_dir;
        pw->pw_shell  = (*shell != '\0') ? shell : ug_def_shell;
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
        char sep;

        if (*line == '\0' || *line == '#')
            continue;

        sep = ug_record_separator(line);
        name = ug_field(&field, sep);
        if (*name == '\0')
            continue;

        gr = &db->gr[db->gr_count];
        gr->gr_name   = name;
        gr->gr_passwd = ug_field(&field, sep);
        gr->gr_gid    = ug_atol(ug_field(&field, sep));
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

static VOID ug_group_members_add(struct UgDatabase *db, char *members,
                                 ULONG *slot, ULONG slots)
{
    while (members != NULL && *members != '\0' && *slot + 1 < slots)
    {
        char *one = ug_next_field(&members, ',');

        if (one != NULL && *one != '\0')
            db->gr_members[(*slot)++] = one;
    }
}

/* Roadshow's DEVS:Internet/groups, NAME/A,ID/A/N,USERS/M. */
void ug_db_parse_groups(struct UgDatabase *db, char *text, ULONG len)
{
    char  *cursor;
    char  *line;
    ULONG  tokens = 0;
    ULONG  commas = 0;
    ULONG  lines = 1;
    ULONG  slot = 0;
    ULONG  slots;
    ULONG  i;
    BOOL   in_word = FALSE;

    /* Each word and comma can begin a member; each line needs one NULL.
       Quoted whitespace only overestimates, safely. */
    for (i = 0; i < len; i++)
    {
        BOOL white = (BOOL)(text[i] == ' ' || text[i] == '\t' ||
                            text[i] == '\n' || text[i] == '\r');

        if (!white && !in_word)
            tokens++;
        if (text[i] == ',')
            commas++;
        in_word = (BOOL)!white;
        if (text[i] == '\n' || text[i] == '\r')
            lines++;
    }

    /* One whitespace token can itself be a comma-separated member list. */
    slots = tokens + commas + lines + 2;
    db->gr_members = ami_alloc(slots * (ULONG)sizeof(char *));
    if (db->gr_members == NULL)
    {
        AMI_WARN("usergroup: out of memory parsing groups file");
        ug_db_default_group(db);
        return;
    }

    cursor = text;
    while (db->gr_count < UG_MAX_GROUP &&
           (line = ug_next_line(&cursor)) != NULL)
    {
        struct ug_group *gr;
        char *scan = line;
        char *arg;
        char *name = NULL;
        char *id = NULL;
        BOOL valid = TRUE;

        while (*scan == ' ' || *scan == '\t')
            scan++;
        if (*scan == '\0' || *scan == '#')
            continue;
        if (!ug_quotes_valid(scan))
            continue;

        gr = &db->gr[db->gr_count];
        gr->gr_mem = &db->gr_members[slot];

        while ((arg = ug_next_arg(&scan)) != NULL)
        {
            char *key;
            char *value = ug_arg_value(arg, &key);

            if (key == NULL && ug_groups_keyword(value))
            {
                key = value;
                value = ug_keyword_value(&scan);
                if (value == NULL)
                {
                    valid = FALSE;
                    break;
                }
            }

            if (key != NULL)
            {
                if (ug_iequal(key, "name"))
                    name = value;
                else if (ug_iequal(key, "id"))
                    id = value;
                else if (ug_iequal(key, "users"))
                {
                    ug_group_members_add(db, value, &slot, slots);
                }
                else
                    valid = FALSE;
                continue;
            }

            if (name == NULL)
                name = value;
            else if (id == NULL)
                id = value;
            else
                ug_group_members_add(db, value, &slot, slots);
        }

        if (!valid || name == NULL || *name == '\0' || id == NULL)
        {
            slot = (ULONG)(gr->gr_mem - db->gr_members);
            continue;
        }

        if (!ug_number(id, &gr->gr_gid))
        {
            slot = (ULONG)(gr->gr_mem - db->gr_members);
            continue;
        }

        db->gr_members[slot++] = NULL;
        gr->gr_name   = name;
        gr->gr_passwd = ug_def_empty;
        db->gr_count++;
    }

    if (db->gr_count == 0)
        ug_db_default_group(db);
}
