/*
 * AmiNetXDuo -- usergroup.library: ids, groups, umask, sessions, login name.
 *
 * AmigaOS has no protection domain: there is one user, and it is root. The
 * set*id() calls therefore always succeed for the default context and simply
 * record what they were told, which is what the ported daemons that call them
 * (in the hope of dropping privilege) expect to see afterwards. If a caller
 * has already dropped its effective uid away from 0, we start refusing, so
 * code that tests for EPERM still behaves sanely.
 *
 * SPDX-License-Identifier: MIT
 */

#include "usergroup_vectors.h"

#include <proto/exec.h>

static BOOL ug_privileged(struct UserGroupBase *base)
{
    return (BOOL)(base->ug_Cred.cr_euid == 0);
}

/* ------------------------------------------------------ user identity --- */

LONG ugl_getuid(UG_A6)
{
    return base->ug_Cred.cr_ruid;
}

LONG ugl_geteuid(UG_A6)
{
    return base->ug_Cred.cr_euid;
}

LONG ugl_setreuid(UG_A6, register LONG real __asm("d0"),
                         register LONG effective __asm("d1"))
{
    if (!ug_privileged(base) &&
        ((real >= 0 && real != base->ug_Cred.cr_ruid) ||
         (effective >= 0 && effective != base->ug_Cred.cr_ruid)))
    {
        ug_set_err(base, UG_EPERM);
        return -1;
    }

    if (real >= 0)
        base->ug_Cred.cr_ruid = real;
    if (effective >= 0)
        base->ug_Cred.cr_euid = effective;

    ug_set_err(base, 0);

    return 0;
}

LONG ugl_setuid(UG_A6, register LONG uid __asm("d0"))
{
    if (!ug_privileged(base) && uid != base->ug_Cred.cr_ruid)
    {
        ug_set_err(base, UG_EPERM);
        return -1;
    }

    base->ug_Cred.cr_ruid = uid;
    base->ug_Cred.cr_euid = uid;
    ug_set_err(base, 0);

    return 0;
}

/* ----------------------------------------------------- group identity --- */

LONG ugl_getgid(UG_A6)
{
    return base->ug_Cred.cr_rgid;
}

LONG ugl_getegid(UG_A6)
{
    /*
     * The credentials block has no cr_egid: 4.4BSD keeps the effective gid in
     * cr_groups[0], and so do we.
     */
    return (base->ug_Cred.cr_ngroups > 0) ? base->ug_Cred.cr_groups[0]
                                          : base->ug_Cred.cr_rgid;
}

LONG ugl_setregid(UG_A6, register LONG real __asm("d0"),
                         register LONG effective __asm("d1"))
{
    if (!ug_privileged(base))
    {
        ug_set_err(base, UG_EPERM);
        return -1;
    }

    if (real >= 0)
        base->ug_Cred.cr_rgid = real;
    if (effective >= 0)
    {
        base->ug_Cred.cr_groups[0] = effective;
        if (base->ug_Cred.cr_ngroups < 1)
            base->ug_Cred.cr_ngroups = 1;
    }

    ug_set_err(base, 0);

    return 0;
}

LONG ugl_setgid(UG_A6, register LONG gid __asm("d0"))
{
    if (!ug_privileged(base))
    {
        ug_set_err(base, UG_EPERM);
        return -1;
    }

    base->ug_Cred.cr_rgid      = gid;
    base->ug_Cred.cr_groups[0] = gid;
    if (base->ug_Cred.cr_ngroups < 1)
        base->ug_Cred.cr_ngroups = 1;

    ug_set_err(base, 0);

    return 0;
}

/* ------------------------------------------------------ group vectors --- */

LONG ugl_getgroups(UG_A6, register LONG gidsetlen __asm("d0"),
                          register LONG *gidset __asm("a1"))
{
    LONG count = base->ug_Cred.cr_ngroups;
    LONG i;

    /* A zero length is the documented "how many are there?" query. */
    if (gidsetlen == 0)
        return count;

    if (gidset == NULL)
    {
        ug_set_err(base, UG_EFAULT);
        return -1;
    }

    if (gidsetlen < count)
    {
        ug_set_err(base, UG_EINVAL);
        return -1;
    }

    for (i = 0; i < count; i++)
        gidset[i] = base->ug_Cred.cr_groups[i];

    ug_set_err(base, 0);

    return count;
}

LONG ugl_setgroups(UG_A6, register LONG gidsetlen __asm("d0"),
                          register LONG *gidset __asm("a1"))
{
    LONG i;

    if (!ug_privileged(base))
    {
        ug_set_err(base, UG_EPERM);
        return -1;
    }

    if (gidsetlen < 0 || gidsetlen > UG_NGROUPS)
    {
        ug_set_err(base, UG_EINVAL);
        return -1;
    }

    if (gidsetlen > 0 && gidset == NULL)
    {
        ug_set_err(base, UG_EFAULT);
        return -1;
    }

    for (i = 0; i < gidsetlen; i++)
        base->ug_Cred.cr_groups[i] = gidset[i];

    base->ug_Cred.cr_ngroups = (WORD)gidsetlen;
    ug_set_err(base, 0);

    return 0;
}

/*
 * initgroups() rebuilds the supplementary list from the group database:
 * basegid first, then every group that lists `name` as a member.
 */
LONG ugl_initgroups(UG_A6, register STRPTR name __asm("a1"),
                           register LONG basegid __asm("d0"))
{
    struct UgDatabase *db;
    WORD count = 0;
    UWORD i;

    if (name == NULL)
    {
        ug_set_err(base, UG_EFAULT);
        return -1;
    }

    ug_db_require_group(base);
    db = &base->ug_Global->db;

    base->ug_Cred.cr_groups[count++] = basegid;

    for (i = 0; i < db->gr_count && count < UG_NGROUPS; i++)
    {
        char **member = db->gr[i].gr_mem;
        BOOL  member_of = FALSE;
        BOOL  duplicate = FALSE;
        WORD  j;

        while (member != NULL && *member != NULL && !member_of)
        {
            if (ug_strcmp(*member, (const char *)name) == 0)
                member_of = TRUE;
            member++;
        }

        if (!member_of)
            continue;

        for (j = 0; j < count; j++)
        {
            if (base->ug_Cred.cr_groups[j] == db->gr[i].gr_gid)
                duplicate = TRUE;
        }

        if (!duplicate)
            base->ug_Cred.cr_groups[count++] = db->gr[i].gr_gid;
    }

    base->ug_Cred.cr_ngroups = count;
    ug_set_err(base, 0);

    return 0;
}

/* ------------------------------------------------------------- umask ---- */

/*
 * The .sfd declares "ULONG umask(UWORD mask)", so only the low word of d0 is
 * meaningful -- the caller may well leave garbage in the top half.
 */
ULONG ugl_umask(UG_A6, register ULONG mask __asm("d0"))
{
    ULONG previous = base->ug_Cred.cr_umask;

    base->ug_Cred.cr_umask = (UWORD)(mask & 0xFFFF);

    return previous;
}

ULONG ugl_getumask(UG_A6)
{
    return base->ug_Cred.cr_umask;
}

/* ----------------------------------------------------------- sessions --- */

/*
 * AmiTCP defines pid_t as "struct Task *", so the session id is the Task
 * pointer of whoever established the session.
 */
LONG ugl_setsid(UG_A6)
{
    base->ug_Cred.cr_session = (LONG)FindTask(NULL);

    return base->ug_Cred.cr_session;
}

LONG ugl_getpgrp(UG_A6)
{
    if (base->ug_Cred.cr_session == 0)
        base->ug_Cred.cr_session = (LONG)FindTask(NULL);

    return base->ug_Cred.cr_session;
}

STRPTR ugl_getlogin(UG_A6)
{
    ug_resolve_login(base);

    return (STRPTR)base->ug_Cred.cr_login;
}

LONG ugl_setlogin(UG_A6, register STRPTR name __asm("a1"))
{
    if (!ug_privileged(base))
    {
        ug_set_err(base, UG_EPERM);
        return -1;
    }

    if (name == NULL || name[0] == '\0')
    {
        ug_set_err(base, UG_EINVAL);
        return -1;
    }

    if (ug_strlen((const char *)name) >= UG_MAXLOGNAME)
    {
        ug_set_err(base, UG_EINVAL);
        return -1;
    }

    ug_strncpy(base->ug_Cred.cr_login, (const char *)name,
               sizeof(base->ug_Cred.cr_login));
    ug_set_err(base, 0);

    return 0;
}
