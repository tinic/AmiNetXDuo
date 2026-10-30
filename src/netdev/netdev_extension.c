/* Version and feature validation for the private SANA-II extension. */

#include "netdev_internal.h"

/* The frozen m68k layout anxs2ext.h documents (a host build's pointers are
   wider, so only the target is held to it). */
#if defined(__mc68000__) || defined(mc68000)
typedef char anxd_ext_size_is_frozen[
    (ANXD_S2_EXTENSION_SIZE == 24u) ? 1 : -1];
typedef char anxd_ext_txflags_at_20[
    (offsetof(AnxdS2Extension, TxFlags) == 20u) ? 1 : -1];
typedef char anxd_batch_header_is_8[
    (offsetof(AnxdS2RxBatch, Cookie) == 8u) ? 1 : -1];
#ifdef AMINETXDUO_TX_BATCH
typedef char anxd_ext_tx_batch_at_32[
    (offsetof(AnxdS2ExtensionTx, TxBatch) == 32u) ? 1 : -1];
typedef char anxd_ext_tx_batch_size_is_44[
    (ANXD_S2_EXTENSION_BATCH_SIZE == 44u) ? 1 : -1];
#endif /* AMINETXDUO_TX_BATCH */
#endif

BOOL netdev_take_extension(AnxdS2Extension *ext, NetdevOpener *op,
                           AnxdS2Extension **answer)
{
    ULONG request;

    if (ext == NULL || op == NULL || answer == NULL ||
        ext->Version < ANXD_S2_ABI_VERSION_MIN ||
        ext->Version > ANXD_S2_ABI_VERSION ||
        ext->Size < (UWORD)ANXD_S2_EXTENSION_SIZE)
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
#ifdef AMINETXDUO_TX_BATCH
    }
    /* Appended to version 3 (AnxdS2ExtensionTx): read only from a record
       whose Size covers them, and only for the bit that names them -- bit
       10 and the 24..31 pointers belonged to an experimental record, and a
       record of that shape never gets these. */
    if ((request & ANXD_S2F_TX_BATCH) != 0 &&
        ext->Size >= (UWORD)ANXD_S2_EXTENSION_BATCH_SIZE)
    {
        const AnxdS2ExtensionTx *tx = (const AnxdS2ExtensionTx *)ext;

        if (tx->TxBatch != NULL && tx->TxChild != NULL &&
            tx->TxPosted != NULL)
        {
            op->op_TxBatch  = (APTR)tx->TxBatch;
            op->op_TxChild  = (APTR)tx->TxChild;
            op->op_TxPosted = (APTR)tx->TxPosted;
        }
#endif /* AMINETXDUO_TX_BATCH */
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

/* One negotiation per open (ANX-006): the first valid record in the taglist
   is taken; a later ANXD_S2_EXTENSION tag is ignored and its Accepted left
   as the opener set it.  Taking it too merged a second record's request with
   the first one's callbacks and reported Accepted 0 over active callbacks. */
BOOL netdev_take_extension_once(AnxdS2Extension *ext, NetdevOpener *op,
                                AnxdS2Extension **answer)
{
    if (answer == NULL || *answer != NULL)
        return FALSE;
    return netdev_take_extension(ext, op, answer);
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
    /* The same conditions netdev_queue_batch() refuses without, CopyTo
       included (F-310). */
    if (nic->rx_batches && op->op_RxDirect != NULL &&
        op->op_RxFilled != NULL && op->op_CopyTo != NULL &&
        op->op_RxLinkHdr && !op->op_Raw && op->op_Filter == NULL)
        supported |= ANXD_S2F_RX_BATCH;
    if (nic->tx_flush != NULL && op->op_TxFlags != NULL)
        supported |= ANXD_S2F_TX_MORE;
#ifdef AMINETXDUO_TX_BATCH
    /* Batches are cut by a core's tx_run under the task-level transmit lock
       (netdev_tx.c), cooked: the shell writes each frame's link header. */
    if (nic->tx_run != NULL && nic->tx_task_lock &&
        op->op_TxBatch != NULL && op->op_TxChild != NULL &&
        op->op_TxPosted != NULL && op->op_TxFlags != NULL && op->op_CopyFrom != NULL && !op->op_Raw)
        supported |= ANXD_S2F_TX_BATCH;
#endif /* AMINETXDUO_TX_BATCH */
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
