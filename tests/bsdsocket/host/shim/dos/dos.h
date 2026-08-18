/* dos.library basics, for the bsdsocket host tests.
   SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_BSD_TEST_DOS_DOS_H
#define AMINETXDUO_BSD_TEST_DOS_DOS_H
typedef LONG BPTR;
typedef LONG BSTR;
#define DOSTRUE   (-1L)
#define DOSFALSE  (0L)
#define RETURN_OK    0
#define RETURN_WARN  5
#define RETURN_ERROR 10
#define RETURN_FAIL  20
#define MODE_OLDFILE 1005
#define MODE_NEWFILE 1006
#define SIGBREAKB_CTRL_C 12
#define SIGBREAKF_CTRL_C (1L << SIGBREAKB_CTRL_C)
#define MKBADDR(x) (((LONG)(x)) >> 2)
#define BADDR(x)   ((APTR)((ULONG)(x) << 2))
#endif
