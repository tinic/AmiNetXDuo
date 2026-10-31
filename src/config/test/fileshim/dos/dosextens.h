/*
 * Host test shim for config_file.c: struct DevProc, which GetDeviceProc()
 * hands out one multi-assign member at a time.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TEST_FILESHIM_DOS_DOSEXTENS_H
#define AMINETXDUO_TEST_FILESHIM_DOS_DOSEXTENS_H

#include_next <dos/dosextens.h>

struct DevProc {
    APTR  dvp_Port;
    BPTR  dvp_Lock;
    ULONG dvp_Flags;
    APTR  dvp_DevNode;
};

#define DVPB_UNLOCK  0
#define DVPF_UNLOCK  (1L << DVPB_UNLOCK)
#define DVPB_ASSIGN  1
#define DVPF_ASSIGN  (1L << DVPB_ASSIGN)

#endif
