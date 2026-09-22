/*
 * anxnet.device: the arithmetic under the serial trace and the beam clock.
 *
 * netdev_device.c's nd_tracex(), nd_now() and nd_since_at() are compiled only
 * under NETDEV_TRACE or NETDEV_TIME and read the custom chips directly, so
 * nothing had ever run their sums off target.  The sums are here, inline,
 * with the register reads and the serial writes left where they were: the
 * clock is bracketed around every frame in a NETDEV_TIME build, and a call
 * per bracket would be the instrument measuring itself.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETDEV_TRACE_H
#define AMINETXDUO_NETDEV_TRACE_H

#include <exec/types.h>

/* Eight hex digits, CR LF and the terminator: 11 bytes into buf. */
#define NETDEV_TRACE_HEX8_LEN   11

static inline VOID netdev_trace_hex8(char *buf, ULONG v)
{
    static const char hex[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 8; i++)
        buf[i] = hex[(v >> ((7 - i) * 4)) & 0xf];
    buf[8]  = '\r';
    buf[9]  = '\n';
    buf[10] = '\0';
}

/*
 * The beam clock.  A unit is one of 256 per line, of which 227 are colour
 * clocks -- the shift in place of a MULU is why -- so a unit is 227/256 of a
 * colour clock and every span is scaled the same way.
 */
#define NETDEV_BEAM_UNIT_NUM    227UL
#define NETDEV_BEAM_UNIT_DEN    256UL

/* The smallest field height in units: NTSC's 262 lines.  A span taken before
   the clock has seen a whole field is not credited with a shorter one. */
#define NETDEV_BEAM_FIELD_MIN   (262UL * 256UL)

/*
 * Nine bits of vpos over eight of hpos.  hi is VPOSR bit 0 (vpos bit 8), vh
 * is VHPOSR: vpos bits 7..0 in the high byte, hpos in the low.
 */
static inline ULONG netdev_beam_pack(UWORD hi, UWORD vh)
{
    return (((ULONG)hi << 8) | (ULONG)(vh >> 8)) << 8 | (ULONG)(vh & 0xff);
}

/*
 * Units from t0 to t1.  A backwards step is the end of the field and nothing
 * else, so the span is repaired by the field height, which is the largest
 * value the clock has returned plus one, floored at NETDEV_BEAM_FIELD_MIN.
 * *wrapped says whether that repair was applied.
 */
static inline ULONG netdev_beam_since(ULONG t0, ULONG t1, ULONG field_top,
                                      BOOL *wrapped)
{
    ULONG wrap;

    if (t1 >= t0)
    {
        *wrapped = FALSE;
        return t1 - t0;
    }

    wrap = field_top + 1UL;
    if (wrap < NETDEV_BEAM_FIELD_MIN)
        wrap = NETDEV_BEAM_FIELD_MIN;

    *wrapped = TRUE;
    return wrap + t1 - t0;
}

#endif /* AMINETXDUO_NETDEV_TRACE_H */
