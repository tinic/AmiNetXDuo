/*
 * AmiNetXDuo, usergroup.library: per-opener context, errors, credentials.
 *
 * SPDX-License-Identifier: MIT
 */

#include "usergroup_vectors.h"

#include "aminetxduo/compat.h"

#include <proto/exec.h>

#include <stddef.h>

/* ----------------------------------------------------------------- errno, */

/*
 * Mirror the error into the caller errno at the width it asked for through
 * UGT_ERRNOPTR, the same contract that SetErrnoPtr() of bsdsocket.library has.
 */
void ug_set_err(struct UserGroupBase *base, LONG err)
{
    base->ug_Err = err;

    if (base->ug_ErrnoPtr == NULL)
        return;

    switch (base->ug_ErrnoSize)
    {
        case 1:  *(BYTE *)base->ug_ErrnoPtr = (BYTE)err; break;
        case 2:  *(WORD *)base->ug_ErrnoPtr = (WORD)err; break;
        default: *(LONG *)base->ug_ErrnoPtr = err;       break;
    }
}

/* --------------------------------------------------------------- context, */

void ug_context_init(struct UserGroupBase *base)
{
    struct Task *self = FindTask(NULL);

    base->ug_Owner      = self;
    base->ug_ErrnoPtr   = NULL;
    base->ug_ErrnoSize  = 0;
    base->ug_IntrMask   = 0;
    base->ug_Err        = 0;
    base->ug_PwCursor   = 0;
    base->ug_GrCursor   = 0;

    /*
     * AmigaOS has no user database and no protection domain, so every opener
     * starts as the one and only user: root. ug_resolve_login() resolves the
     * name lazily from the passwd file, when there is one.
     */
    base->ug_Cred.cr_ruid     = 0;
    base->ug_Cred.cr_rgid     = 0;
    base->ug_Cred.cr_euid     = 0;
    base->ug_Cred.cr_umask    = 0;
    base->ug_Cred.cr_ngroups  = 1;
    base->ug_Cred.cr_groups[0] = 0;
    base->ug_Cred.cr_session  = (LONG)self;
    base->ug_Cred.cr_login[0] = '\0';

    ug_strncpy(base->ug_ProgName, "", sizeof(base->ug_ProgName));
}

/*
 * Fill in cr_login on first use: the passwd file gets a say in the name of the
 * single user, but only when something asks. An explicit setlogin() wins,
 * because it already made the field non-empty.
 */
void ug_resolve_login(struct UserGroupBase *base)
{
    struct UgDatabase *db;
    UWORD i;

    if (base->ug_Cred.cr_login[0] != '\0')
        return;

    ug_db_require_passwd(base);
    db = &base->ug_Global->db;

    for (i = 0; i < db->pw_count; i++)
    {
        if (db->pw[i].pw_uid == base->ug_Cred.cr_ruid)
        {
            ug_strncpy(base->ug_Cred.cr_login, db->pw[i].pw_name,
                       sizeof(base->ug_Cred.cr_login));
            return;
        }
    }

    ug_strncpy(base->ug_Cred.cr_login, "root", sizeof(base->ug_Cred.cr_login));
}

/* ------------------------------------------------------------- vectors --- */

LONG ugl_SetupContextTagList(UG_A6, register STRPTR name __asm("a0"),
                             register struct TagItem *tags __asm("a1"))
{
    struct TagItem *ti = tags;

    if (name != NULL)
        ug_strncpy(base->ug_ProgName, (const char *)name,
                   sizeof(base->ug_ProgName));

    /* NextTagItem() is utility.library, so the list is walked here. */
    while (ti != NULL)
    {
        ULONG tag  = ti->ti_Tag;
        ULONG data = ti->ti_Data;

        if (tag == TAG_DONE)
            break;

        if (tag == TAG_MORE)
        {
            ti = (struct TagItem *)data;
            continue;
        }

        if (tag == TAG_SKIP)
        {
            ti += data + 1;
            continue;
        }

        switch (tag)
        {
            case UGT_ERRNOBPTR:
                base->ug_ErrnoPtr  = (APTR)data;
                base->ug_ErrnoSize = 1;
                break;

            case UGT_ERRNOWPTR:
                base->ug_ErrnoPtr  = (APTR)data;
                base->ug_ErrnoSize = 2;
                break;

            case UGT_ERRNOLPTR:
                base->ug_ErrnoPtr  = (APTR)data;
                base->ug_ErrnoSize = 4;
                break;

            case UGT_OWNER:
                base->ug_Owner = (struct Task *)data;
                base->ug_Cred.cr_session = (LONG)data;
                break;

            case UGT_INTRMASK:
                base->ug_IntrMask = data;
                break;

            default:
                break;      /* TAG_IGNORE and any unknown tag */
        }

        ti++;
    }

    base->ug_Err = 0;

    return 0;
}

LONG ugl_GetErr(UG_A6)
{
    return base->ug_Err;
}

STRPTR ugl_StrError(UG_A6, register LONG err __asm("d1"))
{
    (void)base;

    switch (err)
    {
        case 0:          return (STRPTR)"No error";
        case UG_EPERM:   return (STRPTR)"Operation not permitted";
        case UG_ENOENT:  return (STRPTR)"No such file or directory";
        case UG_ESRCH:   return (STRPTR)"No such process";
        case UG_ENOMEM:  return (STRPTR)"Cannot allocate memory";
        case UG_EACCES:  return (STRPTR)"Permission denied";
        case UG_EFAULT:  return (STRPTR)"Bad address";
        case UG_EINVAL:  return (STRPTR)"Invalid argument";
        case UG_ERANGE:  return (STRPTR)"Result too large";
        case UG_ENOSYS:  return (STRPTR)"Function not implemented";
        default:         return (STRPTR)"Unknown error";
    }
}

/*
 * getcredentials(NULL) and getcredentials(self) are the common cases. For any
 * other task this searches the open contexts.  That context can close as soon
 * as Forbid() is released, so a pointer into it would already be stale before
 * the caller could dereference it.  Cross-task results are copied into the
 * querying opener while the child list is frozen.
 */
struct ug_credentials *ugl_getcredentials(UG_A6,
                                          register struct Task *task __asm("a0"))
{
    struct UgGlobal *g = base->ug_Global;
    struct ug_credentials *result = NULL;
    struct MinNode *node;

    if (task == NULL || task == base->ug_Owner)
    {
        ug_resolve_login(base);
        return &base->ug_Cred;
    }

    /* Forbid(), not g->lock: the writers are the library Open/Close vectors,
       which exec already calls forbidden, and this walk neither blocks nor
       allocates. */
    Forbid();
    for (node = g->children.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
    {
        struct UserGroupBase *other = (struct UserGroupBase *)
            ((UBYTE *)node - offsetof(struct UserGroupBase, ug_Node));

        if (other->ug_Owner == task)
        {
            /*
             * Copied as it is: resolving cr_login here would still write into
             * another task's context without its knowledge, and the caller
             * only asked to read it.
             */
            base->ug_CredResult = other->ug_Cred;
            result = &base->ug_CredResult;
            break;
        }
    }
    Permit();

    if (result == NULL)
        ug_set_err(base, UG_ESRCH);

    return result;
}
