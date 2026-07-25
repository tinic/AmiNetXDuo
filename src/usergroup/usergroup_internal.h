/*
 * AmiNetXDuo -- usergroup.library internals.
 *
 * usergroup.library is AmiTCP's companion to bsdsocket.library. Ported Unix
 * software links against it for the BSD user/group database calls; without it
 * those binaries fail to load. See docs/RESEARCH.md §3.3.
 *
 * ------------------------------------------------------------------ ABI ---
 *
 * The vector table below is CONFIRMED, not inferred. Five independent sources
 * in two lineages agree function-for-function and register-for-register:
 *
 *   1. AmiTCP  fd/usergroup_lib.fd     (c) 1993 AmiTCP/IP Group, HUT Finland
 *   2. Roadshow sfd/usergroup_lib.sfd  $Id: v1.4 2004-09-16 obarthel $
 *   3. Roadshow NDK pragmas/usergroup_pragmas.h + inline/usergroup.h
 *      (fd2pragma V2.164 output of #2, in the local toolchain)
 *   4. The toolchain's own AmiTCP-derived sys-include/inline/usergroup.h
 *      (libnix LP-macro style, UG_-prefixed)
 *   5. clib2 usergroup_headers.h
 *
 *   ##base _UserGroupBase   ##bias 30   39 public vectors, LVO -30 .. -258.
 *
 * The passwd/group record layouts are equally confirmed: AmiTCP's netinclude
 * pwd.h and Roadshow's netinclude pwd.h define byte-identical structures
 * (7 fields, no pw_change/pw_class/pw_expire).
 *
 * NOTE: the local toolchain's ndk-include/pwd.h is NOT the usergroup ABI. It
 * is newlib's own 10-field 4.4BSD struct passwd, substituted over Roadshow's.
 * That is exactly why the toolchain's AmiTCP inline header spells the return
 * type "struct TCP_passwd" -- the 7-field layout we use here.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_USERGROUP_INTERNAL_H
#define AMINETXDUO_USERGROUP_INTERNAL_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <utility/tagitem.h>

/* ------------------------------------------------------------ ABI records --
 *
 * Layout-compatible with AmiTCP's and Roadshow's <pwd.h>, <grp.h>, <utmp.h>
 * and <libraries/usergroup.h>. Deliberately given our own tag names so they
 * cannot collide with newlib's incompatible <pwd.h>/<grp.h>.
 */

#define UG_NGROUPS        32    /* NGROUPS       */
#define UG_MAXLOGNAME     32    /* MAXLOGNAME    */
#define UG_PASSWORD_LEN  128    /* _PASSWORD_LEN */
#define UG_UT_NAMESIZE    32
#define UG_UT_HOSTSIZE    64

struct ug_passwd {
    char  *pw_name;             /* user name           */
    char  *pw_passwd;           /* encrypted password  */
    LONG   pw_uid;              /* user uid            */
    LONG   pw_gid;              /* user gid            */
    char  *pw_gecos;            /* real name etc.      */
    char  *pw_dir;              /* home directory      */
    char  *pw_shell;            /* default shell       */
};

struct ug_group {
    char  *gr_name;
    char  *gr_passwd;
    LONG   gr_gid;
    char **gr_mem;              /* NULL-terminated     */
};

struct ug_utmp {
    LONG   ut_time;
    LONG   ut_sid;
    char   ut_name[UG_UT_NAMESIZE];
    char   ut_host[UG_UT_HOSTSIZE];
};

struct ug_lastlog {
    LONG   ll_time;
    LONG   ll_uid;
    char   ll_name[UG_UT_NAMESIZE];
    char   ll_host[UG_UT_HOSTSIZE];
};

/*
 * struct UserGroupCredentials. cr_umask is a UWORD between two LONGs, so this
 * only matches on a 2-byte-alignment ABI -- which is what m68k gives us and
 * what Roadshow's "#pragma pack(2)" forces on PPC. Asserted in ug_library.c.
 */
struct ug_credentials {
    LONG   cr_ruid;
    LONG   cr_rgid;
    UWORD  cr_umask;
    LONG   cr_euid;
    WORD   cr_ngroups;
    LONG   cr_groups[UG_NGROUPS];
    LONG   cr_session;
    char   cr_login[UG_MAXLOGNAME];
};

/* Context tags, from <libraries/usergroup.h> (identical in both lineages). */
#define UGT_ERRNOBPTR   0x80000001
#define UGT_ERRNOWPTR   0x80000002
#define UGT_ERRNOLPTR   0x80000004
#define UGT_INTRMASK    0x80000010
#define UGT_OWNER       0x80000011

/*
 * errno values, from Roadshow netinclude/sys/errno.h (4.4BSD numbering).
 * These travel through the caller's UGT_ERRNOPTR, so they are ABI.
 */
#define UG_EPERM         1
#define UG_ENOENT        2
#define UG_ESRCH         3
#define UG_ENOMEM       12
#define UG_EACCES       13
#define UG_EFAULT       14
#define UG_EINVAL       22
#define UG_ENOTTY       25
#define UG_ERANGE       34
#define UG_ENOSYS       78

/* ---------------------------------------------------------- the database --
 *
 * Parsed once, shared by every opener, immutable thereafter. Only the
 * iteration cursor and the returned record are per-opener.
 */

#define UG_MAX_PASSWD    64
#define UG_MAX_GROUP     64
#define UG_MAX_FILE   32768     /* refuse to read more than this */

struct UgDatabase {
    struct ug_passwd  pw[UG_MAX_PASSWD];
    struct ug_group   gr[UG_MAX_GROUP];
    UWORD             pw_count;
    UWORD             gr_count;
    char             *pw_text;      /* file image; the strings point into it */
    char             *gr_text;
    char            **gr_members;   /* arena for the gr_mem vectors          */
    BOOL             pw_loaded;
    BOOL             gr_loaded;
};

/* State shared by the master base and every child base. */
struct UgGlobal {
    struct SignalSemaphore lock;
    struct MinList         children;
    struct UgDatabase      db;
    struct DosLibrary     *dosbase;     /* opened lazily, may stay NULL */
    BOOL                   dos_tried;
};

/* ------------------------------------------------------------ library base -
 *
 * Every opener gets its own copy (a "child base"), so the get*ent cursors and
 * the buffers behind getpwnam()/crypt()/getpass() are per-task by
 * construction. This is the same discipline bsdsocket.library uses for
 * SocketBase (docs/RESEARCH.md §3.1).
 */
struct UserGroupBase {
    struct Library         lib;

    struct ExecBase       *ug_SysBase;
    BPTR                   ug_SegList;      /* master only */
    struct UserGroupBase  *ug_Parent;       /* NULL in the master base */
    struct UgGlobal       *ug_Global;

    /* --- per-opener context ------------------------------------------- */
    struct MinNode         ug_Node;         /* link into ug_Global->children */
    struct Task           *ug_Owner;
    APTR                   ug_ErrnoPtr;
    UWORD                  ug_ErrnoSize;
    ULONG                  ug_IntrMask;
    LONG                   ug_Err;
    struct ug_credentials  ug_Cred;
    UWORD                  ug_PwCursor;
    UWORD                  ug_GrCursor;
    struct ug_passwd       ug_PwResult;
    struct ug_group        ug_GrResult;
    char                   ug_ProgName[32];
    char                   ug_PassBuf[UG_PASSWORD_LEN + 1];
    char                   ug_SaltBuf[16];
};

/* --------------------------------------------------------------- helpers -- */

/* ug_library.c */
struct DosLibrary *ug_dos(struct UserGroupBase *base);
void   ug_db_free(struct UgGlobal *g);
ULONG  ug_strlen(const char *s);
void   ug_strncpy(char *dst, const char *src, ULONG size);
int    ug_strcmp(const char *a, const char *b);
void   ug_newlist(struct MinList *list);

/* ug_context.c */
void   ug_set_err(struct UserGroupBase *base, LONG err);
void   ug_context_init(struct UserGroupBase *base);
void   ug_resolve_login(struct UserGroupBase *base);

/* ug_db.c */
void   ug_db_require_passwd(struct UserGroupBase *base);
void   ug_db_require_group(struct UserGroupBase *base);
char  *ug_db_read_file(struct UserGroupBase *base, const char *path, ULONG *len_out);

#endif /* AMINETXDUO_USERGROUP_INTERNAL_H */
