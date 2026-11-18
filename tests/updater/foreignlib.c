/*
 * foreignlib, another stack's bsdsocket.library resident in memory, for the
 * updater scenario S24.
 *
 *     Run >NIL: foreignlib
 *
 * Builds a library node named bsdsocket.library whose IdString is not
 * AmiNetXDuo's, adds it to exec's list and waits for CTRL-C.  Only Open,
 * Close and Expunge exist: a program that reads the IdString works, one that
 * calls a socket function would crash, and in S24 nothing may.  Expunge
 * refuses, so the node stays while this process lives.
 *
 * Compiled by tests/updater/scenario-arm.sh with the harness's m68k gcc.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/nodes.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

static const char lib_name[] = "bsdsocket.library";
static const char lib_id[] = "bsdsocket.library 4.364 (Roadshow fixture)\r\n";

static struct Library *lib_open(register struct Library *base __asm("a6"))
{
    base->lib_OpenCnt++;
    return base;
}

static BPTR lib_close(register struct Library *base __asm("a6"))
{
    base->lib_OpenCnt--;
    return 0;
}

static BPTR lib_expunge(void)
{
    return 0;
}

static ULONG lib_null(void)
{
    return 0;
}

static const APTR vectors[] =
{
    (APTR)lib_open, (APTR)lib_close, (APTR)lib_expunge, (APTR)lib_null,
    (APTR)-1
};

int main(void)
{
    struct Library *lib;

    lib = MakeLibrary((APTR)vectors, NULL, NULL, sizeof(struct Library), 0);
    if (lib == NULL)
    {
        PutStr("foreignlib=FAIL make\n");
        return RETURN_FAIL;
    }
    lib->lib_Node.ln_Type = NT_LIBRARY;
    lib->lib_Node.ln_Name = (char *)lib_name;
    lib->lib_IdString = (APTR)lib_id;
    lib->lib_Version = 4;
    lib->lib_Revision = 364;
    lib->lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    AddLibrary(lib);
    PutStr("foreignlib=resident\n");
    Wait(SIGBREAKF_CTRL_C);
    Forbid();
    Remove(&lib->lib_Node);
    Permit();
    return RETURN_OK;
}
