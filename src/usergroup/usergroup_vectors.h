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

#define UG_A6   register struct UserGroupBase *base __asm("a6")

/* bias 30, setup ------------------------------------------------------- */
LONG   ugl_SetupContextTagList(UG_A6, register STRPTR name __asm("a0"),
                               register struct TagItem *tags __asm("a1"));
LONG   ugl_GetErr  (UG_A6);
STRPTR ugl_StrError(UG_A6, register LONG err __asm("d1"));

/* user identification ---------------------------------------------------- */
LONG   ugl_getuid  (UG_A6);
LONG   ugl_geteuid (UG_A6);
LONG   ugl_setreuid(UG_A6, register LONG real __asm("d0"),
                           register LONG effective __asm("d1"));
LONG   ugl_setuid  (UG_A6, register LONG uid __asm("d0"));

/* group membership ------------------------------------------------------- */
LONG   ugl_getgid  (UG_A6);
LONG   ugl_getegid (UG_A6);
LONG   ugl_setregid(UG_A6, register LONG real __asm("d0"),
                           register LONG effective __asm("d1"));
LONG   ugl_setgid  (UG_A6, register LONG gid __asm("d0"));
LONG   ugl_getgroups (UG_A6, register LONG gidsetlen __asm("d0"),
                             register LONG *gidset __asm("a1"));
LONG   ugl_setgroups (UG_A6, register LONG gidsetlen __asm("d0"),
                             register LONG *gidset __asm("a1"));
LONG   ugl_initgroups(UG_A6, register STRPTR name __asm("a1"),
                             register LONG basegid __asm("d0"));

/* user database ---------------------------------------------------------- */
struct ug_passwd *ugl_getpwnam(UG_A6, register STRPTR login __asm("a1"));
struct ug_passwd *ugl_getpwuid(UG_A6, register LONG uid __asm("d0"));
VOID              ugl_setpwent(UG_A6);
struct ug_passwd *ugl_getpwent(UG_A6);
VOID              ugl_endpwent(UG_A6);

/* group database --------------------------------------------------------- */
struct ug_group  *ugl_getgrnam(UG_A6, register STRPTR name __asm("a1"));
struct ug_group  *ugl_getgrgid(UG_A6, register LONG gid __asm("d0"));
VOID              ugl_setgrent(UG_A6);
struct ug_group  *ugl_getgrent(UG_A6);
VOID              ugl_endgrent(UG_A6);

/* password handling ------------------------------------------------------ */
UBYTE *ugl_crypt    (UG_A6, register UBYTE *key __asm("a0"),
                            register UBYTE *set __asm("a1"));
UBYTE *ugl_GetSalt  (UG_A6, register struct ug_passwd *user __asm("a0"),
                            register UBYTE *buf __asm("a1"),
                            register ULONG size __asm("d0"));
STRPTR ugl_getpass  (UG_A6, register STRPTR prompt __asm("a1"));

/* default protections ---------------------------------------------------- */
ULONG  ugl_umask    (UG_A6, register ULONG mask __asm("d0"));
ULONG  ugl_getumask (UG_A6);

/* sessions --------------------------------------------------------------- */
LONG   ugl_setsid   (UG_A6);
LONG   ugl_getpgrp  (UG_A6);
STRPTR ugl_getlogin (UG_A6);
LONG   ugl_setlogin (UG_A6, register STRPTR name __asm("a1"));

/* user login database (utmp) --------------------------------------------- */
VOID               ugl_setutent  (UG_A6);
struct ug_utmp    *ugl_getutent  (UG_A6);
VOID               ugl_endutent  (UG_A6);
struct ug_lastlog *ugl_getlastlog(UG_A6, register LONG uid __asm("d0"));
LONG               ugl_setlastlog(UG_A6, register LONG uid __asm("d0"),
                                         register STRPTR name __asm("a0"),
                                         register STRPTR host __asm("a1"));

/* credentials ------------------------------------------------------------ */
struct ug_credentials *ugl_getcredentials(UG_A6,
                                          register struct Task *task __asm("a0"));

#endif /* AMINETXDUO_USERGROUP_VECTORS_H */
