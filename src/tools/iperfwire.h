/*
 * The iperf 2 wire format, and the arithmetic that turns a byte count into a
 * rate.  No Amiga headers, so the host tests can compile it.
 *
 * This is iperf 2, not iperf 3.  The two share a name and nothing else: iperf 3
 * opens a control connection on port 5201 and negotiates in JSON, and will not
 * talk to anything here.  A user needs `iperf` 2.x on the other end.  Every
 * field below was read off iperf 2.2.1 on the wire rather than out of its
 * source, and tests/tools/iperfpeer.py is the same format written twice.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_IPERFWIRE_H
#define AMINETXDUO_IPERFWIRE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Every integer on this wire is big-endian, which is the m68k's own order, but
 * these are written a byte at a time anyway: the datagram header is not
 * aligned to anything once a caller puts it at an offset, and a 68000 traps on
 * a misaligned 32-bit load.
 */

/* The datagram header a UDP sender writes, then 24 bytes of client_hdr. */
#define IPERF_DG_LEN        12
#define IPERF_CLIENT_HDR    24
#define IPERF_DG_TOTAL      (IPERF_DG_LEN + IPERF_CLIENT_HDR)

/*
 * The server's answer to the end-of-test datagram.  16 bytes of echoed
 * datagram header (iperf 2.0.10 widened it to carry a 64-bit sequence number),
 * then the report itself.  128 is what 2.2.1 sends.  The trailing fields are
 * its own extensions and a receiver that stops at 56 reads every documented
 * one, so this is padded rather than filled.
 */
#define IPERF_REPORT_OFF    16
#define IPERF_REPORT_LEN    128

/* Set in the report's flags word.  A client that sees it parses the rest. */
#define IPERF_HEADER_VERSION1   0x80000000UL

/* iperf 2's own default datagram, and the ceiling this tool will send.
   The ceiling is 64 KB because -l is the size of one recv(), and the per-call
   cost of a receive is what a read-size sweep is measuring: at 4096 a 400 KB/s
   transfer pays it a hundred times a second, at 65536 six times. */
#define IPERF_UDP_DEFAULT   1470
#define IPERF_BUF_MIN       64
#define IPERF_BUF_MAX       65536

/* The port iperf 2 listens on.  iperf 3 uses 5201 and is a different protocol. */
#define IPERF_PORT          5001

typedef struct IperfWireReport
{
    unsigned long   flags;
    unsigned long   bytes_hi;
    unsigned long   bytes_lo;
    unsigned long   stop_sec;
    unsigned long   stop_usec;
    unsigned long   lost;
    unsigned long   outoforder;
    unsigned long   datagrams;
    unsigned long   jitter_sec;
    unsigned long   jitter_usec;
} IperfWireReport;

/*
 * Fill `len` bytes with iperf's own payload, the digits 0-9 repeating, taking
 * `offset` as the position of buf[0] in the stream.
 *
 * The pattern is load-bearing.  A TCP receiver at the far end reads the first
 * four bytes as a client_hdr and looks at the top two bits: 0x80000000 says
 * "version 1 header follows" and 0x40000000 "extended header follows", and a
 * stream that begins with an arbitrary byte sets one of them about half the
 * time and is then parsed as a negotiation that never happens.  '0','1','2','3'
 * is 0x30313233, which has both clear.  Same reasoning as the zeroed
 * client_hdr in iperf_dg_put().
 */
void iperf_pattern_fill(unsigned char *buf, unsigned long len,
                        unsigned long offset);

/*
 * Write a datagram header at `buf`, which must have IPERF_DG_TOTAL bytes.
 * A negative `id` is the end-of-test marker.  iperf 2 sends -N after ids 1..N.
 *
 * The 24 bytes after the header are zeroed rather than left to the caller's
 * buffer.  An iperf 2.2 server reads a client_hdr there and switches test mode
 * on its flag bits: fed uninitialised memory it enters trip-time mode, reports
 * one datagram and then silently drops the rest of the run.  The vendored
 * nx_iperf.c does exactly that, reproduced against iperf 2.2.1 before this was
 * written.
 */
void iperf_dg_put(unsigned char *buf, long id,
                  unsigned long sec, unsigned long usec);

/* The id out of a received datagram.  Negative means end of test. */
long iperf_dg_id(const unsigned char *buf);

/* Read that id only when the complete datagram header is present. */
int iperf_dg_get(const unsigned char *buf, unsigned long len, long *id);

/* Build the server's end-of-test report.  `buf` needs IPERF_REPORT_LEN bytes. */
void iperf_report_put(unsigned char *buf, const IperfWireReport *rep);

/*
 * Parse one.  0 on success, -1 if it is too short or the flags word does not
 * carry IPERF_HEADER_VERSION1, which is how a stray datagram is told from an
 * answer.
 */
int iperf_report_get(const unsigned char *buf, unsigned long len,
                     IperfWireReport *rep);

/* ------------------------------------------------------------ arithmetic - */

/*
 * Bits per second from a byte count and a millisecond count, rounded to
 * nearest.  0 when `ms` is 0, because a measurement with no duration has no
 * rate.  Callers report that rather than dividing.
 *
 * In 64 bits throughout.  The vendored engine computes
 * `bytes / ticks * rate / 125000` in integer, which divides first and then
 * truncates to whole Mbit/s: at the 3.2 Mbit/s an A1200 actually reaches it
 * reports 3, and below 1 Mbit/s it reports 0.
 */
unsigned long iperf_bits_per_sec(unsigned long bytes_hi, unsigned long bytes_lo,
                                 unsigned long ms);

/*
 * "3.20 Mbit/s" and the like into `buf`, choosing bit/s, Kbit/s or Mbit/s so
 * two decimals always mean something.  Always NUL-terminates.
 */
void iperf_format_rate(char *buf, unsigned long buflen, unsigned long bits);

/*
 * "1.44 MBytes" and the like.  Same rule, and the same reason: a KByte figure
 * for a 300 MB transfer is unreadable and a MByte figure for 8 KB is 0.01.
 */
void iperf_format_bytes(char *buf, unsigned long buflen,
                        unsigned long hi, unsigned long lo);

/* Add `add` to a 64-bit count held as two 32-bit halves. */
void iperf_add64(unsigned long *hi, unsigned long *lo, unsigned long add);

/* ---------------------------------------------------------------- limits - */

/*
 * Every run states either how long it can take or how much it can move, and
 * neither is open-ended.  iperf_limits_check() is the only place that decides,
 * and both the command and httpd go through it before a socket is opened.
 *
 * A server told to run until somebody stops it prints progress for as long as
 * it lives, and a caller that redirected that to a file has an unbounded
 * writer.  A wedged iperf 2.2.1 on the test host wrote 72 GB of interval
 * reports into a shell redirect while this was being developed.
 *
 * An hour is the ceiling.  Longer than any measurement anybody wants from a
 * Shell, short enough that the millisecond clock cannot wrap inside one, and
 * it puts a hard bound on the output: one progress line a second is at most
 * IPERF_MAX_LINES of them.
 */
#define IPERF_MAX_SECONDS   3600UL
#define IPERF_MAX_LINES     IPERF_MAX_SECONDS

/*
 * NULL when the run is bounded and the sizes make sense, otherwise a sentence
 * fragment saying what is wrong.  `seconds` and `kbytes` are the two ways to
 * say when to stop and exactly one of them must be given.
 */
const char *iperf_limits_check(unsigned long seconds, unsigned long kbytes,
                               unsigned long buflen, unsigned long port);

/*
 * How many slices a run of this shape can take before it is declared stuck.
 *
 * The deadline is a clock comparison, so a clock that stops advancing means a
 * deadline that never arrives and a transfer that never ends.  The host-side
 * iperf 2.2.1 used to pin this protocol down wedged on a failed
 * clock_nanosleep() and spun for seventeen hours, filling a disk with interval
 * reports.  A timing call that fails has to end the run with a diagnosis
 * rather than become a busy loop, so there is a second bound that does not
 * consult the clock at all.
 *
 * Generous on purpose.  It is a backstop, and must never fire on a slow
 * machine, only on one whose clock has stopped.
 */
unsigned long iperf_slice_budget(unsigned long seconds, unsigned long kbytes,
                                 unsigned long buflen);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_IPERFWIRE_H */
