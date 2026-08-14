/*
 * anxnet.device, layer 1 of 3: what the SANA-II shell keeps.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETDEV_INTERNAL_H
#define AMINETXDUO_NETDEV_INTERNAL_H

#include <stddef.h>

#include <exec/types.h>
#include <exec/devices.h>
#include <exec/interrupts.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/ports.h>
#include <dos/dos.h>          /* BPTR, for the expunge seglist */

#include "aminetxduo/anxnet.h"
#include "netdev_nic.h"
#include "sana2_device.h"

#define NETDEV_MAX_UNITS    4
#define NETDEV_MCAST_MAX    32      /* the hash is 64 bits; more is pointless */
#define NETDEV_TRACK_MAX    16

struct NetdevUnit;

typedef struct NetdevMcast
{
    UBYTE   addr[NETDEV_ADDR_LEN];
    UWORD   refs;
} NetdevMcast;

typedef struct NetdevTrack
{
    ULONG                       type;
    UWORD                       used;
    struct Sana2PacketTypeStats st;
} NetdevTrack;

/*
 * One per OpenDevice().  io_Unit points at the opener, so every request arrives
 * already attached to the opener that made it: the CMD_READ queue, the copy
 * hooks and the RAW flag are per opener, which is the whole of what SANA-II
 * means by an opener.
 */
typedef struct NetdevOpener
{
    struct MinNode      op_Node;
    struct Unit         op_Unit;
    struct NetdevUnit  *op_Hw;

    APTR                op_CopyTo;
    APTR                op_CopyFrom;
    APTR                op_Filter;

    UBYTE               op_Raw;
    UBYTE               op_Promisc;
    UBYTE               op_Exclusive;
    UBYTE               op_Pad;

    struct List         op_Reads;
    struct List         op_Orphans;
    struct List         op_Events;

    NetdevTrack         op_Track[NETDEV_TRACK_MAX];
    UWORD               op_TrackHigh;   /* one past the highest used slot */
} NetdevOpener;

typedef struct NetdevUnit
{
    NetdevNic                   nu_Nic;
    struct NetdevDevice        *nu_Dev;
    UWORD                       nu_Unit;
    UWORD                       nu_Openers;

    UBYTE                       nu_Configured;
    UBYTE                       nu_Online;
    UBYTE                       nu_IntrAdded;
    UBYTE                       nu_Pad;

    struct List                 nu_OpenerList;
    struct List                 nu_Writes;      /* CMD_WRITE awaiting a slot */

    struct Interrupt            nu_Intr;      /* INT2, the card               */
    struct Interrupt            nu_Tick;      /* INT3 vertical blank, watchdog */
    UBYTE                       nu_TxBuilding; /* a task owns nu_TxBuf         */
    UBYTE                      *nu_TxAt;      /* where the frame was built    */
    UWORD                       nu_TxStall;   /* blanks with a transmit stuck  */
    UWORD                       nu_TxWedges;  /* how often it had to be reset  */

    NetdevMcast                 nu_Mcast[NETDEV_MCAST_MAX];
    ULONG                       nu_McastFull;   /* joins the table could not hold */
    UWORD                       nu_AllMulti;    /* ranges too wide to hash */
    UWORD                       nu_Promisc;     /* openers that asked for it */
    UBYTE                       nu_Exclusive;   /* SANA2OPF_MINE is held */
    UBYTE                       nu_Pad2;

    struct Sana2DeviceStats     nu_Stats;

    /* Frames are staged here on the way out; 4-aligned for the long window. */
    ULONG                       nu_TxBuf[(NETDEV_FRAME_MAX + 7) / 4];
} NetdevUnit;

typedef struct NetdevDevice
{
    struct Device       nd_Device;
    struct Library     *nd_ExpansionBase;
    BPTR                nd_SegList;
    UWORD               nd_UnitCount;
    /* Supported boards found past NETDEV_MAX_UNITS.  Non-zero means a fitted
       card has no unit and nothing else would have said so. */
    UWORD               nd_UnitsDropped;
    NetdevUnit          nd_Units[NETDEV_MAX_UNITS];
} NetdevDevice;

/*
 * io_Unit points at the embedded struct Unit, not at the opener, so a caller
 * or a tool that treats io_Unit as a struct Unit * is not being lied to.  The
 * opener is recovered from it here.
 */
#define NETDEV_OPENER(u) \
    ((NetdevOpener *)(void *)((UBYTE *)(u) - offsetof(NetdevOpener, op_Unit)))

/* netdev_device.c */
VOID netdev_reply(struct IOSana2Req *io, LONG err, ULONG wire);
BOOL netdev_copy_call(APTR fn, APTR to, APTR from, ULONG len);
VOID netdev_event(NetdevUnit *unit, ULONG mask);
VOID netdev_rebuild_filter(NetdevUnit *unit);
VOID netdev_tx_pump(NetdevUnit *unit);

/* netdev_pcmcia.c: the slot has no autoconfig record, so it is claimed
   rather than found.  NULL when there is no slot, nothing in it, or what is
   in it is not a LAN card. */
VOID netdev_trace_val(const char *tag, ULONG v);
APTR netdev_pcmcia_claim(const NetdevCard *card);
VOID netdev_pcmcia_release(VOID);
VOID netdev_pcmcia_bind(NetdevUnit *unit);
VOID netdev_tx_direct(NetdevUnit *unit, struct IOSana2Req *io);
VOID netdev_drop_writes(NetdevUnit *unit, NetdevOpener *op);
LONG netdev_online(NetdevUnit *unit);
VOID netdev_offline(NetdevUnit *unit, ULONG event);

/* netdev_cmds.c */
VOID netdev_perform(NetdevOpener *op, struct IOSana2Req *io);
BOOL netdev_abort(NetdevOpener *op, struct IOSana2Req *io);

#endif /* AMINETXDUO_NETDEV_INTERNAL_H */
