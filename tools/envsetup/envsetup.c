/*
 * envsetup -- prepare the AmigaDOS environment for a harness test run.
 *
 * The harness boots from a bare directory hard drive with a minimal
 * Startup-Sequence, so none of the assigns a normal Workbench boot makes exist.
 * Anything calling GetVar()/SetVar() (ENV:), or writing scratch files (T:),
 * fails without them.
 *
 * Doing this in C rather than with C:Assign and C:MakeDir keeps the harness
 * self-contained -- no Workbench binaries have to be extracted or staged.
 *
 * ENV: and T: are backed by directories on the *host*, not RAM:, so env vars a
 * test sets are visible from the host after the run, and can be pre-seeded by
 * staging an env/ directory.
 *
 * SPDX-License-Identifier: MIT
 */

#include <proto/dos.h>
#include <proto/exec.h>

struct Assignment
{
    const char *name;       /* assign name, without the colon */
    const char *path;       /* directory to create and assign to */
};

static const struct Assignment assignments[] =
{
    { "ENV",    "DH0:env"       },
    { "ENVARC", "DH0:envarc"    },
    { "T",      "DH0:t"         },
    { "CLIPS",  "DH0:clips"     },
    { NULL,     NULL            }
};

int main(void)
{
    const struct Assignment *a;
    LONG failures = 0;

    for (a = assignments; a->name != NULL; a++)
    {
        BPTR lock;

        /* Create it if it isn't staged already; an existing directory is fine. */
        lock = CreateDir((STRPTR)a->path);
        if (lock != 0)
            UnLock(lock);

        lock = Lock((STRPTR)a->path, SHARED_LOCK);
        if (lock == 0)
        {
            Printf("envsetup: cannot lock %s\n", (LONG)a->path);
            failures++;
            continue;
        }

        /* AssignLock() takes ownership of the lock -- do not UnLock it. */
        if (!AssignLock((STRPTR)a->name, lock))
        {
            Printf("envsetup: cannot assign %s: %s\n", (LONG)a->name, (LONG)a->path);
            UnLock(lock);
            failures++;
        }
    }

    return failures == 0 ? RETURN_OK : RETURN_ERROR;
}
