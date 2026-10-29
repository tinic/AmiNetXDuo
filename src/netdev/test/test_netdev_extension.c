/* Host contract for the versioned private OpenDevice negotiation. */

#include <stdio.h>
#include <string.h>

#include "netdev_internal.h"

static int failures;

#define CHECK(expr, what) do { \
    if (!(expr)) { printf("FAIL: %s\n", what); failures++; } \
} while (0)

static UBYTE *rx_direct(APTR data, ULONG len)
{
    (void)len;
    return (UBYTE *)data;
}

static VOID rx_filled(APTR data, ULONG len, ULONG sum, UBYTE flags)
{
    (void)data; (void)len; (void)sum; (void)flags;
}

static UBYTE tx_flags(APTR data)
{
    return data != NULL ? ANXD_S2_TXF_TCP : 0;
}

static VOID tx_flush(NetdevNic *nic)
{
    (void)nic;
}

static void test_valid_record(void)
{
    NetdevOpener op;
    AnxdS2Extension ext;
    AnxdS2Extension *answer = NULL;

    memset(&op, 0, sizeof(op));
    memset(&ext, 0, sizeof(ext));
    ext.Version = ANXD_S2_ABI_VERSION;
    ext.Size = (UWORD)sizeof(ext);
    ext.Request = ANXD_S2F_RX_DIRECT | ANXD_S2F_RX_LINK_HDR |
                  ANXD_S2F_RX_VERIFIED | ANXD_S2F_TX_CSUM_TCP |
                  ANXD_S2F_TX_CSUM_UDP;
    ext.Accepted = 0xffffffffUL;
    ext.RxDirect = rx_direct;
    ext.RxFilled = rx_filled;
    ext.TxFlags = tx_flags;

    CHECK(netdev_take_extension(&ext, &op, &answer),
          "valid record is recognized");
    CHECK(answer == &ext && ext.Accepted == 0,
          "valid record is returned with a clean answer");
    CHECK(op.op_RxDirect == (APTR)rx_direct &&
          op.op_RxFilled == (APTR)rx_filled && op.op_RxLinkHdr,
          "receive callbacks and link-header request are accepted");
    CHECK(op.op_TxFlags == (APTR)tx_flags &&
          op.op_TxCsum == (ANXD_S2_TXF_TCP | ANXD_S2_TXF_UDP),
          "transmit metadata callback owns both requested checksum bits");
}

static void test_version_and_size_gate(void)
{
    NetdevOpener op;
    AnxdS2Extension ext;
    AnxdS2Extension *answer;

    memset(&ext, 0, sizeof(ext));
    ext.Version = ANXD_S2_ABI_VERSION + 1;
    ext.Size = (UWORD)sizeof(ext);
    ext.Accepted = 0x12345678UL;
    memset(&op, 0, sizeof(op));
    answer = NULL;
    CHECK(!netdev_take_extension(&ext, &op, &answer) &&
          answer == NULL && ext.Accepted == 0x12345678UL,
          "unknown version is untouched and ignored");

    ext.Version = ANXD_S2_ABI_VERSION;
    ext.Size = (UWORD)(sizeof(ext) - 1);
    memset(&op, 0, sizeof(op));
    answer = NULL;
    CHECK(!netdev_take_extension(&ext, &op, &answer) &&
          answer == NULL && ext.Accepted == 0x12345678UL,
          "short record is untouched and ignored");

    ext.Size = (UWORD)(sizeof(ext) + 8u);
    memset(&op, 0, sizeof(op));
    answer = NULL;
    CHECK(netdev_take_extension(&ext, &op, &answer) && answer == &ext,
          "a larger compatible record is accepted by its known prefix");

    /* ABI 2 never shipped: a v2 record is ignored like any unknown version,
       and the opener falls back to plain SANA-II. */
    ext.Version = 2;
    ext.Size = (UWORD)sizeof(ext);
    ext.Request = ANXD_S2F_ALL;
    ext.RxDirect = rx_direct;
    ext.RxFilled = rx_filled;
    ext.TxFlags = tx_flags;
    memset(&op, 0, sizeof(op));
    answer = NULL;
    CHECK(!netdev_take_extension(&ext, &op, &answer) && answer == NULL &&
          op.op_RxDirect == NULL && op.op_TxFlags == NULL,
          "an ABI 2 record is ignored like an unknown version");
}

static void test_receive_facts_need_the_receive_callbacks(void)
{
    NetdevOpener op;
    AnxdS2Extension ext;
    AnxdS2Extension *answer = NULL;

    memset(&op, 0, sizeof(op));
    memset(&ext, 0, sizeof(ext));
    ext.Version = ANXD_S2_ABI_VERSION;
    ext.Size = (UWORD)sizeof(ext);
    ext.Request = ANXD_S2F_RX_LINK_HDR | ANXD_S2F_RX_VERIFIED;

    CHECK(netdev_take_extension(&ext, &op, &answer),
          "a record without the receive callbacks is otherwise valid");
    CHECK(answer == &ext && op.op_RxDirect == NULL &&
          op.op_RxFilled == NULL && !op.op_RxLinkHdr && op.op_RxFlags == 0,
          "receive-only facts are inert without the callback pair that reports them");
}

static void test_tx_needs_owned_metadata(void)
{
    NetdevOpener op;
    AnxdS2Extension ext;
    AnxdS2Extension *answer = NULL;

    memset(&op, 0, sizeof(op));
    memset(&ext, 0, sizeof(ext));
    ext.Version = ANXD_S2_ABI_VERSION;
    ext.Size = (UWORD)sizeof(ext);
    ext.Request = ANXD_S2F_TX_CSUM_TCP;
    CHECK(netdev_take_extension(&ext, &op, &answer),
          "record without TX callback is otherwise valid");
    CHECK(answer == &ext && op.op_TxFlags == NULL && op.op_TxCsum == 0,
          "checksum feature is refused without the owned callback");
}

/* S2_CopyToBuff: present, never called. */
static BOOL copy_to(APTR to, APTR from, ULONG len)
{
    (void)to; (void)from; (void)len;
    return TRUE;
}

static void test_supported_features_follow_opener_and_unit(void)
{
    NetdevOpener op;
    NetdevNic nic;
    ULONG supported;

    memset(&op, 0, sizeof(op));
    memset(&nic, 0, sizeof(nic));
    op.op_RxDirect = (APTR)rx_direct;
    op.op_RxFilled = (APTR)rx_filled;
    op.op_RxLinkHdr = TRUE;
    op.op_CopyTo = (APTR)copy_to;
    op.op_RxFlags = ANXD_S2_RXF_VERIFIED;
    op.op_TxFlags = (APTR)tx_flags;
    op.op_TxCsum = ANXD_S2_TXF_TCP | ANXD_S2_TXF_UDP;
    nic.rx_flags_supported = ANXD_S2_RXF_VERIFIED;
    nic.tx_csum_supported = ANXD_S2_TXF_TCP;
    nic.rx_batches = 1;
    nic.rx_holds = 1;
    nic.rx_capacity = 8192;
    nic.tx_flush = tx_flush;

    supported = netdev_extension_supported(&op, &nic);
    CHECK((supported & (ANXD_S2F_RX_DIRECT | ANXD_S2F_RX_LINK_HDR |
                        ANXD_S2F_RX_VERIFIED | ANXD_S2F_TX_CSUM_TCP |
                        ANXD_S2F_RX_BATCH | ANXD_S2F_RX_POLL |
                        ANXD_S2F_RX_CAPACITY | ANXD_S2F_TX_MORE)) ==
          (ANXD_S2F_RX_DIRECT | ANXD_S2F_RX_LINK_HDR |
           ANXD_S2F_RX_VERIFIED | ANXD_S2F_TX_CSUM_TCP |
           ANXD_S2F_RX_BATCH | ANXD_S2F_RX_POLL |
           ANXD_S2F_RX_CAPACITY | ANXD_S2F_TX_MORE),
          "the usable opener/unit feature set is reported");
    CHECK((supported & ANXD_S2F_TX_CSUM_UDP) == 0 &&
          op.op_TxCsum == ANXD_S2_TXF_TCP,
          "unsupported per-unit checksum facts are removed");

    /* F-310: netdev_queue_batch() refuses an opener without CopyTo, so the
       batch is never accepted for one. */
    op.op_CopyTo = NULL;
    supported = netdev_extension_supported(&op, &nic);
    CHECK((supported & ANXD_S2F_RX_BATCH) == 0,
          "an opener without CopyTo never advertises the receive batch");
    CHECK((supported & ANXD_S2F_RX_DIRECT) != 0,
          "while its direct receive is still reported");
    op.op_CopyTo = (APTR)copy_to;

    op.op_Raw = 1;
    supported = netdev_extension_supported(&op, &nic);
    CHECK((supported & ANXD_S2F_RX_BATCH) == 0,
          "a raw opener never advertises the cooked receive batch");
}

/* ANX-006: one negotiation per open.  The first valid ANX record is taken;
   a later one is ignored and its Accepted left as the opener set it, so no
   callbacks can be active behind an Accepted the opener never sees. */
static void test_duplicate_records_first_valid_wins(void)
{
    NetdevOpener     op;
    AnxdS2Extension  first, second;
    AnxdS2Extension *answer = NULL;

    memset(&op, 0, sizeof(op));
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    first.Version  = second.Version = ANXD_S2_ABI_VERSION;
    first.Size     = second.Size    = (UWORD)sizeof(first);
    first.Request  = ANXD_S2F_RX_DIRECT | ANXD_S2F_RX_LINK_HDR;
    first.RxDirect = rx_direct;
    first.RxFilled = rx_filled;
    second.Accepted = 0x5a5a5a5aUL;     /* must stay untouched */

    CHECK(netdev_take_extension_once(&first, &op, &answer) && answer == &first,
          "two ANX records: the first is the negotiation");
    CHECK(!netdev_take_extension_once(&second, &op, &answer) &&
          answer == &first && second.Accepted == 0x5a5a5a5aUL,
          "two ANX records: the second is ignored and not touched");
    CHECK(op.op_RxDirect == (APTR)rx_direct && op.op_RxLinkHdr,
          "two ANX records: the first record's callbacks stay the ones taken");

    /* An invalid first record does not block a valid later one. */
    memset(&op, 0, sizeof(op));
    first.Version = 2;
    second.Request = ANXD_S2F_RX_DIRECT;
    second.RxDirect = rx_direct;
    second.RxFilled = rx_filled;
    answer = NULL;
    CHECK(!netdev_take_extension_once(&first, &op, &answer) && answer == NULL,
          "an invalid first ANX record is not taken");
    CHECK(netdev_take_extension_once(&second, &op, &answer) && answer == &second,
          "a valid later ANX record is taken after an invalid one");
}

/* TX_BATCH's pair, never called here. */
static ULONG tx_batch(APTR data, AnxdS2TxBatchInfo *info)
{
    (void)data; (void)info;
    return 0;
}

static ULONG tx_child(APTR data, ULONG index, UBYTE *dst, ULONG len)
{
    (void)data; (void)index; (void)dst; (void)len;
    return 0;
}

static VOID tx_posted(APTR data, ULONG count)
{
    (void)data; (void)count;
}

static UWORD tx_run(NetdevNic *nic, UWORD first, UWORD count,
                    UWORD (*fill)(APTR arg, UWORD index, UBYTE *buf),
                    APTR arg, UBYTE *failed)
{
    (void)nic; (void)first; (void)count; (void)fill; (void)arg;
    *failed = 0;
    return 0;
}

/*
 * TX_BATCH is read only from a record whose Size covers the pair, for its
 * own bit: a record of the experimental shape (bit 10, Size 32) or a short
 * one never hands over a callback, and the unit offers the bit only with a
 * core that cuts batches under the task-level transmit lock.
 */
static void test_tx_batch_needs_the_appended_pair(void)
{
    NetdevOpener      op;
    AnxdS2ExtensionTx ext;
    AnxdS2Extension  *answer;
    NetdevNic         nic;

    memset(&ext, 0, sizeof(ext));
    ext.Base.Version = ANXD_S2_ABI_VERSION;
    ext.Base.Size    = (UWORD)ANXD_S2_EXTENSION_BATCH_SIZE;
    ext.Base.Request = ANXD_S2F_TX_BATCH;
    ext.Base.TxFlags = tx_flags;
    ext.TxBatch      = tx_batch;
    ext.TxChild      = tx_child;
    ext.TxPosted     = tx_posted;
    memset(&op, 0, sizeof(op));
    answer = NULL;
    CHECK(netdev_take_extension(&ext.Base, &op, &answer) &&
          op.op_TxBatch == (APTR)tx_batch && op.op_TxChild == (APTR)tx_child &&
          op.op_TxPosted == (APTR)tx_posted,
          "a record covering the three hands them over for TX_BATCH");

    ext.Base.Size = (UWORD)(ANXD_S2_EXTENSION_BATCH_SIZE - 1);
    memset(&op, 0, sizeof(op));
    answer = NULL;
    CHECK(netdev_take_extension(&ext.Base, &op, &answer) &&
          op.op_TxBatch == NULL && op.op_TxChild == NULL,
          "a record one byte short of them hands over nothing of it");

    ext.Base.Size = (UWORD)(offsetof(AnxdS2ExtensionTx, TxBatch));
    memset(&op, 0, sizeof(op));
    answer = NULL;
    CHECK(netdev_take_extension(&ext.Base, &op, &answer) &&
          op.op_TxBatch == NULL,
          "the experimental record's size hands over nothing of it");

    ext.Base.Size    = (UWORD)ANXD_S2_EXTENSION_BATCH_SIZE;
    ext.Base.Request = 1UL << 10;
    memset(&op, 0, sizeof(op));
    answer = NULL;
    CHECK(netdev_take_extension(&ext.Base, &op, &answer) &&
          op.op_TxBatch == NULL,
          "bit 10 alone is not TX_BATCH");

    ext.Base.Request = ANXD_S2F_TX_BATCH;
    ext.TxChild      = NULL;
    memset(&op, 0, sizeof(op));
    answer = NULL;
    CHECK(netdev_take_extension(&ext.Base, &op, &answer) &&
          op.op_TxBatch == NULL,
          "two of the three are none");

    /* What the unit offers. */
    ext.TxChild = tx_child;
    memset(&op, 0, sizeof(op));
    answer = NULL;
    (void)netdev_take_extension(&ext.Base, &op, &answer);
    op.op_CopyFrom = (APTR)tx_batch;    /* present, never called */
    memset(&nic, 0, sizeof(nic));
    CHECK((netdev_extension_supported(&op, &nic) & ANXD_S2F_TX_BATCH) == 0,
          "a core without tx_run offers no batches");
    nic.tx_run = tx_run;
    CHECK((netdev_extension_supported(&op, &nic) & ANXD_S2F_TX_BATCH) == 0,
          "nor one without the task-level transmit lock");
    nic.tx_task_lock = 1;
    CHECK((netdev_extension_supported(&op, &nic) & ANXD_S2F_TX_BATCH) != 0,
          "a core with both does");
    op.op_Raw = 1;
    CHECK((netdev_extension_supported(&op, &nic) & ANXD_S2F_TX_BATCH) == 0,
          "but not to a raw opener");
}

int main(void)
{
    test_duplicate_records_first_valid_wins();
    test_valid_record();
    test_version_and_size_gate();
    test_receive_facts_need_the_receive_callbacks();
    test_tx_needs_owned_metadata();
    test_supported_features_follow_opener_and_unit();
    test_tx_batch_needs_the_appended_pair();

    if (failures != 0)
        printf("netdev extension: %d failure(s)\n", failures);
    return failures != 0;
}
