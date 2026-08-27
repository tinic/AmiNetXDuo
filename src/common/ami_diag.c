/*
 * AmiNetXDuo, the serial diagnostic.
 *
 * Its own translation unit and not part of compat.c because tls.library links
 * neither compat.c nor the aminetxduo_common archive -- it runs on ANY
 * bsdsocket.library and carries only the files it truly needs -- yet
 * src/common/ami_random.c, which it does link, calls AMI_INFO().  One
 * implementation, named by both builds; see the header comment in
 * src/tlslib/CMakeLists.txt.
 *
 * No dependencies beyond exec.library: this code runs inside a shared library,
 * so it must not pull in the newlib stdio.
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/compat.h"

#include <exec/execbase.h>
#include <proto/exec.h>
#include <inline/macros.h>

#include <stdarg.h>

/* Where ami_log_level() starts.  A build option only so a development build
   can bake a louder one in; the shipped value is what a field machine gets
   before anybody touches ENV:ANXDLOGLEVEL. */
#ifndef AMINETXDUO_LOG_LEVEL
#  ifdef AMINETXDUO_DEBUG
#    define AMINETXDUO_LOG_LEVEL AMI_LOG_DEBUG
#  else
#    define AMINETXDUO_LOG_LEVEL AMI_LOG_WARN
#  endif
#endif

/*
 * RawPutChar is an exec LVO (-516) that the NDK declares only in the assembler
 * headers, so it is declared here. Serial debug output is the one sink that a
 * shared library always reaches.
 */
#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

/* RawDoFmt callback: one character to the serial debug port. */
static VOID put_char(register UBYTE c   __asm("d0"),
                     register APTR unused __asm("a3"))
{
    (VOID)unused;
    if (c != '\0')
        RawPutChar(c);
}

/*
 * Plain int, no lock: a torn read cannot happen on a 68k word-aligned longword
 * and the worst a race costs is one line printed or dropped around the moment
 * somebody changes it.
 */
static int ami_log_max = AMINETXDUO_LOG_LEVEL;

VOID ami_log_level_set(int level)
{
    if (level < AMI_LOG_ERROR)
        level = AMI_LOG_ERROR;
    else if (level > AMI_LOG_TRACE)
        level = AMI_LOG_TRACE;

    ami_log_max = level;
}

int ami_log_level(VOID)
{
    return ami_log_max;
}

VOID ami_log(int level, const char *fmt, ...)
{
    static const char *const prefix[] = { "ERR ", "WARN", "INFO", "DBG ", "TRC " };
    va_list args;

    if (level > ami_log_max)
        return;
    if (level < AMI_LOG_ERROR || level > AMI_LOG_TRACE)
        level = AMI_LOG_INFO;

    RawPutChar('[');
    {
        const char *p = prefix[level];
        while (*p != '\0')
            RawPutChar(*p++);
    }
    RawPutChar(']');
    RawPutChar(' ');

    va_start(args, fmt);
    RawDoFmt((STRPTR)fmt, args, (void (*)())put_char, NULL);
    va_end(args);

    RawPutChar('\n');
}

