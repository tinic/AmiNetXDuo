/*
 * Minimal <exec/types.h> for the host-side config parser test only.
 * Never compiled for the Amiga, the NDK header is used there.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TEST_EXEC_TYPES_H
#define AMINETXDUO_TEST_EXEC_TYPES_H

typedef unsigned char   UBYTE;
typedef signed   char   BYTE;
typedef unsigned short  UWORD;
typedef signed   short  WORD;
typedef unsigned int    ULONG;
typedef signed   int    LONG;
typedef unsigned char  *STRPTR;
typedef unsigned char   TEXT;
typedef void           *APTR;
typedef void            VOID;
typedef short           BOOL;

#ifndef NULL
#  define NULL ((void *)0)
#endif
#ifndef TRUE
#  define TRUE  1
#endif
#ifndef FALSE
#  define FALSE 0
#endif

#endif /* AMINETXDUO_TEST_EXEC_TYPES_H */
