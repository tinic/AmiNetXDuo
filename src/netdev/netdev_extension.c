/* Version and feature validation for the private SANA-II extension. */

#include "netdev_internal.h"

BOOL netdev_take_extension(AnxdS2Extension *ext, NetdevOpener *op,
                           AnxdS2Extension **answer)
{
    ULONG request;

    if (ext == NULL || op == NULL || answer == NULL ||
        ext->Version != ANXD_S2_ABI_VERSION ||
        ext->Size < (UWORD)sizeof(*ext))
        return FALSE;

    request = ext->Request;
    ext->Accepted = 0;
    *answer = ext;

    if ((request & ANXD_S2F_RX_DIRECT) != 0 &&
        ext->RxDirect != NULL && ext->RxFilled != NULL)
    {
        op->op_RxDirect = (APTR)ext->RxDirect;
        op->op_RxFilled = (APTR)ext->RxFilled;
        op->op_RxLinkHdr =
            (UBYTE)((request & ANXD_S2F_RX_LINK_HDR) != 0);
    }
    if ((request & ANXD_S2F_RX_VERIFIED) != 0)
        op->op_RxFlags = ANXD_S2_RXF_VERIFIED;
    if (ext->TxFlags != NULL)
    {
        op->op_TxFlags = (APTR)ext->TxFlags;
        if ((request & ANXD_S2F_TX_CSUM_TCP) != 0)
            op->op_TxCsum |= ANXD_S2_TXF_TCP;
        if ((request & ANXD_S2F_TX_CSUM_UDP) != 0)
            op->op_TxCsum |= ANXD_S2_TXF_UDP;
    }

    return TRUE;
}
