/*
 * AmiNetXDuo, mbuf ABI pin.
 *
 * Amiga-only, and deliberately a translation unit of its own: on the Amiga
 * "aminetxduo/mbuf.h" is <sys/mbuf.h>, so every assertion below runs against
 * the Roadshow NDK header itself. If a future toolchain substitutes a
 * different <sys/mbuf.h>, the way this toolchain's <pwd.h> turned out to be
 * newlib's rather than the usergroup ABI's, silently wrong by 12 bytes
 * (docs/RESEARCH.md 3.3), the build stops here instead of shipping a struct
 * that applications walk wrongly.
 *
 * Every number was measured on 2026-07-24 with
 *   m68k-amigaos-gcc 15.2.0 -c -O2 -m68020 -Wall -Wextra
 * against ndk-include/sys/mbuf.h (Roadshow, Olaf Barthel 2001-2016, over
 * 4.4BSD's @(#)mbuf.h 8.5 2/19/95).
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/mbuf.h"

#include <stddef.h>

#ifdef AMI_MBUF_REPLICA
#  error "mbuf_abi_check.c is only meaningful against the real NDK <sys/mbuf.h>"
#endif

/* struct m_hdr, the part callers walk directly. */
AMI_STATIC_ASSERT(offsetof(struct mbuf, m_next)    == AMI_MBUF_OFF_NEXT,    "mh_next");
AMI_STATIC_ASSERT(offsetof(struct mbuf, m_nextpkt) == AMI_MBUF_OFF_NEXTPKT, "mh_nextpkt");
AMI_STATIC_ASSERT(offsetof(struct mbuf, m_data)    == AMI_MBUF_OFF_DATA,    "mh_data");
AMI_STATIC_ASSERT(offsetof(struct mbuf, m_len)     == AMI_MBUF_OFF_LEN,     "mh_len");
AMI_STATIC_ASSERT(offsetof(struct mbuf, m_type)    == AMI_MBUF_OFF_TYPE,    "mh_type");
AMI_STATIC_ASSERT(offsetof(struct mbuf, m_flags)   == AMI_MBUF_OFF_FLAGS,   "mh_flags");

/* The M_dat union. */
AMI_STATIC_ASSERT(offsetof(struct mbuf, m_pkthdr) == AMI_MBUF_OFF_PKTHDR, "m_pkthdr");
AMI_STATIC_ASSERT(offsetof(struct mbuf, m_ext)    == AMI_MBUF_OFF_EXT,    "m_ext");
AMI_STATIC_ASSERT(offsetof(struct mbuf, m_pktdat) == AMI_MBUF_OFF_PKTDAT, "m_pktdat");
AMI_STATIC_ASSERT(offsetof(struct mbuf, m_dat)    == AMI_MBUF_OFF_DAT,    "m_dat");

/* Field widths: m_len is a 32-bit LONG and m_type/m_flags are 16-bit WORDs.
   Getting these wrong is the classic way to be right about offsets and still
   corrupt the next field. */
AMI_STATIC_ASSERT(sizeof(((struct mbuf *)0)->m_len)   == 4, "mh_len width");
AMI_STATIC_ASSERT(sizeof(((struct mbuf *)0)->m_type)  == 2, "mh_type width");
AMI_STATIC_ASSERT(sizeof(((struct mbuf *)0)->m_flags) == 2, "mh_flags width");
AMI_STATIC_ASSERT(sizeof(((struct mbuf *)0)->m_next)  == 4, "mh_next width");

/* struct pkthdr and struct m_ext. m_ext is a macro over the union member, so
   the tag has to be uncovered to be measured. */
AMI_STATIC_ASSERT(sizeof(struct pkthdr) == AMI_MBUF_PKTHDR_SIZE, "pkthdr size");
AMI_STATIC_ASSERT(offsetof(struct pkthdr, rcvif) == 0, "pkthdr.rcvif");
AMI_STATIC_ASSERT(offsetof(struct pkthdr, len)   == 4, "pkthdr.len");

#undef m_ext
AMI_STATIC_ASSERT(sizeof(struct m_ext) == AMI_MBUF_EXT_SIZE,  "m_ext size");
AMI_STATIC_ASSERT(offsetof(struct m_ext, ext_buf)  == 0, "ext_buf");
AMI_STATIC_ASSERT(offsetof(struct m_ext, ext_free) == 4, "ext_free");
AMI_STATIC_ASSERT(offsetof(struct m_ext, ext_size) == 8, "ext_size");
#define m_ext M_dat.MH.MH_dat.MH_ext

/* Sizes and the derived data-area lengths. */
AMI_STATIC_ASSERT(sizeof(struct mbuf)  == AMI_MBUF_SIZE,     "mbuf size");
AMI_STATIC_ASSERT(sizeof(struct m_hdr) == AMI_MBUF_HDR_SIZE, "m_hdr size");
AMI_STATIC_ASSERT(MSIZE    == AMI_MBUF_SIZE,  "MSIZE");
AMI_STATIC_ASSERT(MLEN     == AMI_MBUF_MLEN,  "MLEN");
AMI_STATIC_ASSERT(MHLEN    == AMI_MBUF_MHLEN, "MHLEN");
AMI_STATIC_ASSERT(MCLBYTES == 2048,           "MCLBYTES");
AMI_STATIC_ASSERT(MINCLSIZE == AMI_MBUF_MHLEN + 1, "MINCLSIZE");

/* Flag bits. Applications test these directly. */
AMI_STATIC_ASSERT(M_EXT     == 0x0001, "M_EXT");
AMI_STATIC_ASSERT(M_PKTHDR  == 0x0002, "M_PKTHDR");
AMI_STATIC_ASSERT(M_EOR     == 0x0004, "M_EOR");
AMI_STATIC_ASSERT(M_BCAST   == 0x0100, "M_BCAST");
AMI_STATIC_ASSERT(M_MCAST   == 0x0200, "M_MCAST");
AMI_STATIC_ASSERT(M_COPYALL == 1000000000, "M_COPYALL");

/* Not an empty translation unit, ISO C forbids that, and some linkers
   dislike the empty object it produces. */
const char ami_mbuf_abi_checked[] = "mbuf ABI pinned against ndk-include/sys/mbuf.h";
