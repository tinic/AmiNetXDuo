/*
 * bsdsocket.library, AmiTCP-compatible vsyslog().
 *
 * The public syslog() macro builds a RawDoFmt argument stream and calls this
 * LVO.  Log policy is per opener: SocketBaseTagList() owns the tag, facility,
 * option bits and priority mask in AmiSocketBase.  AmiNetXDuo has no resident
 * NETTRACE task or log-file queue, so an accepted message goes to the serial
 * diagnostic already present in every image.  No permanent buffer or task is
 * added for a function that must remain usable on a one-megabyte machine.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <sys/syslog.h>

#define BSD_SYSLOG_FORMAT_SIZE      1024UL
#define BSD_SYSLOG_FALLBACK_SIZE     128UL

typedef struct BsdSyslogBuilder
{
    char  *at;
    char  *end;
} BsdSyslogBuilder;

static VOID bsd_log_char(BsdSyslogBuilder *b, char c)
{
    if (b->at < b->end)
        *b->at++ = c;
}

/* Text copied into a RawDoFmt format string is data.  Quote '%' so a program
   name such as "fetch%20" cannot consume an argument or read past the caller's
   argument stream. */
static VOID bsd_log_data(BsdSyslogBuilder *b, const char *text)
{
    if (text == NULL)
        return;

    while (*text != '\0')
    {
        if (*text == '%')
        {
            if (b->end - b->at < 2)
                break;
            bsd_log_char(b, '%');
        }
        bsd_log_char(b, *text++);
    }
}

static VOID bsd_log_hex(BsdSyslogBuilder *b, unsigned long value)
{
    static const char digits[] = "0123456789abcdef";
    char tmp[2 * sizeof(value)];
    ULONG n = 0;

    do
    {
        tmp[n++] = digits[value & 15UL];
        value >>= 4;
    } while (value != 0UL && n < (ULONG)sizeof(tmp));

    while (n != 0UL)
        bsd_log_char(b, tmp[--n]);
}

/*
 * Copy the caller's format while expanding AmiTCP's %m extension.  %%m must
 * remain the literal "%m", so a doubled percent is copied as a pair before
 * looking for the extension.  Other directives are left for RawDoFmt and keep
 * consuming the caller's original packed argument stream.
 */
static VOID bsd_log_format(BsdSyslogBuilder *b, const char *fmt,
                           const char *error_text)
{
    while (*fmt != '\0' && b->at < b->end)
    {
        if (fmt[0] == '%' && fmt[1] == '%')
        {
            if (b->end - b->at < 2)
                break;
            *b->at++ = *fmt++;
            *b->at++ = *fmt++;
        }
        else if (fmt[0] == '%' && fmt[1] == 'm')
        {
            bsd_log_data(b, error_text);
            fmt += 2;
        }
        else if (fmt[0] == '%')
        {
            char *start = b->at;
            BOOL complete = FALSE;

            /* Keep a conversion indivisible when the 1 KiB ceiling cuts the
               message.  A dangling '%' or "%08l" makes RawDoFmt walk an
               argument stream with a different shape from the caller's. */
            while (*fmt != '\0' && b->at < b->end)
            {
                char c = *fmt++;
                *b->at++ = c;
                if (((c >= 'a' && c <= 'z') ||
                     (c >= 'A' && c <= 'Z')) &&
                    c != 'h' && c != 'l' && c != 'L')
                {
                    complete = TRUE;
                    break;
                }
            }
            if (!complete)
            {
                b->at = start;
                break;
            }
        }
        else
        {
            *b->at++ = *fmt++;
        }
    }
}

static int bsd_log_level(LONG priority)
{
    switch (LOG_PRI(priority))
    {
        case LOG_EMERG:
        case LOG_ALERT:
        case LOG_CRIT:
        case LOG_ERR:
            return AMI_LOG_ERROR;
        case LOG_WARNING:
            return AMI_LOG_WARN;
        case LOG_NOTICE:
        case LOG_INFO:
            return AMI_LOG_INFO;
        default:
            return AMI_LOG_DEBUG;
    }
}

VOID bsd_vsyslog(register LONG priority __asm("d0"),
                 register STRPTR fmt __asm("a0"),
                 register APTR args __asm("a1"),
                 register struct AmiSocketBase *SocketBase __asm("a6"))
{
    char fallback[BSD_SYSLOG_FALLBACK_SIZE];
    char *format;
    BOOL allocated;
    BsdSyslogBuilder b;
    LONG effective;
    const char *tag;

    if (SocketBase == NULL || fmt == NULL)
        return;

    if ((priority & ~(LOG_PRIMASK | LOG_FACMASK)) != 0)
        return;
    if ((SocketBase->sb_LogMask & LOG_MASK(LOG_PRI(priority))) == 0)
        return;

    effective = priority;
    if ((effective & LOG_FACMASK) == 0)
        effective |= SocketBase->sb_LogFacility;

    /* Keep AmiTCP's effective priority intact even though the serial sink has
       no facility router; its visible level is the priority component. */

    format = (char *)ami_alloc(BSD_SYSLOG_FORMAT_SIZE);
    allocated = (format != NULL);
    if (!allocated)
        format = fallback;

    b.at = format;
    b.end = format + (allocated ? BSD_SYSLOG_FORMAT_SIZE
                               : BSD_SYSLOG_FALLBACK_SIZE) - 1;
    tag = (const char *)SocketBase->sb_LogTag;

    if (tag != NULL)
        bsd_log_data(&b, tag);

    if ((SocketBase->sb_LogStat & LOG_PID) != 0)
    {
        const struct Task *task = SocketBase->sb_SysBase != NULL
                                ? SocketBase->sb_SysBase->ThisTask
                                : SocketBase->sb_Task;
        bsd_log_char(&b, '[');
        bsd_log_hex(&b, (unsigned long)task);
        bsd_log_char(&b, ']');
    }

    if (tag != NULL)
    {
        bsd_log_char(&b, ':');
        bsd_log_char(&b, ' ');
    }

    bsd_log_format(&b, (const char *)fmt,
                   bsd_errno_string(SocketBase->sb_Errno));
    *b.at = '\0';

    ami_serial_logv(bsd_log_level(effective), format, args);

    if (allocated)
        ami_free(format);
}
