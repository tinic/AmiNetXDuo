/*
 * AmiNetXDuo -- host-side test for the passwd and group file parsers.
 *
 * ug_parse.c makes no AmigaDOS call, so it needs only the <exec/types.h>
 * shim in src/config/test/shim and the three stubs below. ug_db.c -- the
 * dos.library Open/Read/Close around it -- is not covered here.
 *
 * LINE ENDINGS ARE THE POINT. ug_next_line() ends a line on '\n' or '\r',
 * so a group file written by a classic-Mac editor is many lines to the
 * parser. The arena sizing pass used to count '\n' alone, so it sized for
 * one; the NULL terminator written after each group then walked off the
 * end of the allocation, on a machine with no MMU, from a file in DEVS:.
 * Run under ASan (tools/ci.sh host) the CR-only fixture below is what
 * catches it.
 *
 *   cc -std=c99 -Wall -Wextra -I../../../include -I.. \
 *      -I../../config/test/shim test_usergroup.c ../ug_parse.c -o test_usergroup
 *
 * SPDX-License-Identifier: MIT
 */

#include "ug_parse.h"
#include "aminetxduo/compat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ stubs */

static ULONG stub_outstanding;
static int   stub_verbose;

APTR ami_alloc(ULONG size)
{
    void *p;

    if (size == 0)
        return NULL;

    /*
     * malloc, not calloc: an ASan redzone catches a write past the end either
     * way, but leaving the block uninitialised also lets valgrind and MSan
     * see a gr_mem vector that was never terminated.
     */
    p = malloc(size);
    if (p != NULL)
    {
        memset(p, 0xA5, size);
        stub_outstanding++;
    }

    return p;
}

APTR ami_alloc_flags(ULONG size, ULONG memf)
{
    (void)memf;
    return ami_alloc(size);
}

VOID ami_free(APTR ptr)
{
    if (ptr == NULL)
        return;

    free(ptr);
    stub_outstanding--;
}

ULONG ami_alloc_count(VOID)
{
    return stub_outstanding;
}

VOID ami_log(int level, const char *fmt, ...)
{
    va_list args;

    (void)level;
    if (!stub_verbose)
        return;

    va_start(args, fmt);
    fputs("  [log] ", stdout);
    vprintf(fmt, args);
    fputc('\n', stdout);
    va_end(args);
}

/* --------------------------------------------------------------- harness */

static int failures;
static int checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

#define CHECK_STR(got, want)                                                 \
    do {                                                                     \
        checks++;                                                            \
        if ((got) == NULL || strcmp((got), (want)) != 0) {                   \
            failures++;                                                      \
            printf("  FAIL %s:%d: expected \"%s\", got \"%s\"\n",            \
                   __FILE__, __LINE__, (want), (got) ? (got) : "(null)");    \
        }                                                                    \
    } while (0)

/* Both parsers tokenise in place, so every fixture is copied first. */
static struct UgDatabase *load_group(const char *text)
{
    struct UgDatabase *db = (struct UgDatabase *)calloc(1, sizeof(*db));
    ULONG len = (ULONG)strlen(text);
    char *copy = (char *)malloc(len + 1);

    memcpy(copy, text, len + 1);
    ug_db_parse_group(db, copy, len);

    /* The records point into the copy, so it is freed with the database. */
    db->gr_text = copy;

    return db;
}

static void free_db(struct UgDatabase *db)
{
    if (db->gr_members != NULL)
        ami_free(db->gr_members);
    free(db->gr_text);
    free(db->pw_text);
    free(db);
}

static struct UgDatabase *load_passwd(const char *text)
{
    struct UgDatabase *db = (struct UgDatabase *)calloc(1, sizeof(*db));
    char *copy = (char *)malloc(strlen(text) + 1);

    strcpy(copy, text);
    ug_db_parse_passwd(db, copy);
    db->pw_text = copy;

    return db;
}

/* Count a gr_mem vector by walking it to its terminator, as a caller does. */
static int member_count(char **mem)
{
    int n = 0;

    while (mem[n] != NULL)
        n++;

    return n;
}

/* ----------------------------------------------------------- group files */

static const char group_body[] =
    "wheel:*:0:root,jane\n"
    "users:*:100:jane\n"
    "guests:*:200:\n";

/* The same three lines with each terminator, so a difference is the parser's. */
static void check_three_groups(struct UgDatabase *db, const char *what)
{
    printf("group: %s\n", what);

    CHECK(db->gr_count == 3);
    if (db->gr_count != 3)
        return;

    CHECK_STR(db->gr[0].gr_name, "wheel");
    CHECK(db->gr[0].gr_gid == 0);
    CHECK(member_count(db->gr[0].gr_mem) == 2);
    CHECK_STR(db->gr[0].gr_mem[0], "root");
    CHECK_STR(db->gr[0].gr_mem[1], "jane");

    CHECK_STR(db->gr[1].gr_name, "users");
    CHECK(db->gr[1].gr_gid == 100);
    CHECK(member_count(db->gr[1].gr_mem) == 1);
    CHECK_STR(db->gr[1].gr_mem[0], "jane");

    CHECK_STR(db->gr[2].gr_name, "guests");
    CHECK(db->gr[2].gr_gid == 200);
    CHECK(member_count(db->gr[2].gr_mem) == 0);
}

/* Rewrite every '\n' in a fixture as the given terminator. */
static char *retermed(const char *text, const char *eol)
{
    ULONG n = (ULONG)(strlen(text) * strlen(eol) + 1);
    char *out = (char *)malloc(n);
    char *w = out;

    for (; *text != '\0'; text++)
    {
        if (*text == '\n')
        {
            const char *e;

            for (e = eol; *e != '\0'; e++)
                *w++ = *e;
        }
        else
        {
            *w++ = *text;
        }
    }

    *w = '\0';

    return out;
}

static void test_group_line_endings(void)
{
    static const char *const eols[]  = { "\n", "\r", "\r\n", "\n\r" };
    static const char *const names[] = { "LF", "CR only", "CRLF", "LFCR" };
    unsigned i;

    for (i = 0; i < sizeof(eols) / sizeof(eols[0]); i++)
    {
        char *text = retermed(group_body, eols[i]);
        struct UgDatabase *db = load_group(text);

        check_three_groups(db, names[i]);
        free_db(db);
        free(text);
    }
}

static void test_group_mixed_line_endings(void)
{
    /* One of each, in one file, which is what a hand-edited file looks like. */
    struct UgDatabase *db =
        load_group("wheel:*:0:root,jane\r"
                   "users:*:100:jane\r\n"
                   "guests:*:200:\n");

    check_three_groups(db, "mixed");
    free_db(db);
}

/*
 * THE REPRODUCER. Sixty-four member-less groups, CR-terminated. There is no
 * '\n' anywhere in the file, so the old sizing pass counted one line and
 * allocated four pointers while the parse wrote sixty-four terminators. It is
 * UG_MAX_GROUP lines exactly, so the group cap does not mask it.
 */
static void test_group_cr_only_arena(void)
{
    char text[UG_MAX_GROUP * 16];
    struct UgDatabase *db;
    UWORD i;

    text[0] = '\0';
    for (i = 0; i < UG_MAX_GROUP; i++)
    {
        char line[16];

        snprintf(line, sizeof(line), "g%d:*:%d:\r", (int)i, (int)i);
        strcat(text, line);
    }

    printf("group: %d CR-terminated member-less lines\n", UG_MAX_GROUP);

    db = load_group(text);

    CHECK(db->gr_count == UG_MAX_GROUP);
    for (i = 0; i < db->gr_count; i++)
    {
        CHECK(db->gr[i].gr_gid == (LONG)i);
        CHECK(member_count(db->gr[i].gr_mem) == 0);
    }

    free_db(db);
}

/* Every group carries members as well, so the member writes are pressured too. */
static void test_group_cr_only_members(void)
{
    char text[UG_MAX_GROUP * 32];
    struct UgDatabase *db;
    UWORD i;

    text[0] = '\0';
    for (i = 0; i < UG_MAX_GROUP; i++)
    {
        char line[32];

        snprintf(line, sizeof(line), "g%d:*:%d:root,jane,bob\r", (int)i, (int)i);
        strcat(text, line);
    }

    printf("group: %d CR-terminated lines with three members each\n",
           UG_MAX_GROUP);

    db = load_group(text);

    CHECK(db->gr_count == UG_MAX_GROUP);
    for (i = 0; i < db->gr_count; i++)
    {
        CHECK(member_count(db->gr[i].gr_mem) == 3);
        CHECK_STR(db->gr[i].gr_mem[2], "bob");
    }

    free_db(db);
}

static void test_group_edges(void)
{
    struct UgDatabase *db;

    printf("group: edges\n");

    /* Empty file: the built-in single group, and no arena walked. */
    db = load_group("");
    CHECK(db->gr_count == 1);
    CHECK_STR(db->gr[0].gr_name, "root");
    free_db(db);

    /* Comments, blank lines and a nameless line are all skipped. */
    db = load_group("# a comment\r\r:*:5:x\rwheel:*:0:root\r");
    CHECK(db->gr_count == 1);
    CHECK_STR(db->gr[0].gr_name, "wheel");
    CHECK(member_count(db->gr[0].gr_mem) == 1);
    free_db(db);

    /* No terminator at all on the last line. */
    db = load_group("wheel:*:0:root,jane");
    CHECK(db->gr_count == 1);
    CHECK(member_count(db->gr[0].gr_mem) == 2);
    free_db(db);

    /* A run of commas yields no empty members. */
    db = load_group("wheel:*:0:,,,root,,,\r");
    CHECK(db->gr_count == 1);
    CHECK(member_count(db->gr[0].gr_mem) == 1);
    CHECK_STR(db->gr[0].gr_mem[0], "root");
    free_db(db);

    /* Nothing but terminators. */
    db = load_group("\r\r\r\r\r\r\r\r");
    CHECK(db->gr_count == 1);
    CHECK_STR(db->gr[0].gr_name, "root");
    free_db(db);
}

/* ---------------------------------------------------------- passwd files */

static void test_passwd_line_endings(void)
{
    /*
     * No "SYS:" in a home directory field here on purpose: ':' is the field
     * separator, so an AmigaOS path cannot survive one. Roadshow's file has
     * the same property; an empty field and the fallback is how it is written.
     */
    static const char body[] =
        "root:*:0:0:AmigaOS user::\n"
        "jane:*:1000:100:Jane:Work:Sys/Shell\n";
    static const char *const eols[]  = { "\n", "\r", "\r\n" };
    static const char *const names[] = { "LF", "CR only", "CRLF" };
    unsigned i;

    for (i = 0; i < sizeof(eols) / sizeof(eols[0]); i++)
    {
        char *text = retermed(body, eols[i]);
        struct UgDatabase *db = load_passwd(text);

        printf("passwd: %s\n", names[i]);

        CHECK(db->pw_count == 2);
        if (db->pw_count == 2)
        {
            CHECK_STR(db->pw[0].pw_name, "root");
            CHECK(db->pw[0].pw_uid == 0);
            CHECK_STR(db->pw[0].pw_dir, "SYS:");
            /* An empty shell field falls back rather than staying empty. */
            CHECK_STR(db->pw[0].pw_shell, "C:Shell");

            CHECK_STR(db->pw[1].pw_name, "jane");
            CHECK(db->pw[1].pw_uid == 1000);
            CHECK(db->pw[1].pw_gid == 100);
            CHECK_STR(db->pw[1].pw_gecos, "Jane");
            CHECK_STR(db->pw[1].pw_dir, "Work");
            CHECK_STR(db->pw[1].pw_shell, "Sys/Shell");
        }

        free_db(db);
        free(text);
    }
}

static void test_passwd_edges(void)
{
    struct UgDatabase *db;

    printf("passwd: edges\n");

    db = load_passwd("");
    CHECK(db->pw_count == 1);
    CHECK_STR(db->pw[0].pw_name, "root");
    CHECK_STR(db->pw[0].pw_dir, "SYS:");
    free_db(db);

    db = load_passwd("# comment\r\rroot:*:-1:+3:g:d:s\r");
    CHECK(db->pw_count == 1);
    CHECK(db->pw[0].pw_uid == -1);
    CHECK(db->pw[0].pw_gid == 3);
    free_db(db);

    /* A uid longer than a LONG holds clamps; it used to wrap, which is UB. */
    db = load_passwd("big:*:99999999999999:88888888888888:g:d:s\r"
                     "neg:*:-99999999999999:0:g:d:s\r");
    CHECK(db->pw_count == 2);
    CHECK(db->pw[0].pw_uid == 2147483647L);
    CHECK(db->pw[0].pw_gid == 2147483647L);
    CHECK(db->pw[1].pw_uid == -2147483647L);
    free_db(db);

    /* And the largest value that is not clamped survives exactly. */
    db = load_passwd("edge:*:2147483647:0:g:d:s\r");
    CHECK(db->pw[0].pw_uid == 2147483647L);
    free_db(db);

    /* Short lines: the missing fields come back empty, not off the end. */
    db = load_passwd("root\rjane:*\r");
    CHECK(db->pw_count == 2);
    CHECK_STR(db->pw[1].pw_gecos, "");
    CHECK_STR(db->pw[1].pw_dir, "SYS:");
    free_db(db);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-v") == 0)
        stub_verbose = 1;

    test_group_line_endings();
    test_group_mixed_line_endings();
    test_group_cr_only_arena();
    test_group_cr_only_members();
    test_group_edges();
    test_passwd_line_endings();
    test_passwd_edges();

    CHECK(ami_alloc_count() == 0);

    printf("\n%d checks, %d failure(s)\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
