/*
 * AmiNetXDuo, "wait N seconds" for boot scripts.
 *
 * The bare directory-hardfile boot used by tools/amiberry-run.sh has no C:
 * commands at all, so there is no C:Wait and no C:Delay with which to give a
 * resident debugging tool time to install itself.  This is that, in 20 lines.
 *
 *   waitsecs 5
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <proto/dos.h>

int main(void)
{
    STRPTR args;
    LONG   secs = 5;
    LONG   n    = 0;

    /* argv[] is unreliable with this toolchain's CLI start-up; the raw command
       tail from GetArgStr() is not.  */
    args = (STRPTR)GetArgStr();
    if (args != NULL)
    {
        while (*args == ' ' || *args == '\t')
            args++;
        while (*args >= '0' && *args <= '9')
        {
            n = (n * 10) + (*args - '0');
            args++;
        }
        if (n > 0)
            secs = n;
    }

    Delay(secs * 50L);          /* Delay() ticks are 1/50 s */
    return 0;
}
