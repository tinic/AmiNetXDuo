/*
 * Minimal NDK scalar types for tls.library host tests.
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TLS_TEST_EXEC_TYPES_H
#define AMINETXDUO_TLS_TEST_EXEC_TYPES_H

typedef unsigned char   UBYTE;
typedef signed char     BYTE;
typedef unsigned short  UWORD;
typedef signed short    WORD;
typedef unsigned int    ULONG;
typedef signed int      LONG;
typedef unsigned char  *STRPTR;
typedef const char     *CONST_STRPTR;
typedef void           *APTR;
typedef const void     *CONST_APTR;
typedef void            VOID;
typedef short           BOOL;

#ifndef NULL
#  define NULL ((void *)0)
#endif
#ifndef TRUE
#  define TRUE 1
#endif
#ifndef FALSE
#  define FALSE 0
#endif

#endif
