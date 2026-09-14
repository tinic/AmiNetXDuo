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

/*
 * A line is formatted whole before anything sees it, so a sink can be handed
 * a string.  On the logging task's stack: the port's tick task has 4 KB, and
 * the stack's own lines are short.  Anything past the end is dropped.
 */
#define AMI_LOG_LINE_MAX    160

typedef struct AmiLogLine
{
    char *at;
    char *end;
} AmiLogLine;

/* RawDoFmt callback: one character into the line. */
static VOID put_char(register UBYTE c __asm("d0"),
                     register AmiLogLine *line __asm("a3"))
{
    if (c != '\0' && line->at < line->end)
        *line->at++ = (char)c;
}

static AmiLogLineFn ami_log_line_fn;
static APTR         ami_log_line_ctx;

VOID ami_log_line_sink_set(AmiLogLineFn fn, APTR ctx)
{
    /* The function last on install and first on removal, so a logger between
       the two stores sees no sink or a whole one. */
    if (fn == NULL)
    {
        ami_log_line_fn  = NULL;
        ami_log_line_ctx = ctx;
    }
    else
    {
        ami_log_line_ctx = ctx;
        ami_log_line_fn  = fn;
    }
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

VOID ami_serial_logv(int level, const char *fmt, const void *args)
{
    static const char *const prefix[] = { "ERR ", "WARN", "INFO", "DBG ", "TRC " };
    char        text[AMI_LOG_LINE_MAX];
    AmiLogLine  line;
    const char *p;

    if (fmt == NULL)
        return;
    if (level < AMI_LOG_ERROR || level > AMI_LOG_TRACE)
        level = AMI_LOG_INFO;

    line.at  = text;
    line.end = text + sizeof(text) - 1;
    RawDoFmt((STRPTR)fmt, (APTR)args, (void (*)())put_char, &line);
    *line.at = '\0';

    RawPutChar('[');
    for (p = prefix[level]; *p != '\0'; p++)
        RawPutChar(*p);
    RawPutChar(']');
    RawPutChar(' ');
    for (p = text; *p != '\0'; p++)
        RawPutChar(*p);
    RawPutChar('\n');

    if (ami_log_line_fn != NULL)
        ami_log_line_fn(level, text, ami_log_line_ctx);
}

VOID ami_log(int level, const char *fmt, ...)
{
    va_list args;

    if (level > ami_log_max)
        return;

    va_start(args, fmt);
    ami_serial_logv(level, fmt, (const void *)args);
    va_end(args);
}
