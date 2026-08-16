/* <exec/devices.h> for the netdev host test: the two structures
   netdev_internal.h embeds, and nothing else.
   SPDX-License-Identifier: MIT */

#ifndef AMINETXDUO_NETDEV_TEST_EXEC_DEVICES_H
#define AMINETXDUO_NETDEV_TEST_EXEC_DEVICES_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/ports.h>

struct Device
{
    struct Library dd_Library;
};

struct Unit
{
    struct MsgPort unit_MsgPort;
    UBYTE          unit_flags;
    UBYTE          unit_pad;
    UWORD          unit_OpenCnt;
};

#endif
