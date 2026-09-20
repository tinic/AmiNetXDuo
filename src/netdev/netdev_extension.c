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
        if ((request & ANXD_S2F_RX_VERIFIED) != 0)
            op->op_RxFlags = ANXD_S2_RXF_VERIFIED;
    }
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

/* Resolve request-independent opener prerequisites against one selected
   unit.  Open() intersects this result with Request; keeping the dependency
   rules here makes them testable without the device's resident entry shell. */
ULONG netdev_extension_supported(NetdevOpener *op, const NetdevNic *nic)
{
    ULONG supported = ANXD_S2F_TX_QUICK;

    if (op == NULL || nic == NULL)
        return 0;

    op->op_RxFlags &= nic->rx_flags_supported;
    op->op_TxCsum &= nic->tx_csum_supported;

    if (op->op_RxDirect != NULL && op->op_RxFilled != NULL)
        supported |= ANXD_S2F_RX_DIRECT;
    if (op->op_RxLinkHdr)
        supported |= ANXD_S2F_RX_LINK_HDR;
    if (nic->rx_batches && op->op_RxDirect != NULL &&
        op->op_RxFilled != NULL && op->op_RxLinkHdr && !op->op_Raw &&
        op->op_Filter == NULL)
        supported |= ANXD_S2F_RX_BATCH;
    if (nic->tx_flush != NULL && op->op_TxFlags != NULL)
        supported |= ANXD_S2F_TX_MORE;
    if ((op->op_RxFlags & ANXD_S2_RXF_VERIFIED) != 0)
        supported |= ANXD_S2F_RX_VERIFIED;
    if ((op->op_TxCsum & ANXD_S2_TXF_TCP) != 0)
        supported |= ANXD_S2F_TX_CSUM_TCP;
    if ((op->op_TxCsum & ANXD_S2_TXF_UDP) != 0)
        supported |= ANXD_S2F_TX_CSUM_UDP;
    if (nic->rx_holds)
        supported |= ANXD_S2F_RX_POLL;
    if (nic->rx_capacity != 0)
        supported |= ANXD_S2F_RX_CAPACITY;

    return supported;
}
