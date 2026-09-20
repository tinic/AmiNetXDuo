/*
 * anxnet.device: the Broadcom GENET v5 register map, for the Raspberry Pi 4
 * and CM4 behind a PiStorm32-lite.
 *
 * Offsets and bit positions are from NetBSD's sys/dev/ic/bcmgenetreg.h:
 *
 *   Copyright (c) 2020 Jared McNeill <jmcneill@invisible.ca>
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 *   IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 *   OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *   IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 *   INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 *   NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 *   THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Every register is 32 bits wide and little-endian; genet.c swaps.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef AMINETXDUO_GENETREG_H
#define AMINETXDUO_GENETREG_H

#define GENET_SYS_REV_CTRL              0x000
#define  GENET_SYS_REV_MAJOR(v)         (((v) >> 24) & 0x0fUL)
#define  GENET_SYS_REV_MINOR(v)         (((v) >> 16) & 0x0fUL)
#define GENET_SYS_PORT_CTRL             0x004
#define  GENET_SYS_PORT_MODE_EXT_GPHY   3UL
#define GENET_SYS_RBUF_FLUSH_CTRL       0x008
#define  GENET_SYS_RBUF_FLUSH_RESET     (1UL << 1)
#define GENET_SYS_TBUF_FLUSH_CTRL       0x00c
#define GENET_EXT_RGMII_OOB_CTRL        0x08c
#define  GENET_EXT_RGMII_OOB_ID_MODE_DISABLE    (1UL << 16)
#define  GENET_EXT_RGMII_OOB_RGMII_MODE_EN      (1UL << 6)
#define  GENET_EXT_RGMII_OOB_OOB_DISABLE        (1UL << 5)
#define  GENET_EXT_RGMII_OOB_RGMII_LINK         (1UL << 4)
#define GENET_INTRL2_CPU_STAT           0x200
#define GENET_INTRL2_CPU_CLEAR          0x208
#define GENET_INTRL2_CPU_STAT_MASK      0x20c
#define GENET_INTRL2_CPU_SET_MASK       0x210
#define GENET_INTRL2_CPU_CLEAR_MASK     0x214
#define  GENET_IRQ_MDIO_ERROR           (1UL << 24)
#define  GENET_IRQ_MDIO_DONE            (1UL << 23)
#define  GENET_IRQ_TXDMA_DONE           (1UL << 16)
#define  GENET_IRQ_RXDMA_DONE           (1UL << 13)
#define GENET_RBUF_CTRL                 0x300
/* Frames the RBUF dropped for want of a descriptor or FIFO room: the
   hardware's running count (GENET v3+ offset 0x80 in the RBUF block; the
   bcmgenet driver's rbuf_ovflow_cnt).  Read on the tick into a core stat,
   because a ring that fills between two passes drops silently otherwise. */
#define GENET_RBUF_OVFL_CNT             0x380
#define  GENET_RBUF_BAD_DIS             (1UL << 2)
#define  GENET_RBUF_ALIGN_2B            (1UL << 1)
#define  GENET_RBUF_64B_EN              (1UL << 0)
/* The receive checksum block: with RXCHK_EN the RBUF sums each frame from
   the end of its Ethernet header (L3_PARSE_DIS: whatever the type) to its
   end, and writes it into the 64-byte status block RBUF_64B_EN puts in
   front of the frame in the buffer.  SKIP_FCS leaves the FCS out when the
   MAC forwards one; this driver never sets CRC_FWD, so it is not set. */
#define GENET_RBUF_CHK_CTRL             0x314
#define  GENET_RBUF_RXCHK_EN            (1UL << 0)
#define  GENET_RBUF_SKIP_FCS            (1UL << 4)
#define  GENET_RBUF_L3_PARSE_DIS        (1UL << 5)
#define GENET_RBUF_TBUF_SIZE_CTRL       0x3b4

/* The 64-byte receive status block (RBUF_64B_EN), little-endian words as the
   DMA writes them: length_status at 0 is the descriptor's status word,
   rx_csum at 8 carries the checksum in its low half. */
#define GENET_RX_STATUS64_LEN           64
#define GENET_RX_STATUS64_LENGTH_STATUS 0
#define GENET_RX_STATUS64_CSUM          8
/* The transmit side of the same: TBUF_CTRL's 64B_EN (the RBUF bit's
   position) puts a 64-byte status block in front of every frame the DMA
   reads, and tx_csum_info in it, a little-endian word at 48, asks the TBUF
   to finish a transport checksum: bits 16-30 the offset of the transport
   header from the start of the frame, bits 0-14 the offset of the checksum
   field, bit 15 for UDP, bit 31 (length valid) to take the request.  The
   field must already hold the pseudo-header sum; the block sums from the
   header offset to the end of the frame on top of it and writes the
   complement into the field.  FreeBSD if_genet.c (Karels, McNeill,
   BSD-2-Clause) is the reference for the layout. */
#define GENET_TBUF_CTRL                 0x600
#define GENET_TX_STATUS64_LEN           64
#define GENET_TX_STATUS64_CSUM_INFO     48
#define  GENET_TX_CSUM_LEN_VALID        (1UL << 31)
#define  GENET_TX_CSUM_START_SHIFT      16
#define  GENET_TX_CSUM_UDP              (1UL << 15)
#define GENET_UMAC_CMD                  0x808
#define  GENET_UMAC_CMD_LCL_LOOP_EN     (1UL << 15)
#define  GENET_UMAC_CMD_SW_RESET        (1UL << 13)
#define  GENET_UMAC_CMD_PROMISC         (1UL << 4)
#define  GENET_UMAC_CMD_SPEED_MASK      (3UL << 2)
#define  GENET_UMAC_CMD_SPEED_10        (0UL << 2)
#define  GENET_UMAC_CMD_SPEED_100       (1UL << 2)
#define  GENET_UMAC_CMD_SPEED_1000      (2UL << 2)
#define  GENET_UMAC_CMD_RXEN            (1UL << 1)
#define  GENET_UMAC_CMD_TXEN            (1UL << 0)
#define GENET_UMAC_MAC0                 0x80c
#define GENET_UMAC_MAC1                 0x810
#define GENET_UMAC_MAX_FRAME_LEN        0x814
#define GENET_UMAC_TX_FLUSH             0xb34
#define GENET_UMAC_MIB_CTRL             0xd80
#define  GENET_UMAC_MIB_RESET_TX        (1UL << 2)
#define  GENET_UMAC_MIB_RESET_RUNT      (1UL << 1)
#define  GENET_UMAC_MIB_RESET_RX        (1UL << 0)
#define GENET_MDIO_CMD                  0xe14
#define  GENET_MDIO_START_BUSY          (1UL << 29)
#define  GENET_MDIO_READ                (1UL << 27)
#define  GENET_MDIO_WRITE               (1UL << 26)
#define  GENET_MDIO_PMD(phy)            (((ULONG)(phy) & 0x1fUL) << 21)
#define  GENET_MDIO_REG(reg)            (((ULONG)(reg) & 0x1fUL) << 16)
#define GENET_UMAC_MDF_CTRL             0xe50
#define GENET_UMAC_MDF_ADDR0(n)         (0xe54 + (n) * 0x8)
#define GENET_UMAC_MDF_ADDR1(n)         (0xe58 + (n) * 0x8)
#define GENET_MAX_MDF_FILTER            17

#define GENET_DMA_DESC_COUNT            256
#define GENET_DMA_DESC_SIZE             12
#define GENET_DMA_DEFAULT_QUEUE         16
#define GENET_DMA_RING_SIZE             0x40

#define GENET_RX_BASE                   0x2000
#define GENET_TX_BASE                   0x4000

#define GENET_RX_DMA_RINGBASE(q)        (GENET_RX_BASE + 0xc00 + GENET_DMA_RING_SIZE * (q))
#define GENET_RX_DMA_WRITE_PTR_LO(q)    (GENET_RX_DMA_RINGBASE(q) + 0x00)
#define GENET_RX_DMA_WRITE_PTR_HI(q)    (GENET_RX_DMA_RINGBASE(q) + 0x04)
#define GENET_RX_DMA_PROD_INDEX(q)      (GENET_RX_DMA_RINGBASE(q) + 0x08)
#define GENET_RX_DMA_CONS_INDEX(q)      (GENET_RX_DMA_RINGBASE(q) + 0x0c)
#define GENET_RX_DMA_RING_BUF_SIZE(q)   (GENET_RX_DMA_RINGBASE(q) + 0x10)
#define GENET_RX_DMA_START_ADDR_LO(q)   (GENET_RX_DMA_RINGBASE(q) + 0x14)
#define GENET_RX_DMA_START_ADDR_HI(q)   (GENET_RX_DMA_RINGBASE(q) + 0x18)
#define GENET_RX_DMA_END_ADDR_LO(q)     (GENET_RX_DMA_RINGBASE(q) + 0x1c)
#define GENET_RX_DMA_END_ADDR_HI(q)     (GENET_RX_DMA_RINGBASE(q) + 0x20)
#define GENET_RX_DMA_MBUF_DONE_THRES(q) (GENET_RX_DMA_RINGBASE(q) + 0x24)
#define GENET_RX_DMA_XON_XOFF_THRES(q)  (GENET_RX_DMA_RINGBASE(q) + 0x28)
#define GENET_RX_DMA_READ_PTR_LO(q)     (GENET_RX_DMA_RINGBASE(q) + 0x2c)
#define GENET_RX_DMA_READ_PTR_HI(q)     (GENET_RX_DMA_RINGBASE(q) + 0x30)

#define GENET_TX_DMA_RINGBASE(q)        (GENET_TX_BASE + 0xc00 + GENET_DMA_RING_SIZE * (q))
#define GENET_TX_DMA_READ_PTR_LO(q)     (GENET_TX_DMA_RINGBASE(q) + 0x00)
#define GENET_TX_DMA_READ_PTR_HI(q)     (GENET_TX_DMA_RINGBASE(q) + 0x04)
#define GENET_TX_DMA_CONS_INDEX(q)      (GENET_TX_DMA_RINGBASE(q) + 0x08)
#define GENET_TX_DMA_PROD_INDEX(q)      (GENET_TX_DMA_RINGBASE(q) + 0x0c)
#define GENET_TX_DMA_RING_BUF_SIZE(q)   (GENET_TX_DMA_RINGBASE(q) + 0x10)
#define GENET_TX_DMA_START_ADDR_LO(q)   (GENET_TX_DMA_RINGBASE(q) + 0x14)
#define GENET_TX_DMA_START_ADDR_HI(q)   (GENET_TX_DMA_RINGBASE(q) + 0x18)
#define GENET_TX_DMA_END_ADDR_LO(q)     (GENET_TX_DMA_RINGBASE(q) + 0x1c)
#define GENET_TX_DMA_END_ADDR_HI(q)     (GENET_TX_DMA_RINGBASE(q) + 0x20)
#define GENET_TX_DMA_MBUF_DONE_THRES(q) (GENET_TX_DMA_RINGBASE(q) + 0x24)
#define GENET_TX_DMA_FLOW_PERIOD(q)     (GENET_TX_DMA_RINGBASE(q) + 0x28)
#define GENET_TX_DMA_WRITE_PTR_LO(q)    (GENET_TX_DMA_RINGBASE(q) + 0x2c)
#define GENET_TX_DMA_WRITE_PTR_HI(q)    (GENET_TX_DMA_RINGBASE(q) + 0x30)

#define GENET_RX_DESC_STATUS(i)         (GENET_RX_BASE + GENET_DMA_DESC_SIZE * (i) + 0x00)
#define  GENET_RX_DESC_STATUS_BUFLEN(s) (((s) >> 16) & 0x0fffUL)
#define  GENET_RX_DESC_STATUS_OWN       (1UL << 15)
#define  GENET_RX_DESC_STATUS_EOP       (1UL << 14)
#define  GENET_RX_DESC_STATUS_SOP       (1UL << 13)
#define  GENET_RX_DESC_STATUS_ALL_ERRS  0x1fUL
#define  GENET_RX_DESC_STATUS_LEN_ERR   (1UL << 4)
#define  GENET_RX_DESC_STATUS_FRAME_ERR (1UL << 3)
#define  GENET_RX_DESC_STATUS_RX_ERR    (1UL << 2)
#define  GENET_RX_DESC_STATUS_CRC_ERR   (1UL << 1)
#define  GENET_RX_DESC_STATUS_OVRUN_ERR (1UL << 0)
#define GENET_RX_DESC_ADDRESS_LO(i)     (GENET_RX_BASE + GENET_DMA_DESC_SIZE * (i) + 0x04)
#define GENET_RX_DESC_ADDRESS_HI(i)     (GENET_RX_BASE + GENET_DMA_DESC_SIZE * (i) + 0x08)

#define GENET_TX_DESC_STATUS(i)         (GENET_TX_BASE + GENET_DMA_DESC_SIZE * (i) + 0x00)
#define  GENET_TX_DESC_STATUS_BUFLEN(n) (((ULONG)(n) & 0x0fffUL) << 16)
#define  GENET_TX_DESC_STATUS_OWN       (1UL << 15)
#define  GENET_TX_DESC_STATUS_EOP       (1UL << 14)
#define  GENET_TX_DESC_STATUS_SOP       (1UL << 13)
#define  GENET_TX_DESC_STATUS_QTAG      (0x3fUL << 7)
#define  GENET_TX_DESC_STATUS_CRC       (1UL << 6)
#define  GENET_TX_DESC_STATUS_CKSUM     (1UL << 4)
#define GENET_TX_DESC_ADDRESS_LO(i)     (GENET_TX_BASE + GENET_DMA_DESC_SIZE * (i) + 0x04)
#define GENET_TX_DESC_ADDRESS_HI(i)     (GENET_TX_BASE + GENET_DMA_DESC_SIZE * (i) + 0x08)

#define GENET_RX_DMA_RING_CFG           (GENET_RX_BASE + 0x1040 + 0x00)
#define GENET_RX_DMA_CTRL               (GENET_RX_BASE + 0x1040 + 0x04)
#define  GENET_RX_DMA_CTRL_RBUF_EN(q)   (1UL << ((q) + 1))
#define  GENET_RX_DMA_CTRL_EN           (1UL << 0)
#define GENET_RX_SCB_BURST_SIZE         (GENET_RX_BASE + 0x1040 + 0x0c)
#define GENET_RX_DMA_RING_TIMEOUT(q)    (GENET_RX_BASE + 0x1040 + 0x2c + 4 * (q))

#define GENET_TX_DMA_RING_CFG           (GENET_TX_BASE + 0x1040 + 0x00)
#define GENET_TX_DMA_CTRL               (GENET_TX_BASE + 0x1040 + 0x04)
#define  GENET_TX_DMA_CTRL_RBUF_EN(q)   (1UL << ((q) + 1))
#define  GENET_TX_DMA_CTRL_EN           (1UL << 0)
#define GENET_TX_SCB_BURST_SIZE         (GENET_TX_BASE + 0x1040 + 0x0c)
#define GENET_TX_DMA_RING_TIMEOUT(q)    (GENET_TX_BASE + 0x1040 + 0x2c + 4 * (q))

#define GENET_INTR_THRESHOLD_MASK       0x1ffUL
#define GENET_DMA_RING_TIMEOUT_MASK     0xffffUL

/* MII, the registers the PHY layer in genet.c reads; IEEE 802.3 clause 22. */
#define MII_BMCR                        0x00
#define  BMCR_RESET                     0x8000
#define  BMCR_AUTOEN                    0x1000
#define  BMCR_STARTNEG                  0x0200
#define MII_BMSR                        0x01
#define  BMSR_LINK                      0x0004
#define  BMSR_ACOMP                     0x0020
#define MII_PHYIDR1                     0x02
#define MII_PHYIDR2                     0x03
/* Broadcom auxiliary status: the negotiated speed and duplex, HCD bits. */
#define BRGPHY_MII_AUXSTS               0x19
#define  BRGPHY_AUXSTS_AN_RES           0x0700
#define  BRGPHY_RES_1000FD              0x0700
#define  BRGPHY_RES_1000HD              0x0600
#define  BRGPHY_RES_100FD               0x0500
#define  BRGPHY_RES_100T4               0x0400
#define  BRGPHY_RES_100HD               0x0300
#define  BRGPHY_RES_10FD                0x0200
#define  BRGPHY_RES_10HD                0x0100

#endif /* AMINETXDUO_GENETREG_H */
