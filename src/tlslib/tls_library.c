/*
 * tls.library, library skeleton.
 *
 * This file must be linked first so that the "moveq #-1,d0 / rts" below is at
 * offset 0 of the first code hunk, which makes the library file harmless if
 * executed.  Same rule as bsdsocket.library and usergroup.library.
 *
 *   bsdsocket.library clones a base per OpenLibrary() because the ABI puts
 *   errno, the descriptor table and the tag state *in* SocketBase, so two
 *   tasks that share one corrupt each other.  None of this library's state
 *   lives in the base.  A caller's state is entirely inside the TLSConnection
 *   it was handed.  One base is therefore correct, and cheaper.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_vectors.h"

#include "crypto68k.h"       /* c68k_cpu_select() */
#ifdef AMINETXDUO_CRYPTO68K_LTO_PROBE
#include "c68k_p256.h"
#endif
#include "aminetxduo/compat.h"   /* ami_rt_cpu_select() */

/* Included explicitly, not transitively.  NDK 3.2 reaches <exec/resident.h>
   through another header and NDK 3.9 does not, so an omission builds here and
   fails on a machine with the other NDK, which is how it reached CI.
   src/bsdsocket/bsdsocket_internal.h and src/usergroup/ug_library.c both name
   it for the same reason. */
#include <exec/execbase.h>   /* AttnFlags, for c68k_cpu_select() */
#include <exec/resident.h>
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
#ifdef AMINETXDUO_CRYPTO68K_LTO_PROBE
UINT probe_status;
#endif

    SysBase = sysbase;

    /* Which crypto primitives this machine wants, before any of them runs.
       In an AMINETXDUO_CPU=any build this is the whole of what makes one
       tls.library serve a 68000 and a 68060.  Anywhere else it does nothing.
       src/crypto68k/c68k_cpu.c. */
    c68k_cpu_select((ULONG)sysbase->AttnFlags);

    /* And the compiler runtime, for the same reason: the bignum code reaches
       __mulsi3 and __udivsi3 on every limb in a -m68000 build.
       src/common/ami_udivdi3.c. */
    ami_rt_cpu_select(((ULONG)sysbase->AttnFlags & AFF_68020) != 0UL,
                      ((ULONG)sysbase->AttnFlags & AFF_68020) != 0UL &&
                      ((ULONG)sysbase->AttnFlags & AFF_68060) == 0UL);

#ifdef AMINETXDUO_CRYPTO68K_LTO_PROBE
    /* Diagnostic only: preserve the product's initialization order, then run
       the known-answer check without a socket, interface, card or peer. */
    probe_status = c68k_p256_self_check();
#endif

    if (!tls_runtime_open())
        return NULL;

    base->tb_SegList = seglist;
    base->tb_SysBase = sysbase;

    /* The profiler's segment tag.  The sum is computed so that the magic by
       itself, found by accident in somebody's data, is not enough to pass. */
    base->tb_ProfMagic   = 0x50534731UL;    /* 'PSG1' */
    base->tb_ProfSize    = 5UL * sizeof(ULONG);
    base->tb_ProfLibBase = (ULONG)base;
    base->tb_ProfSegList = (ULONG)seglist;
    base->tb_ProfSum     = 0UL - (base->tb_ProfMagic + base->tb_ProfSize +
                                  base->tb_ProfLibBase + base->tb_ProfSegList);

    base->tb_Lib.lib_Node.ln_Type = NT_LIBRARY;
    base->tb_Lib.lib_Node.ln_Name = tls_lib_name;
    base->tb_Lib.lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    base->tb_Lib.lib_Version      = TLS_LIB_VERSION;
    base->tb_Lib.lib_Revision     = TLS_LIB_REVISION;
    base->tb_Lib.lib_IdString     = tls_lib_id;

#ifdef AMINETXDUO_CRYPTO68K_LTO_PROBE
    /* The loader only sees struct Library.  Revision is a harmless channel
       for the diagnostic result in a build which is explicitly unshippable. */
    base->tb_Lib.lib_Revision = (UWORD)probe_status;
#endif

    InitSemaphore(&base->tb_Lock);

    base->tb_CryptoReady = FALSE;

    /* Silences the unused warning.  The helper is kept for a future
       per-opener base. */
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
           and TLSClose() freed it. */
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

    /* The session cache is the one thing the base owns outright, allocated
       lazily on the first handshake that has something to remember.  It holds
       master secrets, so it is cleared and not merely freed. */
    if (TLSBase->tb_Sessions != NULL)
    {
        tls_bzero(TLSBase->tb_Sessions,
                  TLS_RESUME_SLOTS * (ULONG)sizeof(TLSResumeEntry));
        tls_free(TLSBase->tb_Sessions);
        TLSBase->tb_Sessions = NULL;
    }

    tls_runtime_close();

    Remove((struct Node *)TLSBase);
    FreeMem((UBYTE *)TLSBase - neg, neg + pos);

    return seglist;
}

APTR tls_lib_reserved(VOID)
{
    return NULL;
}
