/*
 * <exec/types.h> for the sana2 host test.
 *
 * Not src/config/test/shim's copy, and the difference is load-bearing: the NDK
 * spells VOID as a macro, and aminetxduo/sana2.h undefines and restores it
 * around tx_api.h for exactly that reason. A typedef there collides instead.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_SANA2_TEST_EXEC_TYPES_H
#define AMINETXDUO_SANA2_TEST_EXEC_TYPES_H

typedef unsigned char   UBYTE;
typedef signed   char   BYTE;
typedef unsigned short  UWORD;
typedef signed   short  WORD;
typedef unsigned int    ULONG;
typedef signed   int    LONG;
typedef unsigned char  *STRPTR;
typedef unsigned char   TEXT;
typedef const unsigned char *CONST_STRPTR;
typedef void           *APTR;
typedef short           BOOL;

#ifndef VOID
#  define VOID void
#endif

#ifndef NULL
#  define NULL ((void *)0)
#endif
#ifndef TRUE
#  define TRUE  1
#endif
#ifndef FALSE
#  define FALSE 0
#endif

#endif
