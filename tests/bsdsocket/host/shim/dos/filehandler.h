/* <dos/filehandler.h> for the bsdsocket host tests.  The DosPacket action
   numbers TCP: answers, and the startup structures a handler is handed.
   SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_BSD_TEST_DOS_FILEHANDLER_H
#define AMINETXDUO_BSD_TEST_DOS_FILEHANDLER_H
#include <exec/types.h>
#include <dos/dos.h>

struct DosEnvec {
    ULONG de_TableSize, de_SizeBlock, de_SecOrg, de_Surfaces;
    ULONG de_SectorPerBlock, de_BlocksPerTrack, de_Reserved, de_PreAlloc;
    ULONG de_Interleave, de_LowCyl, de_HighCyl, de_NumBuffers;
    ULONG de_BufMemType, de_MaxTransfer, de_Mask, de_BootPri;
    ULONG de_DosType, de_Baud, de_Control, de_BootBlocks;
};

struct FileSysStartupMsg {
    ULONG fssm_Unit;
    BSTR  fssm_Device;
    BPTR  fssm_Environ;
    ULONG fssm_Flags;
};
#endif
