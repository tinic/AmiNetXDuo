/* <exec/io.h> for the sana2 host test: the fields sana2_device.h's
   struct IOSana2Req is built on, and nothing else.
   SPDX-License-Identifier: MIT */

#ifndef AMINETXDUO_SANA2_TEST_EXEC_IO_H
#define AMINETXDUO_SANA2_TEST_EXEC_IO_H

#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/ports.h>

struct Device;
struct Unit;

struct IORequest
{
    struct Message   io_Message;
    struct Device   *io_Device;
    struct Unit     *io_Unit;
    UWORD            io_Command;
    UBYTE            io_Flags;
    BYTE             io_Error;
};

struct IOStdReq
{
    struct Message   io_Message;
    struct Device   *io_Device;
    struct Unit     *io_Unit;
    UWORD            io_Command;
    UBYTE            io_Flags;
    BYTE             io_Error;
    ULONG            io_Actual;
    ULONG            io_Length;
    APTR             io_Data;
    ULONG            io_Offset;
};

#define CMD_INVALID     0
#define CMD_RESET       1
#define CMD_READ        2
#define CMD_WRITE       3
#define CMD_UPDATE      4
#define CMD_CLEAR       5
#define CMD_STOP        6
#define CMD_START       7
#define CMD_FLUSH       8
#define CMD_NONSTD      9

#define IOF_QUICK       (1 << 0)

#endif
