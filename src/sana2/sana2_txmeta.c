/* Per-write metadata callback for AmiNetXDuo's private SANA-II extension. */

#include "sana2_internal.h"

/* A driver may call this while advancing a queued write from interrupt
   context.  ios2_Data is the opener-owned slot for every request we submit,
   so the entire callback is deliberately one guarded byte load. */
UBYTE ami_sana2_tx_flags(APTR ios2_data)
{
    const AmiTxSlot *slot = (const AmiTxSlot *)ios2_data;

    return (slot != NULL) ? slot->tx_flags : 0;
}
