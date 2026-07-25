/*
 * tls.library -- library skeleton.
 *
 * This file must be linked FIRST so that the "moveq #-1,d0 / rts" below is at
 * offset 0 of the first code hunk -- that is what makes the library file
 * harmless if someone tries to execute it.  Same rule as bsdsocket.library and
 * usergroup.library.
 *
 * WHY THERE IS NO PER-OPENER BASE
 *
 *   bsdsocket.library clones a base per OpenLibrary() because the ABI puts
 *   errno, the descriptor table and the tag state *in* SocketBase, so two
 *   tasks sharing one would corrupt each other.  Nothing in this library's
 *   contract lives in the base: a caller's state is entirely inside the
 *   TLSConnection it was handed.  One base is therefore correct, and cheaper.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_vectors.h"

#include <proto/exec.h>

struct ExecBase *SysBase;

/* Executing the library file returns a failure code rather than crashing. */
asm("    .text                       \n"
    "    .globl _tls_library_entry   \n"
    "_tls_library_entry:             \n"
    "    moveq  #-1,%d0              \n"
    "    rts                         \n");

static char tls_lib_name[] = TLS_LIB_NAME;
static char tls_lib_id[]   = "tls.library 1.0 (AmiNetXDuo)\r\n";

static struct TLSLibBase *tls_lib_init(
    register struct TLSLibBase *base    __asm("d0"),
    register APTR               seglist __asm("a0"),
    register struct ExecBase   *sysbase __asm("a6"));

static const APTR tls_init_table[4] =
{
    (APTR)sizeof(struct TLSLibBase),
    (APTR)TlsVectorTable,
    (APTR)NULL,
    (APTR)tls_lib_init
};

const struct Resident tls_romtag =
{
    RTC_MATCHWORD,
    (struct Resident *)&tls_romtag,
    (APTR)(&tls_romtag + 1),
    RTF_AUTOINIT,
    TLS_LIB_VERSION,
    NT_LIBRARY,
    0,
    tls_lib_name,
    tls_lib_id,
    (APTR)tls_init_table
};

static VOID tls_new_list(struct MinList *list)
{
    list->mlh_Head     = (struct MinNode *)&list->mlh_Tail;
    list->mlh_Tail     = NULL;
    list->mlh_TailPred = (struct MinNode *)&list->mlh_Head;
}

static struct TLSLibBase *tls_lib_init(
    register struct TLSLibBase *base    __asm("d0"),
    register APTR               seglist __asm("a0"),
    register struct ExecBase   *sysbase __asm("a6"))
{
    SysBase = sysbase;

    if (!tls_runtime_open())
        return NULL;

    base->tb_SegList = seglist;
    base->tb_SysBase = sysbase;

    base->tb_Lib.lib_Node.ln_Type = NT_LIBRARY;
    base->tb_Lib.lib_Node.ln_Name = tls_lib_name;
    base->tb_Lib.lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    base->tb_Lib.lib_Version      = TLS_LIB_VERSION;
    base->tb_Lib.lib_Revision     = TLS_LIB_REVISION;
    base->tb_Lib.lib_IdString     = tls_lib_id;

    InitSemaphore(&base->tb_Lock);

    base->tb_CryptoReady = FALSE;

    /* Silence the unused warning without pretending the list is used: the
       shape is here so a future per-opener base has somewhere to hang. */
    (VOID)tls_new_list;

    return base;
}

struct TLSLibBase *tls_lib_open(
    register ULONG              version __asm("d0"),
    register struct TLSLibBase *TLSBase __asm("a6"))
{
    (VOID)version;

    TLSBase->tb_Lib.lib_Flags &= ~LIBF_DELEXP;
    TLSBase->tb_Lib.lib_OpenCnt++;

    return TLSBase;
}

APTR tls_lib_close(register struct TLSLibBase *TLSBase __asm("a6"))
{
    if (TLSBase->tb_Lib.lib_OpenCnt > 0)
        TLSBase->tb_Lib.lib_OpenCnt--;

    if (TLSBase->tb_Lib.lib_OpenCnt == 0)
    {
        /* Nothing to release: the trust-store index belongs to a connection,
           and TLSClose() gave it back. */
        if ((TLSBase->tb_Lib.lib_Flags & LIBF_DELEXP) != 0)
            return tls_lib_expunge(TLSBase);
    }

    return NULL;
}

APTR tls_lib_expunge(register struct TLSLibBase *TLSBase __asm("a6"))
{
    APTR  seglist;
    ULONG neg, pos;

    if (TLSBase->tb_Lib.lib_OpenCnt > 0)
    {
        TLSBase->tb_Lib.lib_Flags |= LIBF_DELEXP;
        return NULL;
    }

    seglist = TLSBase->tb_SegList;
    neg     = TLSBase->tb_Lib.lib_NegSize;
    pos     = TLSBase->tb_Lib.lib_PosSize;

    tls_runtime_close();

    Remove((struct Node *)TLSBase);
    FreeMem((UBYTE *)TLSBase - neg, neg + pos);

    return seglist;
}

APTR tls_lib_reserved(VOID)
{
    return NULL;
}
