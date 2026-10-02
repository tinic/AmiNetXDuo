/*
 * Integer-only printf for the Dropbear clients.
 *
 * build.sh links ssh and scp with --wrap=vfprintf,_vfprintf_r,_svfprintf_r,
 * so every printf, fprintf, snprintf, vsnprintf and vfprintf formats through
 * newlib's iprintf engine.  Neither client prints a float: the one %f in
 * Dropbear is the DEBUG_TRACE timestamp, and build.sh drops the wraps under
 * -T.  What stays out of the link is vfprintf.o, svfprintf.o, dtoa.o, mprec.o
 * and the soft-float members they call.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdarg.h>
#include <stdio.h>

/* newlib-internal (libc/stdio/local.h, not installed). */
__stdargs int _svfiprintf_r(struct _reent *, FILE *, const char *, va_list);
__stdargs int __wrap__svfprintf_r(struct _reent *, FILE *, const char *,
                                  va_list);

__stdargs int __wrap__svfprintf_r(struct _reent *r, FILE *fp, const char *fmt,
                                  va_list ap)
{
    return _svfiprintf_r(r, fp, fmt, ap);
}

extern __typeof__(_vfprintf_r) __wrap__vfprintf_r;
int __wrap__vfprintf_r(struct _reent *r, FILE *fp, const char *fmt, va_list ap)
{
    return _vfiprintf_r(r, fp, fmt, ap);
}

/* scp.c calls vfprintf() itself, and vfprintf.o is the float engine. */
extern __typeof__(vfprintf) __wrap_vfprintf;
int __wrap_vfprintf(FILE *fp, const char *fmt, va_list ap)
{
    return vfiprintf(fp, fmt, ap);
}
