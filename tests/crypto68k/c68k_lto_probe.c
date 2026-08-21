/*
 * AmiNetXDuo, fast loader for the crypto68k P-256 LTO probe.
 *
 * A build configured with AMINETXDUO_CRYPTO68K_LTO_PROBE runs the two-entry
 * P-256 known-answer check from tls.library's init routine.  That preserves
 * the full product link and LTO partitioning while removing the socket, the
 * network and the peer.  OpenLibrary() is therefore the whole experiment.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/libraries.h>
#include <proto/dos.h>
#include <proto/exec.h>

int main(void)
{

struct Library  *base;


    PutStr((CONST_STRPTR)"AmiNetXDuo, crypto68k P-256 LTO library probe\n");
    PutStr((CONST_STRPTR)"  BEGIN OpenLibrary and P-256 self-check\n");

    base = OpenLibrary((CONST_STRPTR)"tls.library", 1UL);
    if (base == NULL)
    {
        PutStr((CONST_STRPTR)"  FAIL: tls.library did not initialize\n");
        PutStr((CONST_STRPTR)"1 checks, 1 failures, FAIL\n");
        return(20);
    }

    switch (base -> lib_Revision)
    {
        case 0u:
            CloseLibrary(base);
            PutStr((CONST_STRPTR)"1 checks, 0 failures, PASS\n");
            return(0);
        /* Diagnostic status values are defined beside the self-check in
           c68k_p256.h.  This loader deliberately does not link crypto68k. */
        case 0x201u:
            PutStr((CONST_STRPTR)"  FAIL: generic multiply X coordinate\n");
            break;
        case 0x202u:
            PutStr((CONST_STRPTR)"  FAIL: generic multiply Y coordinate\n");
            break;
        case 0x203u:
            PutStr((CONST_STRPTR)"  FAIL: fixed-base comb X coordinate\n");
            break;
        case 0x204u:
            PutStr((CONST_STRPTR)"  FAIL: fixed-base comb Y coordinate\n");
            break;
        case 0x211u:
            PutStr((CONST_STRPTR)"  FAIL: field multiplication\n");
            break;
        case 0x212u:
            PutStr((CONST_STRPTR)"  FAIL: field squaring\n");
            break;
        case 0x213u:
            PutStr((CONST_STRPTR)"  FAIL: field inversion\n");
            break;
        default:
            PutStr((CONST_STRPTR)"  FAIL: unknown P-256 self-check status\n");
            break;
    }

    CloseLibrary(base);
    PutStr((CONST_STRPTR)"1 checks, 1 failures, FAIL\n");
    return(20);
}
