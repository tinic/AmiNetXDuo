/*
 * The pcap file format, byte for byte.
 *
 * A pcap writer that is wrong is not a writer that fails.  Wireshark and
 * tcpdump both read the file header first and believe it: a swapped magic and
 * they read every length backwards, a snap length below a record's own capture
 * length and tcpdump stops at that record and reports "bogus savefile header",
 * losing every frame after it rather than the one.  None of that shows up on
 * the machine that wrote the file.
 *
 * Host-side because that is where the bytes can be asserted, and because
 * `unsigned long` is 64 bits here and 32 on the target: a field written with a
 * store of a long rather than four bytes would pass on one and not the other.
 *
 * Sabotage: tests/tools/toolpcap-verdict-selftest.sh breaks toolpcap.c in five
 * named ways and requires this to fail on each.  A test nobody has seen fail
 * is a test that has not been run.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "toolpcap.h"

static int failures;
static int checks;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                    \
            printf(__VA_ARGS__);                                             \
            printf("\n");                                                    \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------- the sink -- */

#define SINK_MAX    262144

typedef struct Sink
{
    unsigned char   bytes[SINK_MAX];
    unsigned long   len;
    int             calls;
    int             refuse_after;   /* -1 never                             */
} Sink;

static Sink sink;

static int sink_write(void *cookie, const unsigned char *data,
                      unsigned long len)
{
    Sink *s = (Sink *)cookie;

    (void)cookie;

    if (s->refuse_after >= 0 && s->calls >= s->refuse_after)
    {
        s->calls++;
        return -1;
    }

    s->calls++;

    if (s->len + len > SINK_MAX)
        return -1;

    memcpy(s->bytes + s->len, data, len);
    s->len += len;

    return 0;
}

static void sink_reset(void)
{
    memset(&sink, 0, sizeof(sink));
    sink.refuse_after = -1;
}

static unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

static unsigned long be16(const unsigned char *p)
{
    return ((unsigned long)p[0] << 8) | (unsigned long)p[1];
}

/* One ToolPcap, out of line: it carries a 16 KB buffer. */
static ToolPcap out;

/* ------------------------------------------------------- the file header -- */

static void test_file_header(void)
{
    sink_reset();
    tool_pcap_begin(&out, sink_write, &sink, 96);
    tool_pcap_end(&out);

    CHECK(sink.len == TOOL_PCAP_FILE_HDR,
          "an empty capture is %lu bytes, not %d", sink.len,
          TOOL_PCAP_FILE_HDR);

    if (sink.len < TOOL_PCAP_FILE_HDR)
        return;

    /*
     * 0xa1b2c3d4 written big-endian.  A reader takes the byte order of the
     * whole file from this longword; the little-endian spelling, 0xd4c3b2a1,
     * is a VALID pcap file that says every length is byte-swapped, so getting
     * this wrong produces a file that opens and is nonsense.
     */
    CHECK(sink.bytes[0] == 0xa1 && sink.bytes[1] == 0xb2 &&
          sink.bytes[2] == 0xc3 && sink.bytes[3] == 0xd4,
          "magic is %02x%02x%02x%02x, not a1b2c3d4",
          sink.bytes[0], sink.bytes[1], sink.bytes[2], sink.bytes[3]);

    CHECK(be16(sink.bytes + 4) == 2, "major version is %lu, not 2",
          be16(sink.bytes + 4));
    CHECK(be16(sink.bytes + 6) == 4, "minor version is %lu, not 4",
          be16(sink.bytes + 6));
    CHECK(be32(sink.bytes + 8) == 0, "thiszone is %lu, not 0",
          be32(sink.bytes + 8));
    CHECK(be32(sink.bytes + 12) == 0, "sigfigs is %lu, not 0",
          be32(sink.bytes + 12));
    CHECK(be32(sink.bytes + 16) == 96, "snaplen is %lu, not 96",
          be32(sink.bytes + 16));
    CHECK(be32(sink.bytes + 20) == TOOL_PCAP_DLT_EN10MB,
          "link type is %lu, not DLT_EN10MB", be32(sink.bytes + 20));

    CHECK(out.filelen == TOOL_PCAP_FILE_HDR,
          "filelen after the header is %lu, not %d", out.filelen,
          TOOL_PCAP_FILE_HDR);
    CHECK(out.records == 0, "an empty capture holds %lu records", out.records);
}

/* The snap length is the caller's, not a constant. */
static void test_snaplen_is_recorded(void)
{
    sink_reset();
    tool_pcap_begin(&out, sink_write, &sink, 1514);
    tool_pcap_end(&out);

    CHECK(be32(sink.bytes + 16) == 1514, "snaplen is %lu, not 1514",
          be32(sink.bytes + 16));
}

/* ----------------------------------------------------------- the records -- */

static void test_one_record(void)
{
    unsigned char frame[60];
    unsigned long i;

    for (i = 0; i < sizeof(frame); i++)
        frame[i] = (unsigned char)(i + 1);

    sink_reset();
    tool_pcap_begin(&out, sink_write, &sink, 96);
    tool_pcap_record(&out, 0x51e0f2c3UL, 123456UL, 60, 64, frame);
    tool_pcap_end(&out);

    CHECK(sink.len == TOOL_PCAP_FILE_HDR + TOOL_PCAP_REC_HDR + 60,
          "one 60-byte record is %lu bytes of file", sink.len);

    if (sink.len < TOOL_PCAP_FILE_HDR + TOOL_PCAP_REC_HDR + 60)
        return;

    {
        const unsigned char *r = sink.bytes + TOOL_PCAP_FILE_HDR;

        CHECK(be32(r + 0) == 0x51e0f2c3UL, "ts_sec is %lu", be32(r + 0));
        CHECK(be32(r + 4) == 123456UL, "ts_usec is %lu", be32(r + 4));
        CHECK(be32(r + 8) == 60, "incl_len is %lu, not 60", be32(r + 8));

        /*
         * orig_len is what the frame was on the wire and NOT what was stored.
         * The two are equal only when nothing was truncated, so a writer that
         * puts caplen in both fields passes every capture taken with a snap
         * length nothing reached.
         */
        CHECK(be32(r + 12) == 64, "orig_len is %lu, not 64", be32(r + 12));

        CHECK(memcmp(r + TOOL_PCAP_REC_HDR, frame, 60) == 0,
              "the frame bytes are not the ones handed in");
    }

    CHECK(out.records == 1, "records is %lu, not 1", out.records);
    CHECK(out.caplen_total == 60, "caplen_total is %lu, not 60",
          out.caplen_total);
    CHECK(out.filelen == TOOL_PCAP_FILE_HDR + TOOL_PCAP_REC_HDR + 60,
          "filelen is %lu", out.filelen);
}

/* Back to back, in order, with nothing between them. */
static void test_records_are_contiguous(void)
{
    unsigned char frame[20];
    unsigned long i;
    unsigned long off;

    memset(frame, 0xAB, sizeof(frame));

    sink_reset();
    tool_pcap_begin(&out, sink_write, &sink, 96);
    for (i = 0; i < 100; i++)
        tool_pcap_record(&out, 1000 + i, i, 20, 20, frame);
    tool_pcap_end(&out);

    CHECK(out.records == 100, "records is %lu, not 100", out.records);
    CHECK(sink.len == TOOL_PCAP_FILE_HDR + 100 * (TOOL_PCAP_REC_HDR + 20),
          "100 records came to %lu bytes", sink.len);

    off = TOOL_PCAP_FILE_HDR;
    for (i = 0; i < 100 && off + TOOL_PCAP_REC_HDR <= sink.len; i++)
    {
        CHECK(be32(sink.bytes + off) == 1000 + i,
              "record %lu has ts_sec %lu", i, be32(sink.bytes + off));
        off += TOOL_PCAP_REC_HDR + 20;
    }

    /*
     * There is no padding between pcap records.  The capture records this
     * reads FROM are BPF_WORDALIGN padded, and carrying that alignment into
     * the file desynchronises every reader after the first odd-length frame.
     */
    CHECK(off == sink.len, "the records do not tile the file: %lu of %lu",
          off, sink.len);
}

/* More than one bufferful, so the flush path is on the record path. */
static void test_buffer_boundary(void)
{
    unsigned char frame[1500];
    unsigned long i;
    unsigned long want;

    for (i = 0; i < sizeof(frame); i++)
        frame[i] = (unsigned char)(i & 0xff);

    sink_reset();
    tool_pcap_begin(&out, sink_write, &sink, 1500);
    for (i = 0; i < 100; i++)
        tool_pcap_record(&out, i, 0, 1500, 1500, frame);
    tool_pcap_end(&out);

    want = TOOL_PCAP_FILE_HDR + 100 * (TOOL_PCAP_REC_HDR + 1500);

    CHECK(sink.len == want, "%lu bytes written, not %lu", sink.len, want);
    CHECK(sink.calls > 1, "150 KB went out in %d sink calls", sink.calls);
    CHECK(out.filelen == want, "filelen is %lu, not %lu", out.filelen, want);

    /* The last frame, in full, at the end: a boundary that drops or repeats
       bytes shows up here and nowhere in the counters. */
    if (sink.len == want)
        CHECK(memcmp(sink.bytes + want - 1500, frame, 1500) == 0,
              "the last frame is not intact across the buffer boundary");
}

/* ------------------------------------------------------- the invariants -- */

/*
 * A record longer than the file header's snap length is what makes tcpdump
 * stop reading.  The channel truncates to the filter's return value, so this
 * is the belt to that brace: clamped, counted, and the file stays readable.
 */
static void test_caplen_is_clamped(void)
{
    unsigned char frame[200];

    memset(frame, 0x5A, sizeof(frame));

    sink_reset();
    tool_pcap_begin(&out, sink_write, &sink, 96);
    tool_pcap_record(&out, 1, 2, 200, 200, frame);
    tool_pcap_end(&out);

    CHECK(out.clamped == 1, "clamped is %lu, not 1", out.clamped);
    CHECK(sink.len == TOOL_PCAP_FILE_HDR + TOOL_PCAP_REC_HDR + 96,
          "the record was written at %lu bytes",
          sink.len - TOOL_PCAP_FILE_HDR - TOOL_PCAP_REC_HDR);

    if (sink.len >= TOOL_PCAP_FILE_HDR + TOOL_PCAP_REC_HDR)
        CHECK(be32(sink.bytes + TOOL_PCAP_FILE_HDR + 8) == 96,
              "incl_len is %lu, not the snap length",
              be32(sink.bytes + TOOL_PCAP_FILE_HDR + 8));
}

/* The same malformation from the other side. */
static void test_datalen_never_below_caplen(void)
{
    unsigned char frame[64];

    memset(frame, 0x11, sizeof(frame));

    sink_reset();
    tool_pcap_begin(&out, sink_write, &sink, 96);
    tool_pcap_record(&out, 1, 2, 64, 10, frame);
    tool_pcap_end(&out);

    if (sink.len >= TOOL_PCAP_FILE_HDR + TOOL_PCAP_REC_HDR)
    {
        const unsigned char *r = sink.bytes + TOOL_PCAP_FILE_HDR;

        CHECK(be32(r + 12) >= be32(r + 8),
              "orig_len %lu is below incl_len %lu", be32(r + 12),
              be32(r + 8));
    }
}

/* Four bytes each, whatever a long is on this machine. */
static void test_fields_are_four_bytes(void)
{
    unsigned char frame[4];

    memset(frame, 0, sizeof(frame));

    sink_reset();
    tool_pcap_begin(&out, sink_write, &sink, 96);
    tool_pcap_record(&out, 0xFFFFFFFFUL, 0xFFFFFFFFUL, 4, 4, frame);
    tool_pcap_end(&out);

    CHECK(sink.len == TOOL_PCAP_FILE_HDR + TOOL_PCAP_REC_HDR + 4,
          "a record with the widest possible fields is %lu bytes of file",
          sink.len - TOOL_PCAP_FILE_HDR);

    if (sink.len >= TOOL_PCAP_FILE_HDR + TOOL_PCAP_REC_HDR)
    {
        const unsigned char *r = sink.bytes + TOOL_PCAP_FILE_HDR;

        CHECK(be32(r + 0) == 0xFFFFFFFFUL, "ts_sec came back %lu",
              be32(r + 0));
    }
}

/* ------------------------------------------------------- failure and end -- */

/*
 * A sink that says no marks the file failed and STOPS.  A writer that carries
 * on after a full disk spends the rest of the capture calling Write() for
 * nothing, and -- worse -- the second half of the file is then a run of
 * records with a hole in the middle, which is a pcap that reads as valid and
 * is not.  Once it has failed the only correct output is no more output.
 */
static void test_write_failure_is_sticky(void)
{
    unsigned char frame[1500];
    unsigned long i;

    memset(frame, 0x22, sizeof(frame));

    sink_reset();
    sink.refuse_after = 1;              /* the first flush goes, no more     */

    tool_pcap_begin(&out, sink_write, &sink, 1500);

    for (i = 0; i < 100; i++)
        tool_pcap_record(&out, i, 0, 1500, 1500, frame);

    tool_pcap_end(&out);

    CHECK(out.failed != 0, "a refused write left failed clear");

    /* One accepted call, one refusal, and nothing after it -- not ten more
       refusals and not the remaining 130 KB. */
    CHECK(sink.calls == 2, "the sink was called %d times, not twice",
          sink.calls);
    CHECK(sink.len < 100UL * (TOOL_PCAP_REC_HDR + 1500),
          "%lu bytes reached a sink that refused after the first call",
          sink.len);
}

/* A sink that refuses from the first call writes nothing at all. */
static void test_write_failure_from_the_start(void)
{
    unsigned char frame[64];

    memset(frame, 0x77, sizeof(frame));

    sink_reset();
    sink.refuse_after = 0;

    tool_pcap_begin(&out, sink_write, &sink, 96);
    tool_pcap_record(&out, 1, 0, 64, 64, frame);
    tool_pcap_end(&out);

    CHECK(out.failed != 0, "a file whose every write was refused is not "
          "marked failed");
    CHECK(sink.len == 0, "%lu bytes reached a sink that refused everything",
          sink.len);
}

/* Nothing is written after the file is finished. */
static void test_end_closes(void)
{
    unsigned char frame[64];
    unsigned long after;

    memset(frame, 0x33, sizeof(frame));

    sink_reset();
    tool_pcap_begin(&out, sink_write, &sink, 96);
    tool_pcap_record(&out, 1, 0, 64, 64, frame);
    tool_pcap_end(&out);

    after = sink.len;

    tool_pcap_record(&out, 2, 0, 64, 64, frame);
    tool_pcap_end(&out);

    CHECK(sink.len == after, "%lu bytes were written after the file ended",
          sink.len - after);
    CHECK(out.records == 1, "records is %lu after a write past the end",
          out.records);
}

/*
 * Every byte handed over is buffered or written, never both and never
 * neither: filelen is what NetCapture's SIZE limit is tested against, so a
 * filelen that runs ahead of the file stops the capture early and one that
 * lags lets the file grow past what the user allowed.
 */
static void test_filelen_tracks_the_file(void)
{
    unsigned char frame[100];
    unsigned long i;

    memset(frame, 0x44, sizeof(frame));

    sink_reset();
    tool_pcap_begin(&out, sink_write, &sink, 100);

    for (i = 0; i < 500; i++)
    {
        tool_pcap_record(&out, i, 0, 100, 100, frame);

        CHECK(out.filelen >= sink.len,
              "filelen %lu is behind the %lu bytes already written",
              out.filelen, sink.len);
    }

    tool_pcap_end(&out);

    CHECK(out.filelen == sink.len,
          "filelen is %lu and the file is %lu bytes", out.filelen, sink.len);
}

int main(void)
{
    test_file_header();
    test_snaplen_is_recorded();
    test_one_record();
    test_records_are_contiguous();
    test_buffer_boundary();
    test_caplen_is_clamped();
    test_datalen_never_below_caplen();
    test_fields_are_four_bytes();
    test_write_failure_is_sticky();
    test_write_failure_from_the_start();
    test_end_closes();
    test_filelen_tracks_the_file();

    printf("toolpcap: %d checks, %d failures\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
