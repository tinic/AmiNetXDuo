/*
 * AmiNetXDuo, host-side test for the usergroup id, group-set and session
 * vectors.
 *
 * WHY THESE.  ugl_getgroups() and ugl_setgroups() move a caller-supplied
 * array in and out of a FIXED cr_groups[UG_NGROUPS] inside the library base,
 * and ugl_initgroups() fills the same array from the group file, where the
 * bound is however many groups name the user.  Three chances to write past
 * a 32-entry array on a machine with no MMU, and none of them was tested.
 *
 * cr_groups is a member, not an allocation, so a write past its end lands in
 * cr_session rather than in an ASan redzone.  cr_session is therefore checked
 * as a canary wherever a length is pushed to the limit; the caller-side array
 * is heap-allocated at the exact length so ASan does have a redzone there.
 *
 * ug_db.c and ug_parse.c are linked because initgroups() reads the group
 * file, over the dos.library shim.  See shim/proto/dos.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "usergroup_internal.h"
#include "usergroup_vectors.h"

#include <proto/dos.h>
#include <proto/exec.h>

#include <stdint.h>
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
        memset(p, 0xA5, size);
        stub_outstanding++;
    }

    return p;
}

APTR ami_alloc_flags(ULONG size, ULONG memf) { (void)memf; return ami_alloc(size); }

VOID ami_free(APTR p)
{
    if (p != NULL)
    {
        stub_outstanding--;
        free(p);
    }
}

void ug_set_err(struct UserGroupBase *base, LONG err) { base->ug_Err = err; }

int ug_strcmp(const char *a, const char *b) { return strcmp(a, b); }

ULONG ug_strlen(const char *s) { return (ULONG)strlen(s); }

void ug_strncpy(char *dst, const char *src, ULONG size)
{
    ULONG i;

    if (size == 0)
        return;

    for (i = 0; i + 1 < size && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* ug_context.c's; this test does not link it. */
static int stub_resolve_calls;

void ug_resolve_login(struct UserGroupBase *base)
{
    stub_resolve_calls++;
    if (base->ug_Cred.cr_login[0] == '\0')
        ug_strncpy(base->ug_Cred.cr_login, "root",
                   sizeof(base->ug_Cred.cr_login));
}

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

#define CANARY  0x5A5A5A5AL

static void world_reset(void)
{
    memset(&g, 0, sizeof(g));
    memset(&base, 0, sizeof(base));
    memset(&me, 0, sizeof(me));

    me.pr_Task.tc_Node.ln_Type = NT_PROCESS;
    shim_current_task = &me.pr_Task;

    base.ug_Global = &g;
    stub_dosbase   = (struct DosLibrary *)&g;

    /* root, one group, as ug_context_init() leaves it. */
    base.ug_Cred.cr_ngroups = 1;
    base.ug_Cred.cr_groups[0] = 0;

    /* The word immediately after cr_groups[]. A write past the array lands
       here, where no sanitizer can see it: this is the redzone. */
    base.ug_Cred.cr_session = CANARY;

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

/* getgroups(0, NULL) is the documented "how many are there?" query and must
   not touch the pointer. */
static void test_getgroups_query(void)
{
    world_reset();

    CHECK(ugl_getgroups(&base, 0, NULL) == 1);

    base.ug_Cred.cr_ngroups = 5;
    CHECK(ugl_getgroups(&base, 0, NULL) == 5);
    CHECK(base.ug_Cred.cr_session == CANARY);

    world_free();
}

/* A buffer shorter than the answer is EINVAL, and nothing is written. */
static void test_getgroups_short_buffer(void)
{
    LONG *buf;
    LONG  i;

    world_reset();

    base.ug_Cred.cr_ngroups = 4;
    for (i = 0; i < 4; i++)
        base.ug_Cred.cr_groups[i] = 100 + i;

    /* Exactly three longs: ASan gives it a redzone at buf[3]. */
    buf = malloc(3 * sizeof(LONG));
    CHECK(buf != NULL);

    CHECK(ugl_getgroups(&base, 3, buf) == -1);
    CHECK(base.ug_Err == UG_EINVAL);

    free(buf);

    CHECK(ugl_getgroups(&base, 4, NULL) == -1);
    CHECK(base.ug_Err == UG_EFAULT);

    world_free();
}

/* An exactly sized buffer is filled exactly, and one past the end is not
   written: the allocation is the answer's length, so ASan owns buf[count]. */
static void test_getgroups_exact(void)
{
    LONG *buf;
    LONG  i;

    world_reset();

    base.ug_Cred.cr_ngroups = UG_NGROUPS;
    for (i = 0; i < UG_NGROUPS; i++)
        base.ug_Cred.cr_groups[i] = 1000 + i;

    buf = malloc(UG_NGROUPS * sizeof(LONG));
    CHECK(buf != NULL);

    CHECK(ugl_getgroups(&base, UG_NGROUPS, buf) == UG_NGROUPS);
    CHECK(base.ug_Err == 0);

    for (i = 0; i < UG_NGROUPS; i++)
        CHECK(buf[i] == 1000 + i);

    free(buf);
    world_free();
}

/* setgroups bounds its input at UG_NGROUPS.  One more is EINVAL, and the
   canary proves nothing was written on the way to finding out. */
static void test_setgroups_bounds(void)
{
    LONG over[UG_NGROUPS + 1];
    LONG i;

    world_reset();

    for (i = 0; i < UG_NGROUPS + 1; i++)
        over[i] = 7000 + i;

    CHECK(ugl_setgroups(&base, UG_NGROUPS + 1, over) == -1);
    CHECK(base.ug_Err == UG_EINVAL);
    CHECK(base.ug_Cred.cr_session == CANARY);
    CHECK(base.ug_Cred.cr_ngroups == 1);        /* unchanged */

    CHECK(ugl_setgroups(&base, -1, over) == -1);
    CHECK(base.ug_Err == UG_EINVAL);

    /* Exactly the limit is accepted and fills the array to its last slot. */
    CHECK(ugl_setgroups(&base, UG_NGROUPS, over) == 0);
    CHECK(base.ug_Err == 0);
    CHECK(base.ug_Cred.cr_ngroups == UG_NGROUPS);
    CHECK(base.ug_Cred.cr_groups[UG_NGROUPS - 1] == 7000 + UG_NGROUPS - 1);
    CHECK(base.ug_Cred.cr_session == CANARY);   /* and not one past it */

    world_free();
}

/* A zero length may pass NULL; any other length may not. */
static void test_setgroups_null(void)
{
    world_reset();

    CHECK(ugl_setgroups(&base, 0, NULL) == 0);
    CHECK(base.ug_Cred.cr_ngroups == 0);

    CHECK(ugl_setgroups(&base, 1, NULL) == -1);
    CHECK(base.ug_Err == UG_EFAULT);

    world_free();
}

/* Everything that changes identity is root-only. */
static void test_privilege(void)
{
    LONG one[1];

    world_reset();
    one[0] = 5;

    base.ug_Cred.cr_euid = 100;                 /* no longer root */

    CHECK(ugl_setgroups(&base, 1, one) == -1);
    CHECK(base.ug_Err == UG_EPERM);

    CHECK(ugl_setlogin(&base, (STRPTR)"jane") == -1);
    CHECK(base.ug_Err == UG_EPERM);

    /* setuid is the exception, and deliberately: a caller who dropped its
       effective uid may always go back to its REAL one.  Anything else is
       still EPERM. */
    CHECK(ugl_setuid(&base, 55) == -1);
    CHECK(base.ug_Err == UG_EPERM);
    CHECK(base.ug_Cred.cr_ruid == 0);           /* and nothing moved */
    CHECK(base.ug_Cred.cr_euid == 100);

    CHECK(ugl_setuid(&base, 0) == 0);           /* back to cr_ruid, allowed */
    CHECK(base.ug_Cred.cr_ruid == 0);
    CHECK(base.ug_Cred.cr_euid == 0);           /* both, not just one */

    /* Root again, so the same calls are allowed. */
    CHECK(ugl_setgroups(&base, 1, one) == 0);
    CHECK(ugl_setlogin(&base, (STRPTR)"jane") == 0);
    CHECK_STR((char *)ugl_getlogin(&base), "jane");

    world_free();
}

/*
 * initgroups() fills cr_groups from the group file.  A user in more groups
 * than the array holds must stop at UG_NGROUPS, and basegid is always first.
 */
static void test_initgroups_caps_at_ngroups(void)
{
    static char file[4096];
    char       *p = file;
    int         i;
    LONG        n;

    world_reset();

    /* 40 groups, more than UG_NGROUPS, every one of them containing jane.
       snprintf, not sprintf: the macOS SDK marks sprintf __deprecated_msg and
       the sanitize arm builds with -Werror, so sprintf is a build failure
       there and only there.  Nothing about the test needs the unbounded one. */
    for (i = 0; i < 40; i++)
    {
        size_t left = sizeof(file) - (size_t)(p - file);
        int    n    = snprintf(p, left, "g%d:*:%d:jane\n", i, 500 + i);

        if (n <= 0 || (size_t)n >= left)
            break;
        p += n;
    }

    shim_dos_add_file("DEVS:Internet/group", file, (long)(p - file));

    CHECK(ugl_initgroups(&base, (STRPTR)"jane", 42) == 0);

    n = base.ug_Cred.cr_ngroups;
    CHECK(n == UG_NGROUPS);                     /* capped, not 41 */
    CHECK(base.ug_Cred.cr_groups[0] == 42);     /* basegid first */
    CHECK(base.ug_Cred.cr_session == CANARY);   /* nothing past the array */

    world_free();
}

/* A gid already present is not added twice, so a user whose basegid is also
   one of their groups does not lose a slot to it. */
static void test_initgroups_dedupes(void)
{
    static const char file[] =
        "wheel:*:10:jane\n"
        "staff:*:20:jane\n";

    world_reset();
    shim_dos_add_file("DEVS:Internet/group", file, (long)sizeof(file) - 1);

    CHECK(ugl_initgroups(&base, (STRPTR)"jane", 10) == 0);

    CHECK(base.ug_Cred.cr_ngroups == 2);        /* 10 once, then 20 */
    CHECK(base.ug_Cred.cr_groups[0] == 10);
    CHECK(base.ug_Cred.cr_groups[1] == 20);
    CHECK(base.ug_Cred.cr_session == CANARY);

    world_free();
}

/* A user in no group at all still gets their basegid. */
static void test_initgroups_no_membership(void)
{
    static const char file[] = "wheel:*:10:root\n";

    world_reset();
    shim_dos_add_file("DEVS:Internet/group", file, (long)sizeof(file) - 1);

    CHECK(ugl_initgroups(&base, (STRPTR)"nobody", 77) == 0);
    CHECK(base.ug_Cred.cr_ngroups == 1);
    CHECK(base.ug_Cred.cr_groups[0] == 77);

    CHECK(ugl_initgroups(&base, NULL, 1) == -1);
    CHECK(base.ug_Err == UG_EFAULT);

    world_free();
}

/* setreuid/setregid take -1 as "leave this one alone", which is the whole
   reason the pair exists. */
static void test_setreuid_minus_one(void)
{
    world_reset();

    CHECK(ugl_setreuid(&base, 100, -1) == 0);
    CHECK(base.ug_Cred.cr_ruid == 100);
    CHECK(base.ug_Cred.cr_euid == 0);           /* untouched */

    /* There is no cr_egid: AmiTCP's effective gid IS cr_groups[0], which is
       why setregid() writes there and getegid() reads it back. */
    CHECK(ugl_setregid(&base, -1, 200) == 0);
    CHECK(base.ug_Cred.cr_rgid == 0);           /* untouched */
    CHECK(base.ug_Cred.cr_groups[0] == 200);
    CHECK(ugl_getegid(&base) == 200);
    CHECK(ugl_getgid(&base) == 0);

    world_free();
}

/* umask returns the previous value, which is what a caller restores from. */
static void test_umask_returns_previous(void)
{
    world_reset();

    CHECK(ugl_umask(&base, 022) == 0);
    CHECK(ugl_getumask(&base) == 022);
    CHECK(ugl_umask(&base, 077) == 022);
    CHECK(ugl_getumask(&base) == 077);

    world_free();
}

/* The session id is the caller's Task pointer, and getpgrp() fills it in
   lazily rather than answering zero. */
static void test_session(void)
{
    world_reset();
    base.ug_Cred.cr_session = 0;

    CHECK(ugl_getpgrp(&base) == (LONG)(uintptr_t)&me.pr_Task);
    CHECK(ugl_setsid(&base) == (LONG)(uintptr_t)&me.pr_Task);

    world_free();
}

int main(void)
{
    test_getgroups_query();
    test_getgroups_short_buffer();
    test_getgroups_exact();
    test_setgroups_bounds();
    test_setgroups_null();
    test_privilege();
    test_initgroups_caps_at_ngroups();
    test_initgroups_dedupes();
    test_initgroups_no_membership();
    test_setreuid_minus_one();
    test_umask_returns_previous();
    test_session();

    printf("\n%d checks, %d failure(s)\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
