/*
 * AmiNetXDuo, host-side test for usergroup.library credentials.
 *
 * WHY THIS FILE EXISTS.  ugl_getcredentials() returned NULL (UG_ESRCH) for any
 * task that had not opened usergroup.library.  ch_nfsmount asks for the
 * credentials of the *foreign* task behind a DOS packet, and such a task never
 * opens this library, so it got NULL and read pw_name -- offset 0 -- from it:
 * a LONG-read from $0, the v0.26.3 enforcer hit and crash.  The library had no
 * coverage of this vector at all, because usergroup_vectors.h had no host
 * branch for its register declarations.  It has one now (UG_REG), so the
 * behaviour is asserted on every host build instead of on a user's machine.
 *
 * ug_context.c reaches exec three times -- FindTask, Forbid, Permit -- which
 * src/config/test/shim/proto/exec.h supplies.  ug_db.c is not linked; the one
 * call into it is stubbed, because the subject here is the credentials walk.
 *
 *   cc -std=c99 -Wall -Wextra -I../../../include -I.. \
 *      -I../../config/test/shim test_ug_credentials.c ../ug_context.c \
 *      -o test_ug_credentials
 *
 * SPDX-License-Identifier: MIT
 */

#include "usergroup_internal.h"
#include "usergroup_vectors.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#define UG_HAVE_MPROTECT 1
#include <sys/mman.h>
#include <unistd.h>
#endif

/* --------------------------------------------------------------- the shim */

struct Task *shim_current_task;
int          shim_forbid_depth;

/* ------------------------------------------------------------------ stubs */

/*
 * The real one opens dos.library and reads DEVS:Internet/passwd.  Here the
 * database is always absent, which is the case that matters: cr_login stays
 * empty and ug_resolve_login() must still leave a usable credentials block.
 */
static int stub_require_passwd_calls;

void ug_db_require_passwd(struct UserGroupBase *base)
{
    (void)base;
    stub_require_passwd_calls++;
}

void ug_strncpy(char *dst, const char *src, ULONG size)
{
    ULONG i;

    if (size == 0)
        return;

    for (i = 0; i + 1 < size && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
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

/* The library stores a task pointer in a LONG.  Truncating the same way the
   library does keeps the comparison exact on a 64-bit host without repeating
   the warning at every call site. */
static LONG as_session(const void *p)
{
    return (LONG)(uintptr_t)p;
}

/* A base wired up the way ug_lib_open() wires one, minus everything the
   credentials walk does not touch. */
static void base_init(struct UserGroupBase *base, struct UgGlobal *g,
                      struct Task *owner)
{
    memset(base, 0, sizeof(*base));
    base->ug_Global = g;
    shim_current_task = owner;
    ug_context_init(base);
}

static void global_init(struct UgGlobal *g)
{
    memset(g, 0, sizeof(*g));
    g->children.mlh_Head     = (struct MinNode *)&g->children.mlh_Tail;
    g->children.mlh_Tail     = NULL;
    g->children.mlh_TailPred = (struct MinNode *)&g->children.mlh_Head;
}

static void global_add(struct UgGlobal *g, struct UserGroupBase *base)
{
    struct MinNode *node = &base->ug_Node;
    struct MinNode *head = g->children.mlh_Head;

    node->mln_Succ = head;
    node->mln_Pred = (struct MinNode *)&g->children.mlh_Head;
    head->mln_Pred = node;
    g->children.mlh_Head = node;
}

/* ------------------------------------------------------------------ tests */

/* getcredentials(NULL) and getcredentials(self) are the same answer, and it is
   the opener's own block -- not a copy. */
static void test_self_and_null(void)
{
    struct UgGlobal      g;
    struct UserGroupBase base;
    struct Task          me;
    struct ug_credentials *a, *b;

    global_init(&g);
    memset(&me, 0, sizeof(me));
    base_init(&base, &g, &me);
    global_add(&g, &base);

    a = ugl_getcredentials(&base, NULL);
    b = ugl_getcredentials(&base, &me);

    CHECK(a != NULL);
    CHECK(b != NULL);
    CHECK(a == &base.ug_Cred);
    CHECK(b == &base.ug_Cred);
    CHECK(a->cr_ruid == 0);
    CHECK(a->cr_ngroups == 1);
    CHECK(shim_forbid_depth == 0);
}

/*
 * THE REGRESSION.  A task that never opened the library is not an error.
 * AmiTCP documents every valid task as a success, and the NFS mount path
 * depends on it.  NULL here is the crash.
 */
static void test_unknown_task_is_not_null(void)
{
    struct UgGlobal      g;
    struct UserGroupBase base;
    struct Task          me, stranger;
    struct ug_credentials *cred;

    global_init(&g);
    memset(&me, 0, sizeof(me));
    memset(&stranger, 0, sizeof(stranger));
    base_init(&base, &g, &me);
    global_add(&g, &base);

    cred = ugl_getcredentials(&base, &stranger);

    CHECK(cred != NULL);                        /* the v0.26.3 defect */
    CHECK(cred == &base.ug_CredResult);         /* a snapshot, not ug_Cred */
    CHECK(cred != &base.ug_Cred);
    CHECK(cred->cr_session == as_session(&me));
    CHECK(cred->cr_umask == 0022);
    CHECK(cred->cr_ruid == base.ug_Cred.cr_ruid);
    CHECK(cred->cr_euid == base.ug_Cred.cr_euid);
    CHECK(base.ug_Err == 0);                    /* not UG_ESRCH any more */
    CHECK(shim_forbid_depth == 0);
}

/*
 * The comment on ugl_getcredentials() says the caller's `task` pointer must
 * never be dereferenced, because a stale one would turn the compatibility
 * fallback back into an enforcer hit.  An unreadable page proves it: any read
 * through `task` is a SIGSEGV, in every build rather than only under ASan.
 *
 * A freed block was tried first and is wrong for this: gcc tracks provenance
 * through a uintptr_t round-trip once -O2 inlines, so -Wuse-after-free fires
 * on the test itself.  mprotect() leaves nothing to track.
 */
static void test_stale_task_pointer_is_not_dereferenced(void)
{
#ifdef UG_HAVE_MPROTECT
    struct UgGlobal      g;
    struct UserGroupBase base;
    struct Task          me;
    struct Task         *unreadable;
    struct ug_credentials *cred;
    void                *page;
    long                 pagesize = sysconf(_SC_PAGESIZE);

    global_init(&g);
    memset(&me, 0, sizeof(me));
    base_init(&base, &g, &me);
    global_add(&g, &base);

    CHECK(pagesize >= (long)sizeof(struct Task));
    page = mmap(NULL, (size_t)pagesize, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(page != MAP_FAILED);
    if (page == MAP_FAILED)
        return;

    memset(page, 0, sizeof(struct Task));
    CHECK(mprotect(page, (size_t)pagesize, PROT_NONE) == 0);

    unreadable = (struct Task *)page;

    cred = ugl_getcredentials(&base, unreadable);

    CHECK(cred != NULL);
    CHECK(cred->cr_session == as_session(&me));
    CHECK(shim_forbid_depth == 0);

    munmap(page, (size_t)pagesize);
#else
    /* A host that cannot make a page unreadable cannot run this assertion.
       Saying so as a FAILURE, not a skip: a gate that cannot see must fail. */
    checks++;
    failures++;
    printf("  FAIL %s:%d: no mprotect() here, the stale-pointer assertion "
           "cannot be proven on this host\n", __FILE__, __LINE__);
#endif
}

/*
 * A second opener's credentials come back COPIED.  Returning &other->ug_Cred
 * would hand out a pointer into a context that can close the moment Permit()
 * runs.
 */
static void test_other_opener_is_copied(void)
{
    struct UgGlobal      g;
    struct UserGroupBase mine, theirs;
    struct Task          me, them;
    struct ug_credentials *cred;

    global_init(&g);
    memset(&me, 0, sizeof(me));
    memset(&them, 0, sizeof(them));

    base_init(&mine, &g, &me);
    global_add(&g, &mine);
    base_init(&theirs, &g, &them);
    global_add(&g, &theirs);

    theirs.ug_Cred.cr_ruid = 42;
    theirs.ug_Cred.cr_rgid = 43;

    cred = ugl_getcredentials(&mine, &them);

    CHECK(cred != NULL);
    CHECK(cred == &mine.ug_CredResult);
    CHECK(cred != &theirs.ug_Cred);             /* copied, not aliased */
    CHECK(cred->cr_ruid == 42);
    CHECK(cred->cr_rgid == 43);
    CHECK(mine.ug_Cred.cr_ruid == 0);           /* mine is untouched */
    CHECK(shim_forbid_depth == 0);

    /* The snapshot survives the other context going away. */
    memset(&theirs, 0xA5, sizeof(theirs));
    CHECK(cred->cr_ruid == 42);
}

/* An empty child list is the state during the very first Open().  The walk
   must terminate on mlh_Tail == NULL, not run off it. */
static void test_empty_child_list(void)
{
    struct UgGlobal      g;
    struct UserGroupBase base;
    struct Task          me, stranger;
    struct ug_credentials *cred;

    global_init(&g);
    memset(&me, 0, sizeof(me));
    memset(&stranger, 0, sizeof(stranger));
    base_init(&base, &g, &me);      /* deliberately NOT added to the list */

    cred = ugl_getcredentials(&base, &stranger);

    CHECK(cred != NULL);
    CHECK(cred->cr_session == as_session(&me));
    CHECK(shim_forbid_depth == 0);
}

int main(void)
{
    test_self_and_null();
    test_unknown_task_is_not_null();
    test_stale_task_pointer_is_not_dereferenced();
    test_other_opener_is_copied();
    test_empty_child_list();

    CHECK(stub_require_passwd_calls > 0);   /* the lazy resolve did run */

    printf("\n%d checks, %d failure(s)\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
