/*
 * toolpcap, the classic libpcap file format.  See toolpcap.h.
 *
 * `unsigned long` is 32 bits on the target and 64 on the host that tests this,
 * so every field is cut to its four bytes on the way out rather than assumed
 * to be four bytes wide.  A host test that passed only because the host agreed
 * with the target about the width of a long would prove nothing about the
 * target.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolpcap.h"

static void tool_pcap_raw(ToolPcap *o, const unsigned char *data,
                          unsigned long len)
{
    unsigned long i;

    if (!o->open || o->failed || len == 0)
        return;

    o->filelen += len;

    if (o->used + len > TOOL_PCAP_BUFSIZE)
        tool_pcap_flush(o);

    /*
     * Bigger than the buffer, so it cannot be buffered at all.  Only reachable
     * with a snap length above 16 KB, which BIOCSBLEN's own ceiling does not
     * allow today; written anyway, because the alternative is a silent
     * overrun of a fixed array on a machine with no MMU.
     */
    if (len > TOOL_PCAP_BUFSIZE)
    {
        if (o->sink != 0 && o->sink(o->cookie, data, len) < 0)
            o->failed = 1;
        return;
    }

    for (i = 0; i < len; i++)
        o->buf[o->used + i] = data[i];

    o->used += len;
}

static void tool_pcap_u32(ToolPcap *o, unsigned long v)
{
    unsigned char b[4];

    b[0] = (unsigned char)((v >> 24) & 0xFFUL);
    b[1] = (unsigned char)((v >> 16) & 0xFFUL);
    b[2] = (unsigned char)((v >> 8) & 0xFFUL);
    b[3] = (unsigned char)(v & 0xFFUL);

    tool_pcap_raw(o, b, 4);
}

static void tool_pcap_u16(ToolPcap *o, unsigned long v)
{
    unsigned char b[2];

    b[0] = (unsigned char)((v >> 8) & 0xFFUL);
    b[1] = (unsigned char)(v & 0xFFUL);

    tool_pcap_raw(o, b, 2);
}

void tool_pcap_flush(ToolPcap *o)
{
    unsigned long n = o->used;

    o->used = 0;

    if (n == 0 || o->failed || o->sink == 0)
        return;

    if (o->sink(o->cookie, o->buf, n) < 0)
        o->failed = 1;
}

void tool_pcap_begin(ToolPcap *o, ToolPcapSink sink, void *cookie,
                     unsigned long snaplen)
{
    o->sink         = sink;
    o->cookie       = cookie;
    o->snaplen      = snaplen;
    o->used         = 0;
    o->records      = 0;
    o->caplen_total = 0;
    o->filelen      = 0;
    o->clamped      = 0;
    o->failed       = 0;
    o->open         = 1;

    tool_pcap_u32(o, TOOL_PCAP_MAGIC);
    tool_pcap_u16(o, TOOL_PCAP_VERSION_MAJOR);
    tool_pcap_u16(o, TOOL_PCAP_VERSION_MINOR);
    tool_pcap_u32(o, 0);                        /* thiszone: UTC            */
    tool_pcap_u32(o, 0);                        /* sigfigs                  */
    tool_pcap_u32(o, snaplen);
    tool_pcap_u32(o, TOOL_PCAP_DLT_EN10MB);
}

void tool_pcap_record(ToolPcap *o, unsigned long sec, unsigned long usec,
                      unsigned long caplen, unsigned long datalen,
                      const unsigned char *data)
{
    if (!o->open || o->failed || data == 0)
        return;

    /*
     * The two invariants a reader depends on, held here rather than trusted
     * of the caller.  A record longer than the file header's snap length is
     * what tcpdump reports as "invalid packet capture length ... bogus
     * savefile header", and it stops reading the file at that point: every
     * frame after the bad one is lost, not just the bad one.
     */
    if (caplen > o->snaplen)
    {
        caplen = o->snaplen;
        o->clamped++;
    }

    /* A frame that claims fewer bytes on the wire than it carries in the file
       is the same malformation from the other side. */
    if (datalen < caplen)
        datalen = caplen;

    tool_pcap_u32(o, sec);
    tool_pcap_u32(o, usec);
    tool_pcap_u32(o, caplen);
    tool_pcap_u32(o, datalen);
    tool_pcap_raw(o, data, caplen);

    o->records++;
    o->caplen_total += caplen;
}

void tool_pcap_end(ToolPcap *o)
{
    if (!o->open)
        return;

    tool_pcap_flush(o);
    o->open = 0;
}
