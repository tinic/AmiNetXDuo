/*
 * AmiNetXDuo, host-side test for the usergroup passwd and group DATABASE.
 *
 * WHAT WAS UNCOVERED.  ug_parse.c has had a host test since it was split out,
 * but ug_db.c -- the dos.library half that decides WHICH file is read, what
 * happens when there is none, and what getpwnam()/getpwuid() hand back -- had
 * no test anywhere in the tree.  Those are the lookups whose NULL return a
 * caller dereferences as pw_name, at offset 0, which is a longword read from
 * $0 on a machine with no MMU.  Returning NULL for an unknown name is correct
 * and is what POSIX asks for; the point of pinning it is that the CONTRACT is
 * now written down in a test rather than inferred, and that the no-file case
 * -- the one every stock install is in, because we ship no passwd file -- is
 * asserted to produce a usable single-user database instead of nothing.
 *
 * src/config/test/shim/proto/dos.h supplies the four dos.library calls over
 * an in-memory file table.  See the note there about Seek() returning the
 * position before the seek: ug_db.c's file sizing depends on it.
 *
 *   cc -std=c99 -Wall -Wextra -I../../../include -I.. \
 *      -I../../config/test/shim test_ug_db.c ../ug_db.c ../ug_parse.c \
 *      -o test_ug_db
 *
 * SPDX-License-Identifier: MIT
 */

#include "usergroup_internal.h"
#include "usergroup_vectors.h"

#include <proto/dos.h>
#include <proto/exec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------- the shim */

struct Task *shim_current_task;
int          shim_forbid_depth;
int          shim_semaphore_depth;

SHIM_DOS_DEFINE_STATE;

/* ------------------------------------------------------------------ stubs */

static ULONG stub_outstanding;

APTR ami_alloc(ULONG size)
{
    void *p;

    if (size == 0)
        return NULL;

    p = malloc(size);
    if (p != NULL)
    {
        memset(p, 0xA5, size);      /* never handed back zeroed by accident */
        stub_outstanding++;
    }

    return p;
}

APTR ami_alloc_flags(ULONG size, ULONG memf)
{
    (void)memf;
    return ami_alloc(size);
}

VOID ami_free(APTR p)
{
    if (p != NULL)
    {
        stub_outstanding--;
        free(p);
    }
}

/* ug_context.c's, but this test does not link it: the subject is the
   database, and ug_Err is the only thing the lookups set. */
void ug_set_err(struct UserGroupBase *base, LONG err)
{
    base->ug_Err = err;
}

int ug_strcmp(const char *a, const char *b)
{
    return strcmp(a, b);
}

/* ug_library.c's.  A non-NULL DosLibrary is all ug_db.c wants; the shim's
   Open() does not look at it. */
static struct DosLibrary *stub_dosbase;

struct DosLibrary *ug_dos(struct UserGroupBase *base)
{
    (void)base;
    return stub_dosbase;
}

/* ---------------------------------------------------------------- harness */

static int checks;
static int failures;

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

static struct UgGlobal      g;
static struct UserGroupBase base;
static struct Process       me;

static void world_reset(void)
{
    memset(&g, 0, sizeof(g));
    memset(&base, 0, sizeof(base));
    memset(&me, 0, sizeof(me));

    me.pr_Task.tc_Node.ln_Type = NT_PROCESS;
    shim_current_task = &me.pr_Task;

    base.ug_Global = &g;
    stub_dosbase   = (struct DosLibrary *)&g;   /* any non-NULL */

    shim_dos_reset();
}

static void world_free(void)
{
    if (g.db.pw_text != NULL)
        ami_free(g.db.pw_text);
    if (g.db.gr_text != NULL)
        ami_free(g.db.gr_text);
    /* The arena behind every gr_mem vector, allocated separately by
       ug_db_parse_group().  LeakSanitizer finds it if this is forgotten. */
    if (g.db.gr_members != NULL)
        ami_free(g.db.gr_members);
}

/* ------------------------------------------------------------------ tests */

/*
 * The case every stock machine is in: we install no passwd file.  The library
 * must still answer, as the single user root, or the whole database is a NULL
 * waiting for a caller to dereference.
 */
static void test_no_file_gives_root(void)
{
    struct ug_passwd *pw;

    world_reset();

    pw = ugl_getpwuid(&base, 0);

    CHECK(pw != NULL);
    if (pw != NULL)
    {
        CHECK_STR(pw->pw_name, "root");
        CHECK(pw->pw_uid == 0);
        CHECK(pw->pw_gid == 0);
        CHECK_STR(pw->pw_dir, "SYS:");
    }
    CHECK(g.db.pw_count == 1);
    CHECK(g.db.pw_text == NULL);            /* nothing was read */
    CHECK(base.ug_Err == 0);

    /* Both candidate paths were tried, and each Open() was made with the
       requester suppressed. */
    CHECK(shim_dos_opens == 2);
    CHECK(shim_dos_open_unwindowed == 2);
    CHECK(shim_dos_closes == 0);            /* neither one opened */
    CHECK(shim_semaphore_depth == 0);

    world_free();
}

/* An unknown uid is ENOENT and NULL.  That is the contract; what must never
   happen is a half-filled struct that looks valid. */
static void test_unknown_uid_and_name(void)
{
    world_reset();

    CHECK(ugl_getpwuid(&base, 1) == NULL);
    CHECK(base.ug_Err == UG_ENOENT);

    CHECK(ugl_getpwnam(&base, (STRPTR)"nobody") == NULL);
    CHECK(base.ug_Err == UG_ENOENT);

    /* A NULL name is EFAULT, and must not reach strcmp. */
    CHECK(ugl_getpwnam(&base, NULL) == NULL);
    CHECK(base.ug_Err == UG_EFAULT);

    world_free();
}

/* DEVS:Internet/passwd is read, parsed and looked up, and it wins over the
   AmiTCP path. */
static void test_devs_file_wins(void)
{
    static const char devs[] =
        "root::0:0:Root:SYS:\n"
        "jane:x:100:200:Jane Doe:Work:sh\n";
    static const char amitcp[] = "root::0:0:WRONG:SYS:\n";
    struct ug_passwd *pw;

    world_reset();
    shim_dos_add_file("DEVS:Internet/passwd", devs, (long)sizeof(devs) - 1);
    shim_dos_add_file("AmiTCP:db/passwd", amitcp, (long)sizeof(amitcp) - 1);

    pw = ugl_getpwnam(&base, (STRPTR)"jane");

    CHECK(pw != NULL);
    if (pw != NULL)
    {
        CHECK(pw->pw_uid == 100);
        CHECK(pw->pw_gid == 200);
        CHECK_STR(pw->pw_gecos, "Jane Doe");
        CHECK_STR(pw->pw_shell, "sh");
    }
    CHECK(g.db.pw_count == 2);
    CHECK(shim_dos_opens == 1);             /* stopped at the first hit */
    CHECK(shim_dos_closes == 1);
    CHECK(base.ug_Err == 0);

    /* The gecos proves which file was used. */
    pw = ugl_getpwuid(&base, 0);
    CHECK(pw != NULL);
    if (pw != NULL)
        CHECK_STR(pw->pw_gecos, "Root");

    world_free();
}

/* With DEVS: absent the AmiTCP path is the fallback, which is the whole
   reason two paths are listed. */
static void test_amitcp_fallback(void)
{
    static const char amitcp[] = "root::0:0:FromAmiTCP:SYS:\n";
    struct ug_passwd *pw;

    world_reset();
    shim_dos_add_file("AmiTCP:db/passwd", amitcp, (long)sizeof(amitcp) - 1);

    pw = ugl_getpwuid(&base, 0);

    CHECK(pw != NULL);
    if (pw != NULL)
        CHECK_STR(pw->pw_gecos, "FromAmiTCP");
    CHECK(shim_dos_opens == 2);             /* DEVS: missed, AmiTCP: hit */
    CHECK(shim_dos_closes == 1);

    world_free();
}

/*
 * The file is read once.  pw_loaded is the flag that makes it so, and a
 * second lookup that re-read DEVS: on every getpwnam() would be a dos.library
 * call per name on a machine where that is expensive.
 */
static void test_read_once(void)
{
    static const char devs[] = "root::0:0:Root:SYS:\n";

    world_reset();
    shim_dos_add_file("DEVS:Internet/passwd", devs, (long)sizeof(devs) - 1);

    CHECK(ugl_getpwuid(&base, 0) != NULL);
    CHECK(shim_dos_opens == 1);

    CHECK(ugl_getpwnam(&base, (STRPTR)"root") != NULL);
    CHECK(ugl_getpwuid(&base, 0) != NULL);
    CHECK(shim_dos_opens == 1);             /* still one */
    CHECK(shim_dos_closes == 1);

    world_free();
}

/* The result is the opener's own copy, so a second lookup overwrites it and
   two openers never share one buffer. */
static void test_result_is_the_openers_copy(void)
{
    static const char devs[] =
        "root::0:0:Root:SYS:\n"
        "jane::100:200:Jane:Work:sh\n";
    struct ug_passwd *a, *b;

    world_reset();
    shim_dos_add_file("DEVS:Internet/passwd", devs, (long)sizeof(devs) - 1);

    a = ugl_getpwuid(&base, 0);
    CHECK(a == &base.ug_PwResult);

    b = ugl_getpwnam(&base, (STRPTR)"jane");
    CHECK(b == a);                          /* one slot, deliberately */
    if (b != NULL)
        CHECK(b->pw_uid == 100);

    world_free();
}

/* A file over UG_MAX_FILE is ignored rather than truncated, and the default
   database stands in for it. */
static void test_oversize_file_ignored(void)
{
    static char big[UG_MAX_FILE + 64];
    struct ug_passwd *pw;

    memset(big, 'x', sizeof(big));
    big[sizeof(big) - 1] = '\n';

    world_reset();
    shim_dos_add_file("DEVS:Internet/passwd", big, (long)sizeof(big));

    pw = ugl_getpwuid(&base, 0);

    CHECK(pw != NULL);                      /* fell back to the default */
    if (pw != NULL)
        CHECK_STR(pw->pw_name, "root");
    CHECK(g.db.pw_count == 1);
    CHECK(g.db.pw_text == NULL);
    CHECK(shim_dos_opens == 2);             /* DEVS: refused, AmiTCP: tried */
    CHECK(shim_dos_closes == 1);            /* only DEVS: ever opened */

    world_free();
}

/* A bare Task is not a Process and has no pr_ fields, so Open() must not be
   reached at all.  Reading pr_WindowPtr off a Task would be a read past the
   end of the structure. */
static void test_bare_task_never_opens(void)
{
    static const char devs[] = "root::0:0:Root:SYS:\n";
    struct Task        bare;
    struct ug_passwd  *pw;

    world_reset();
    shim_dos_add_file("DEVS:Internet/passwd", devs, (long)sizeof(devs) - 1);

    memset(&bare, 0, sizeof(bare));
    bare.tc_Node.ln_Type = NT_TASK;
    shim_current_task = &bare;

    pw = ugl_getpwuid(&base, 0);

    CHECK(shim_dos_opens == 0);             /* never called */
    CHECK(pw != NULL);                      /* and still answers */
    if (pw != NULL)
        CHECK_STR(pw->pw_name, "root");

    world_free();
}

/* The group side takes the same two paths and the same default. */
static void test_group_lookups(void)
{
    static const char devs[] = "wheel:*:0:root\nstaff:*:10:jane,root\n";
    struct ug_group *gr;

    world_reset();
    shim_dos_add_file("DEVS:Internet/group", devs, (long)sizeof(devs) - 1);

    gr = ugl_getgrgid(&base, 10);
    CHECK(gr != NULL);
    if (gr != NULL)
        CHECK_STR(gr->gr_name, "staff");

    gr = ugl_getgrnam(&base, (STRPTR)"wheel");
    CHECK(gr != NULL);
    if (gr != NULL)
        CHECK(gr->gr_gid == 0);

    CHECK(ugl_getgrnam(&base, (STRPTR)"absent") == NULL);
    CHECK(base.ug_Err == UG_ENOENT);
    CHECK(ugl_getgrnam(&base, NULL) == NULL);
    CHECK(base.ug_Err == UG_EFAULT);

    world_free();
}

int main(void)
{
    test_no_file_gives_root();
    test_unknown_uid_and_name();
    test_devs_file_wins();
    test_amitcp_fallback();
    test_read_once();
    test_result_is_the_openers_copy();
    test_oversize_file_ignored();
    test_bare_task_never_opens();
    test_group_lookups();

    printf("\n%d checks, %d failure(s)\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
