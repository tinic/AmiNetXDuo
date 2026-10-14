/*
 * NetMeter's arithmetic: byte counts and rates as short text, the per-second
 * delta of a 64-bit count kept in two uint32_t halves, and the bar scale.  No
 * AmigaOS calls, so tests/tools/host/test_netmeter_fmt_host.c runs it on the
 * build machine.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NETMETER_FMT_H
#define NETMETER_FMT_H

#include <stdint.h>

/* The smallest full-scale a bar shrinks to, in bytes a second: below it a
   handful of ARP frames would fill the bar. */
#define NM_SCALE_FLOOR  8192u

/* "0 B/s", "512 B/s", "1.50 KB/s", "12.3 KB/s", "256 KB/s", "2.71 MB/s". */
void nm_format_rate(uint32_t bytes_per_second, char *out, uint32_t outlen);

/* "312 bytes", "1.50 KB", "12.3 MB", "4.07 GB", from a count in two halves. */
void nm_format_total(uint32_t hi, uint32_t lo, char *out, uint32_t outlen);

/* What a count in two halves grew by since the previous reading, at most
   0xFFFFFFFF.  A count that went backwards was reset (the interface was
   removed and added again) and reads zero. */
uint32_t nm_delta(uint32_t hi, uint32_t lo, uint32_t prev_hi, uint32_t prev_lo);

/* The longest interval between two samples taken as one: past it, or
   backwards, the clock was set (SNTP, a date change) and the interval is not
   measured. */
#define NM_INTERVAL_MAX 30000u

/* Milliseconds from one DateStamp (days, minutes, 50 Hz ticks) to the next;
   0 for a clock that went back or jumped past NM_INTERVAL_MAX. */
uint32_t nm_interval_ms(int32_t days0, int32_t min0, int32_t tick0,
                        int32_t days1, int32_t min1, int32_t tick1);

/* Bytes a second from bytes over milliseconds; zero for no time. */
uint32_t nm_rate(uint32_t bytes, uint32_t ms);

/* Bytes a second over a whole session: a count in two halves over
   milliseconds, rounded down, 0xFFFFFFFF when it does not fit.  No 64-bit
   division: the 68000 has none. */
uint32_t nm_average(uint32_t hi, uint32_t lo, uint32_t ms);

/* A session's bytes each way and the milliseconds they were counted over. */
typedef struct NmSpan
{
    uint32_t rx_hi, rx_lo, tx_hi, tx_lo;
    uint32_t ms;
} NmSpan;

/* One interval's bytes and length into the span.  An interval that would
   take the time past 0xFFFFFFFF ms (49.7 days) is not counted at all, bytes
   nor time, so from then on the average stays where it was. */
void nm_span_add(NmSpan *s, uint32_t rx, uint32_t tx, uint32_t ms);

/* The full-scale after one more second: a peak above it raises it with some
   headroom at once, otherwise it falls by a sixteenth, never below
   NM_SCALE_FLOOR. */
uint32_t nm_scale_next(uint32_t scale, uint32_t rx_rate, uint32_t tx_rate);

/* Pixels of a `width`-pixel bar that `rate` fills against `scale`. */
uint32_t nm_bar_fill(uint32_t rate, uint32_t scale, uint32_t width);

#endif /* NETMETER_FMT_H */
