/*
 * toolpcap, the classic libpcap file format, written by hand.
 *
 * Classic libpcap and not pcapng: it opens in Wireshark and in tcpdump with no
 * conversion step, and the whole format is a 24-byte file header and a 16-byte
 * header in front of each frame.
 *
 * Every field goes out big-endian, byte by byte, including the magic.  A pcap
 * file carries its endianness in that first longword and every tool in the
 * family has honoured it since 1993, so a 68k-written file opens on a
 * little-endian host as it stands.  Writing the bytes by hand rather than
 * storing a ULONG also avoids any alignment assumption about the buffer.
 *
 * No Amiga headers and no file handle, so the host tests can compile it and so
 * that NetTrace and NetCapture write the same bytes.  Where those bytes go is
 * the sink's business; toolbpf.c has the dos.library one.  Two copies of a
 * file-format writer is how they drift.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TOOLPCAP_H
#define AMINETXDUO_TOOLPCAP_H

#ifdef __cplusplus
extern "C" {
#endif

#define TOOL_PCAP_MAGIC             0xa1b2c3d4UL
#define TOOL_PCAP_VERSION_MAJOR     2
#define TOOL_PCAP_VERSION_MINOR     4
#define TOOL_PCAP_DLT_EN10MB        1

#define TOOL_PCAP_FILE_HDR          24
#define TOOL_PCAP_REC_HDR           16

/*
 * 16 KB.  One Write() per 16 KB rather than one per frame: a floppy or an IDE
 * write on this machine costs milliseconds, and a capture that stops to write
 * every 96-byte record drops frames while it does.  The buffer lives inside
 * ToolPcap, which is why the owning struct is a file-scope static in every
 * caller: a Shell command runs on a 4 KB stack.
 */
#define TOOL_PCAP_BUFSIZE           16384UL

/*
 * Where the bytes go.  0 accepted, negative means the write failed and the
 * capture is truncated from here on.  Called with `len` up to TOOL_PCAP_BUFSIZE
 * and never with 0.
 */
typedef int (*ToolPcapSink)(void *cookie, const unsigned char *data,
                            unsigned long len);

typedef struct ToolPcap
{
    ToolPcapSink    sink;
    void           *cookie;

    unsigned long   snaplen;        /* as written in the file header        */
    unsigned long   used;           /* bytes in buf                         */

    unsigned long   records;        /* frames written                       */
    unsigned long   caplen_total;   /* frame bytes written, headers apart   */
    unsigned long   filelen;        /* every byte accepted, buffered or not */
    unsigned long   clamped;        /* records cut down to snaplen          */

    int             failed;         /* a sink call said no                  */
    int             open;

    unsigned char   buf[TOOL_PCAP_BUFSIZE];
} ToolPcap;

/*
 * Start a file: zero the state and write the 24-byte header.  `snaplen` is
 * what the reader is told the longest stored frame can be, and is enforced on
 * every record below -- a record longer than the file header promises is a
 * malformed file, and tcpdump and Wireshark disagree about which of the two to
 * believe.
 */
void tool_pcap_begin(ToolPcap *o, ToolPcapSink sink, void *cookie,
                     unsigned long snaplen);

/*
 * One frame.  `caplen` is how many bytes `data` holds and `datalen` how many
 * the frame had on the wire; a datalen below caplen is corrected upwards
 * rather than written, for the same reason as the clamp above.
 */
void tool_pcap_record(ToolPcap *o, unsigned long sec, unsigned long usec,
                      unsigned long caplen, unsigned long datalen,
                      const unsigned char *data);

/* Push what is buffered at the sink.  Called by tool_pcap_end() as well. */
void tool_pcap_flush(ToolPcap *o);

/* Flush and mark the file finished.  Further records are refused. */
void tool_pcap_end(ToolPcap *o);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_TOOLPCAP_H */
