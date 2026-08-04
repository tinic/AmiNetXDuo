/* <sys/mbuf.h> for the bsdsocket host tests.  AmiTCP's mbuf surface is
   declared but not implemented here, so nothing needs its contents.
   SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_BSD_TEST_SYS_MBUF_H
#define AMINETXDUO_BSD_TEST_SYS_MBUF_H
struct mbuf { struct mbuf *m_next; ULONG m_len; APTR m_data; };
#endif
