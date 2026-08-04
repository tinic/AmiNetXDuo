/* <dos/dostags.h> for the bsdsocket host tests.  The NP_* tags CreateNewProc
   takes.  SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_BSD_TEST_DOS_DOSTAGS_H
#define AMINETXDUO_BSD_TEST_DOS_DOSTAGS_H
#include <utility/tagitem.h>
#define NP_Dummy      (TAG_USER + 1000)
#define NP_Seglist    (NP_Dummy + 1)
#define NP_FreeSeglist (NP_Dummy + 2)
#define NP_Entry      (NP_Dummy + 3)
#define NP_Input      (NP_Dummy + 4)
#define NP_Output     (NP_Dummy + 5)
#define NP_CloseInput (NP_Dummy + 6)
#define NP_CloseOutput (NP_Dummy + 7)
#define NP_Error      (NP_Dummy + 8)
#define NP_CloseError (NP_Dummy + 9)
#define NP_CurrentDir (NP_Dummy + 10)
#define NP_StackSize  (NP_Dummy + 11)
#define NP_Name       (NP_Dummy + 13)
#define NP_Priority   (NP_Dummy + 14)
#endif
