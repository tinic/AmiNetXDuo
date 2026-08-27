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
#define SHARED_LOCK    (-2L)
#define EXCLUSIVE_LOCK (-1L)

/* The DOS error codes tcp_handler.c returns, values from NDK dos/dos.h.  Only
   the ones the handler names: a code nothing returns is a number a reader can
   be wrong about with nothing to catch it. */
#define ERROR_NO_FREE_STORE       103
#define ERROR_BAD_NUMBER          115
#define ERROR_OBJECT_IN_USE       202
#define ERROR_OBJECT_NOT_FOUND    205
#define ERROR_ACTION_NOT_KNOWN    209
#define ERROR_OBJECT_WRONG_TYPE   212
#define ERROR_DEVICE_NOT_MOUNTED  218
#define ERROR_SEEK_ERROR          219
#define ERROR_READ_PROTECTED      224

#define MKBADDR(x) (((LONG)(x)) >> 2)
#define BADDR(x)   ((APTR)((ULONG)(x) << 2))
#endif
