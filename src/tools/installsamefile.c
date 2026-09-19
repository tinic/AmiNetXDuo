/*
 * InstallSameFile, answer whether two AmigaDOS names resolve to one object.
 *
 * Commodore Installer has checksums but no file-identity primitive.  A
 * checksum cannot distinguish the drawer's library from a byte-identical
 * copy earlier in LIBS:, which is exactly the distinction a private install
 * has to make.  Keep the missing primitive here, using dos.library's
 * SameLock(), and keep it beside the Installer rather than installing it.
 *
 * Exit status is RETURN_OK only for the same object, RETURN_WARN for two
 * different objects, and RETURN_ERROR when either name cannot be locked.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <proto/dos.h>

#include "aminetxduo/version.h"

const char *const tool_name = "InstallSameFile";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("InstallSameFile");

#define TEMPLATE "FIRST/A,SECOND/A"

enum
{
    ARG_FIRST = 0,
    ARG_SECOND,
    ARG_COUNT
};

int main(int argc, char **argv)
{
    LONG           args[ARG_COUNT] = { 0, 0 };
    struct RDArgs *rda;
    BPTR           first;
    BPTR           second;
    LONG           same;

    (VOID)argv;

    if (argc == 0)
        return RETURN_FAIL;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
        return RETURN_ERROR;

    first  = Lock((CONST_STRPTR)args[ARG_FIRST], ACCESS_READ);
    second = Lock((CONST_STRPTR)args[ARG_SECOND], ACCESS_READ);
    if (first == 0 || second == 0)
    {
        if (first != 0)
            UnLock(first);
        if (second != 0)
            UnLock(second);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    same = SameLock(first, second);
    UnLock(second);
    UnLock(first);
    FreeArgs(rda);

    return (same == LOCK_SAME) ? RETURN_OK : RETURN_WARN;
}
