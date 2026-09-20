/* Stable machine/card input for a derived station address.
 * SPDX-License-Identifier: MIT
 */

#include "netdev_internal.h"
#include "netdev_macgen.h"

#include <exec/execbase.h>
#include <exec/memory.h>
#include <libraries/configvars.h>

#include <proto/exec.h>
#include <proto/expansion.h>

extern struct ExecBase      *SysBase;
extern struct ExpansionBase *ExpansionBase;

static VOID fp_put(UBYTE *buf, UWORD max, UWORD *n, ULONG v, UBYTE bytes)
{
    UBYTE i;

    for (i = 0; i < bytes; i++)
    {
        if (*n >= max)
            return;
        buf[(*n)++] = (UBYTE)(v >> ((bytes - 1u - i) * 8u));
    }
}

/* A card whose address PROM reads all-zero or all-ones needs a derived
   address.  The requirement is not uniqueness but "the same on every boot of
   this machine, and different from the next machine wherever the two machines
   differ". */
UWORD netdev_mac_fingerprint(UBYTE *buf, UWORD max, ULONG salt)
{
    UWORD n = 0;
    UWORD i;

    fp_put(buf, max, &n, salt, 4);

    if (SysBase != NULL)
    {
        struct MemHeader *mh;

        fp_put(buf, max, &n, (ULONG)SysBase->AttnFlags, 2);
        fp_put(buf, max, &n, (ULONG)SysBase->LibNode.lib_Version, 2);
        fp_put(buf, max, &n, (ULONG)SysBase->LibNode.lib_Revision, 2);
        fp_put(buf, max, &n, SysBase->ex_EClockFrequency, 4);
        fp_put(buf, max, &n, (ULONG)SysBase->MaxLocMem, 4);

        /* Forbid(), not Disable(): the list is only rearranged by AddMemList
           and by a task, and this runs at probe time where a Disable() would
           be the heavier of the two for no gain. */
        Forbid();
        i = 0;
        for (mh = (struct MemHeader *)SysBase->MemList.lh_Head;
             mh->mh_Node.ln_Succ != NULL && i < 4; i++)
        {
            fp_put(buf, max, &n, (ULONG)mh->mh_Attributes, 2);
            fp_put(buf, max, &n, (ULONG)(APTR)mh->mh_Lower, 4);
            fp_put(buf, max, &n, (ULONG)(APTR)mh->mh_Upper, 4);
            mh = (struct MemHeader *)mh->mh_Node.ln_Succ;
        }
        Permit();
    }

    if (ExpansionBase != NULL)
    {
        struct ConfigDev *cd = NULL;

        i = 0;
        while ((cd = FindConfigDev(cd, -1, -1)) != NULL && i < 4)
        {
            fp_put(buf, max, &n, (ULONG)cd->cd_Rom.er_Manufacturer, 2);
            fp_put(buf, max, &n, (ULONG)cd->cd_Rom.er_Product, 1);
            fp_put(buf, max, &n, cd->cd_Rom.er_SerialNumber, 4);
            i++;
        }
    }

    n = (UWORD)(n + netdev_pcmcia_fingerprint(buf + n, (UWORD)(max - n)));

#if defined(NETDEV_TRACE) || defined(NETDEV_TIME)
    netdev_trace_val("anx: fp bytes ", (ULONG)n);
#endif

    return n;
}
