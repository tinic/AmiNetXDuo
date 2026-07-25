/*
 * AmiNetXDuo -- SANA-II device interface definitions.
 *
 * The m68k-amigaos-gcc NDK that this project builds against ships no
 * <devices/sana2.h>, so the interface is restated here from the published
 * SANA-II Network Device Driver Specification (AmigaMail Vol. 2 / SANA-II
 * revision 2 autodocs). Command numbers, structure layouts and error codes are
 * wire-and-ABI facts of the protocol, not implementation.
 *
 * The include guard deliberately matches Commodore's, so that if a future NDK
 * does provide <devices/sana2.h> only one of the two definitions is ever seen.
 * The AmiNetXDuo-specific extras below sit outside that guard.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_SANA2_DEVICE_H
#define AMINETXDUO_SANA2_DEVICE_H

#include <exec/types.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <exec/errors.h>
#include <devices/timer.h>
#include <utility/tagitem.h>

#ifndef SANA2_SANA2DEVICE_H
#define SANA2_SANA2DEVICE_H 1

#define SANA2_MAX_ADDR_BITS     (128)
#define SANA2_MAX_ADDR_BYTES    ((SANA2_MAX_ADDR_BITS + 7) / 8)

struct IOSana2Req
{
    struct IORequest ios2_Req;
    ULONG   ios2_WireError;                         /* wire specific error   */
    ULONG   ios2_PacketType;                        /* packet type           */
    UBYTE   ios2_SrcAddr[SANA2_MAX_ADDR_BYTES];     /* source address        */
    UBYTE   ios2_DstAddr[SANA2_MAX_ADDR_BYTES];     /* destination address   */
    ULONG   ios2_DataLength;                        /* length of packet data */
    VOID   *ios2_Data;                              /* packet data           */
    VOID   *ios2_StatData;                          /* statistics data       */
    VOID   *ios2_BufferManagement;                  /* buffer mgmt tag list  */
};

/* io_Flags bits. */
#define SANA2IOB_RAW    (7)             /* raw packet IO requested           */
#define SANA2IOF_RAW    (1 << SANA2IOB_RAW)
#define SANA2IOB_BCAST  (6)             /* broadcast packet (received)       */
#define SANA2IOF_BCAST  (1 << SANA2IOB_BCAST)
#define SANA2IOB_MCAST  (5)             /* multicast packet (received)       */
#define SANA2IOF_MCAST  (1 << SANA2IOB_MCAST)
#define SANA2IOB_QUICK  (IOB_QUICK)
#define SANA2IOF_QUICK  (IOF_QUICK)

/* OpenDevice() flags. */
#define SANA2OPB_MINE   (0)             /* exclusive access requested        */
#define SANA2OPF_MINE   (1 << SANA2OPB_MINE)
#define SANA2OPB_PROM   (1)             /* promiscuous mode requested        */
#define SANA2OPF_PROM   (1 << SANA2OPB_PROM)

/* OpenDevice() buffer-management tags. */
#define S2_Dummy        (TAG_USER + 0xB0000)
#define S2_CopyToBuff   (S2_Dummy + 1)
#define S2_CopyFromBuff (S2_Dummy + 2)
#define S2_PacketFilter (S2_Dummy + 3)

struct Sana2DeviceQuery
{
    ULONG   SizeAvailable;      /* bytes available                           */
    ULONG   SizeSupplied;       /* bytes supplied                            */
    ULONG   DevQueryFormat;     /* this is type 0                            */
    ULONG   DeviceLevel;        /* this document is level 0                  */
    UWORD   AddrFieldSize;      /* address size in bits                      */
    ULONG   MTU;                /* maximum packet data size                  */
    ULONG   BPS;                /* line rate (bits/sec)                      */
    ULONG   HardwareType;       /* what the wire is                          */
};

/* Registered hardware types. */
#define S2WireType_Ethernet     1
#define S2WireType_IEEE802      6
#define S2WireType_Arcnet       7
#define S2WireType_LocalTalk    11
#define S2WireType_DyLAN        12
#define S2WireType_AmokNet      200
#define S2WireType_Liana        202
#define S2WireType_PPP          253
#define S2WireType_SLIP         254
#define S2WireType_CSLIP        255
#define S2WireType_PLIP         420

struct Sana2PacketTypeStats
{
    ULONG PacketsSent;
    ULONG PacketsReceived;
    ULONG BytesSent;
    ULONG BytesReceived;
    ULONG PacketsDropped;
};

struct Sana2SpecialStatRecord
{
    ULONG Type;
    ULONG Count;
    char *String;
};

struct Sana2SpecialStatHeader
{
    ULONG RecordCountMax;
    ULONG RecordCountSupplied;
    /* struct Sana2SpecialStatRecord[RecordCountMax] follows */
};

struct Sana2DeviceStats
{
    ULONG PacketsReceived;
    ULONG PacketsSent;
    ULONG BadData;
    ULONG Overruns;
    ULONG Unused;
    ULONG UnknownTypesReceived;
    ULONG Reconfigurations;
    struct timeval LastStart;
};

/* Device commands. */
#define S2_START                (CMD_NONSTD)
#define S2_DEVICEQUERY          (S2_START +  0)
#define S2_GETSTATIONADDRESS    (S2_START +  1)
#define S2_CONFIGINTERFACE      (S2_START +  2)
#define S2_ADDMULTICASTADDRESS  (S2_START +  5)
#define S2_DELMULTICASTADDRESS  (S2_START +  6)
#define S2_MULTICAST            (S2_START +  7)
#define S2_BROADCAST            (S2_START +  8)
#define S2_TRACKTYPE            (S2_START +  9)
#define S2_UNTRACKTYPE          (S2_START + 10)
#define S2_GETTYPESTATS         (S2_START + 11)
#define S2_GETSPECIALSTATS      (S2_START + 12)
#define S2_GETGLOBALSTATS       (S2_START + 13)
#define S2_ONEVENT              (S2_START + 14)
#define S2_READORPHAN           (S2_START + 15)
#define S2_ONLINE               (S2_START + 16)
#define S2_OFFLINE              (S2_START + 17)
#define S2_END                  (S2_START + 18)

/* io_Error values (see also <exec/errors.h>). */
#define S2ERR_NO_ERROR          0
#define S2ERR_NO_RESOURCES      1
#define S2ERR_BAD_ARGUMENT      3
#define S2ERR_BAD_STATE         4
#define S2ERR_BAD_ADDRESS       5
#define S2ERR_MTU_EXCEEDED      6
#define S2ERR_NOT_SUPPORTED     8
#define S2ERR_SOFTWARE          9
#define S2ERR_OUTOFSERVICE      10
#define S2ERR_TX_FAILURE        11

/* ios2_WireError values. */
#define S2WERR_GENERIC_ERROR    0
#define S2WERR_NOT_CONFIGURED   1
#define S2WERR_UNIT_ONLINE      2
#define S2WERR_UNIT_OFFLINE     3
#define S2WERR_ALREADY_TRACKED  4
#define S2WERR_NOT_TRACKED      5
#define S2WERR_BUFF_ERROR       6
#define S2WERR_SRC_ADDRESS      7
#define S2WERR_DST_ADDRESS      8
#define S2WERR_BAD_BROADCAST    9
#define S2WERR_BAD_MULTICAST    10
#define S2WERR_MULTICAST_FULL   11
#define S2WERR_BAD_EVENT        12
#define S2WERR_BAD_STATDATA     13
#define S2WERR_IS_CONFIGURED    15
#define S2WERR_NULL_POINTER     16
#define S2WERR_TOO_MANY_RETIRES 17
#define S2WERR_RCVREL_HDW_ERR   18

/* S2_ONEVENT event mask bits. */
#define S2EVENT_ERROR           (1L << 0)
#define S2EVENT_TX              (1L << 1)
#define S2EVENT_RX              (1L << 2)
#define S2EVENT_ONLINE          (1L << 3)
#define S2EVENT_OFFLINE         (1L << 4)
#define S2EVENT_BUFF            (1L << 5)
#define S2EVENT_HARDWARE        (1L << 6)
#define S2EVENT_SOFTWARE        (1L << 7)

#endif /* SANA2_SANA2DEVICE_H */

/*
 * Post-Commodore buffer-management tags.
 *
 * S2_CopyToBuff16 / S2_CopyFromBuff16 are a widely-deployed extension for
 * devices whose DMA or PIO path moves 16-bit units: the hook has the same
 * (a0=to, a1=from, d0=len) signature, but the device is allowed to round the
 * length up to an even number of bytes, so the destination must tolerate one
 * spare byte.
 *
 * These tag numbers are NOT present in any header shipped with this toolchain
 * or in the Commodore SANA-II sources, so they are unverified here. Offering a
 * mis-numbered buffer-management hook to a device is a correctness hazard, not
 * merely a lost optimisation, so AMI_SANA2_OFFER_COPY16 defaults to 0 and the
 * cooked/raw paths never depend on it.
 */
#ifndef S2_CopyToBuff16
#define S2_CopyToBuff16         (S2_Dummy + 4)
#endif
#ifndef S2_CopyFromBuff16
#define S2_CopyFromBuff16       (S2_Dummy + 5)
#endif

#endif /* AMINETXDUO_SANA2_DEVICE_H */
