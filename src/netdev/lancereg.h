/*
 * Am7990/Am79C960 LANCE register and descriptor bits.
 *
 * From the Am7990 data sheet.  Only what lance.c uses is here: the chip has
 * four control registers on an Am7990 and 128 on an Am79C960, and everything
 * past CSR3 is Ariadne-only tuning this driver does not touch.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_LANCEREG_H
#define AMINETXDUO_LANCEREG_H

/* RAP values. */
#define LE_CSR0         0       /* status and control */
#define LE_CSR1         1       /* init block address, low 16 */
#define LE_CSR2         2       /* init block address, high 8 */
#define LE_CSR3         3       /* bus behaviour: BSWP, ACON, BCON */

/* CSR0.  Every bit here except INEA/STRT/STOP/INIT/TDMD is write-1-to-clear. */
#define LE_C0_ERR       0x8000
#define LE_C0_BABL      0x4000  /* transmitter timeout */
#define LE_C0_CERR      0x2000  /* no heartbeat after a send */
#define LE_C0_MISS      0x1000  /* a frame arrived with no free descriptor */
#define LE_C0_MERR      0x0800  /* the chip lost the bus */
#define LE_C0_RINT      0x0400
#define LE_C0_TINT      0x0200
#define LE_C0_IDON      0x0100  /* the init block has been read */
#define LE_C0_INTR      0x0080  /* any of the above, and what INT follows */
#define LE_C0_INEA      0x0040  /* interrupt enable */
#define LE_C0_RXON      0x0020
#define LE_C0_TXON      0x0010
#define LE_C0_TDMD      0x0008  /* send now, do not wait for the poll */
#define LE_C0_STOP      0x0004
#define LE_C0_STRT      0x0002
#define LE_C0_INIT      0x0001

/* Init block, MODE word. */
#define LE_MODE_PROM    0x8000  /* promiscuous */
#define LE_MODE_DRTY    0x0020  /* disable retry */
#define LE_MODE_DTCR    0x0008  /* no CRC on transmit */
#define LE_MODE_LOOP    0x0004

/* Receive descriptor, second word: status in the high byte. */
#define LE_R1_OWN       0x8000  /* the chip owns it */
#define LE_R1_ERR       0x4000
#define LE_R1_FRAM      0x2000  /* framing error */
#define LE_R1_OFLO      0x1000  /* the chip could not keep up */
#define LE_R1_CRC       0x0800
#define LE_R1_BUFF      0x0400  /* ran out of buffer */
#define LE_R1_STP       0x0200  /* start of packet */
#define LE_R1_ENP       0x0100  /* end of packet */

/* Transmit descriptor, second word. */
#define LE_T1_OWN       0x8000
#define LE_T1_ERR       0x4000
#define LE_T1_MORE      0x1000  /* more than one retry */
#define LE_T1_ONE       0x0800  /* exactly one retry */
#define LE_T1_DEF       0x0400  /* deferred */
#define LE_T1_STP       0x0200
#define LE_T1_ENP       0x0100

/* Transmit descriptor, fourth word.  BUFF and UFLO stop an Am7990's
   transmitter; the ring must be reinitialised before another frame can go. */
#define LE_T3_BUFF      0x8000
#define LE_T3_UFLO      0x4000
#define LE_T3_LCOL      0x1000
#define LE_T3_LCAR      0x0800
#define LE_T3_RTRY      0x0400
#define LE_T3_TDR_MASK  0x03ff

#endif /* AMINETXDUO_LANCEREG_H */
