/*
 * AmiNetXDuo, usergroup.library public vectors.
 *
 * One declaration per LVO, in table order, with the register assignment each
 * argument must arrive in. Transcribed verbatim from the AmiTCP
 * fd/usergroup_lib.fd and the Roadshow sfd/usergroup_lib.sfd, which agree
 * exactly. See the ABI note at the top of usergroup_internal.h.
 *
 * Both the table in ug_library.c and every implementation file include this,
 * so a signature cannot drift away from the ABI unnoticed.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_USERGROUP_VECTORS_H
#define AMINETXDUO_USERGROUP_VECTORS_H

#include "usergroup_internal.h"

/*
 * The register assignment IS the ABI on m68k, so it is not optional there.
 * Off-target it is not expressible -- the host has no a6 -- and without a
 * host branch this header cannot be included by a host test, which is why
 * ugl_getcredentials() shipped the v0.26.3 NFS-mount defect with no coverage.
 * The declaration text stays single-sourced either way: one macro pair, so a
 * host build cannot drift from the vector table it is testing.
 */
#if defined(__m68k__)
#define UG_A6            register struct UserGroupBase *base __asm("a6")
#define UG_REG(decl, r)  register decl __asm(r)
#else
#define UG_A6            struct UserGroupBase *base
#define UG_REG(decl, r)  decl
#endif

/* bias 30, setup ------------------------------------------------------- */
LONG   ugl_SetupContextTagList(UG_A6, UG_REG(STRPTR name, "a0"),
                               UG_REG(struct TagItem *tags, "a1"));
LONG   ugl_GetErr  (UG_A6);
STRPTR ugl_StrError(UG_A6, UG_REG(LONG err, "d1"));

/* user identification ---------------------------------------------------- */
LONG   ugl_getuid  (UG_A6);
LONG   ugl_geteuid (UG_A6);
LONG   ugl_setreuid(UG_A6, UG_REG(LONG real, "d0"),
                           UG_REG(LONG effective, "d1"));
LONG   ugl_setuid  (UG_A6, UG_REG(LONG uid, "d0"));

/* group membership ------------------------------------------------------- */
LONG   ugl_getgid  (UG_A6);
LONG   ugl_getegid (UG_A6);
LONG   ugl_setregid(UG_A6, UG_REG(LONG real, "d0"),
                           UG_REG(LONG effective, "d1"));
LONG   ugl_setgid  (UG_A6, UG_REG(LONG gid, "d0"));
LONG   ugl_getgroups (UG_A6, UG_REG(LONG gidsetlen, "d0"),
                             UG_REG(LONG *gidset, "a1"));
LONG   ugl_setgroups (UG_A6, UG_REG(LONG gidsetlen, "d0"),
                             UG_REG(LONG *gidset, "a1"));
LONG   ugl_initgroups(UG_A6, UG_REG(STRPTR name, "a1"),
                             UG_REG(LONG basegid, "d0"));

/* user database ---------------------------------------------------------- */
struct ug_passwd *ugl_getpwnam(UG_A6, UG_REG(STRPTR login, "a1"));
struct ug_passwd *ugl_getpwuid(UG_A6, UG_REG(LONG uid, "d0"));
VOID              ugl_setpwent(UG_A6);
struct ug_passwd *ugl_getpwent(UG_A6);
VOID              ugl_endpwent(UG_A6);

/* group database --------------------------------------------------------- */
struct ug_group  *ugl_getgrnam(UG_A6, UG_REG(STRPTR name, "a1"));
struct ug_group  *ugl_getgrgid(UG_A6, UG_REG(LONG gid, "d0"));
VOID              ugl_setgrent(UG_A6);
struct ug_group  *ugl_getgrent(UG_A6);
VOID              ugl_endgrent(UG_A6);

/* password handling ------------------------------------------------------ */
UBYTE *ugl_crypt    (UG_A6, UG_REG(UBYTE *key, "a0"),
                            UG_REG(UBYTE *set, "a1"));
UBYTE *ugl_GetSalt  (UG_A6, UG_REG(struct ug_passwd *user, "a0"),
                            UG_REG(UBYTE *buf, "a1"),
                            UG_REG(ULONG size, "d0"));
STRPTR ugl_getpass  (UG_A6, UG_REG(STRPTR prompt, "a1"));

/* default protections ---------------------------------------------------- */
ULONG  ugl_umask    (UG_A6, UG_REG(ULONG mask, "d0"));
ULONG  ugl_getumask (UG_A6);

/* sessions --------------------------------------------------------------- */
LONG   ugl_setsid   (UG_A6);
LONG   ugl_getpgrp  (UG_A6);
STRPTR ugl_getlogin (UG_A6);
LONG   ugl_setlogin (UG_A6, UG_REG(STRPTR name, "a1"));

/* user login database (utmp) --------------------------------------------- */
VOID               ugl_setutent  (UG_A6);
struct ug_utmp    *ugl_getutent  (UG_A6);
VOID               ugl_endutent  (UG_A6);
struct ug_lastlog *ugl_getlastlog(UG_A6, UG_REG(LONG uid, "d0"));
LONG               ugl_setlastlog(UG_A6, UG_REG(LONG uid, "d0"),
                                         UG_REG(STRPTR name, "a0"),
                                         UG_REG(STRPTR host, "a1"));

/* credentials ------------------------------------------------------------ */
struct ug_credentials *ugl_getcredentials(UG_A6,
                                          UG_REG(struct Task *task, "a0"));

#endif /* AMINETXDUO_USERGROUP_VECTORS_H */
