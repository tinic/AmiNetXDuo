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

    op.op_Raw = 1;
    supported = netdev_extension_supported(&op, &nic);
    CHECK((supported & ANXD_S2F_RX_BATCH) == 0,
          "a raw opener never advertises the cooked receive batch");
}

int main(void)
{
    test_valid_record();
    test_version_and_size_gate();
    test_receive_facts_need_the_receive_callbacks();
    test_tx_needs_owned_metadata();
    test_supported_features_follow_opener_and_unit();

    if (failures != 0)
        printf("netdev extension: %d failure(s)\n", failures);
    return failures != 0;
}
