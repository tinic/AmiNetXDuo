/* struct Resident, for the bsdsocket host tests.
   SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_BSD_TEST_EXEC_RESIDENT_H
#define AMINETXDUO_BSD_TEST_EXEC_RESIDENT_H
struct Resident {
    UWORD  rt_MatchWord;
    struct Resident *rt_MatchTag;
    APTR   rt_EndSkip;
    UBYTE  rt_Flags;
    UBYTE  rt_Version;
    UBYTE  rt_Type;
    BYTE   rt_Pri;
    char  *rt_Name;
    char  *rt_IdString;
    APTR   rt_Init;
};
#define RTC_MATCHWORD 0x4AFC
#define RTF_AUTOINIT  (1<<7)
#endif
