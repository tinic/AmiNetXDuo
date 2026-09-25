/*
 * rt_Name out of a load file, without loading it. See romtag.h.
 *
 * Before relocation a pointer in a hunk holds an offset into the hunk its
 * RELOC32 entry names, so a RomTag is a 0x4AFC whose rt_MatchTag equals its own
 * offset and is relocated against its own hunk; rt_Name is then an offset into
 * whichever hunk the entry at +14 names.
 *
 * SPDX-License-Identifier: MIT
 */

#include "romtag.h"

#define HUNK_CODE           0x3E9UL
#define HUNK_DATA           0x3EAUL
#define HUNK_BSS            0x3EBUL
#define HUNK_RELOC32        0x3ECUL
#define HUNK_SYMBOL         0x3F0UL
#define HUNK_DEBUG          0x3F1UL
#define HUNK_END            0x3F2UL
#define HUNK_HEADER         0x3F3UL
#define HUNK_DREL32         0x3F7UL     /* what V37 LoadSeg reads as SHORT */
#define HUNK_RELOC32SHORT   0x3FCUL

#define ROMTAG_HUNKS_MAX    16
#define RTC_MATCHWORD       0x4AFCU
#define RT_MATCHTAG         2           /* offsets inside struct Resident */
#define RT_NAME             14
#define NO_HUNK             0xFFFFFFFFUL

typedef struct RtHunk
{
    const UBYTE *data;                  /* NULL for BSS */
    ULONG        len;
} RtHunk;

typedef struct RtScan
{
    RtHunk hunks[ROMTAG_HUNKS_MAX];
    ULONG  count;
    ULONG  tag_hunk;                    /* NO_HUNK until a candidate is seen */
    ULONG  tag_off;
    ULONG  match_target;                /* hunk the +2 entry names           */
    ULONG  name_target;                 /* hunk the +14 entry names          */
} RtScan;

static ULONG rt_long(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8) | (ULONG)p[3];
}

static ULONG rt_word(const UBYTE *p)
{
    return ((ULONG)p[0] << 8) | (ULONG)p[1];
}

static VOID rt_find_tag(RtScan *s, ULONG hunk)
{
    const RtHunk *h = &s->hunks[hunk];
    ULONG         off;

    if (s->tag_hunk != NO_HUNK || h->data == NULL || h->len < RT_NAME + 4)
        return;

    for (off = 0; off + RT_NAME + 4 <= h->len; off += 2)
    {
        if (rt_word(h->data + off) == RTC_MATCHWORD &&
            rt_long(h->data + off + RT_MATCHTAG) == off)
        {
            s->tag_hunk = hunk;
            s->tag_off  = off;
            return;
        }
    }
}

static VOID rt_reloc(RtScan *s, ULONG hunk, ULONG target, ULONG off)
{
    if (hunk != s->tag_hunk)
        return;

    if (off == s->tag_off + RT_MATCHTAG)
        s->match_target = target;
    else if (off == s->tag_off + RT_NAME)
        s->name_target = target;
}

BOOL romtag_name(const UBYTE *file, ULONG len, char *name, ULONG namelen)
{
    RtScan s;
    ULONG  pos = 0;
    ULONG  first;
    ULONG  last;
    ULONG  hunk = 0;
    ULONG  i;

    if (name == NULL || namelen == 0)
        return FALSE;
    name[0] = '\0';

    if (file == NULL || len < 20 || len > ROMTAG_FILE_MAX ||
        rt_long(file) != HUNK_HEADER || rt_long(file + 4) != 0)
        return FALSE;

    first = rt_long(file + 12);
    last  = rt_long(file + 16);
    pos   = 20;
    if (last < first || last - first >= ROMTAG_HUNKS_MAX)
        return FALSE;

    s.count        = last - first + 1;
    s.tag_hunk     = NO_HUNK;
    s.tag_off      = 0;
    s.match_target = NO_HUNK;
    s.name_target  = NO_HUNK;

    /* The size table; both memory bits set means one more long follows. */
    for (i = 0; i < s.count; i++)
    {
        ULONG size;

        if (pos + 4 > len)
            return FALSE;
        size = rt_long(file + pos);
        pos += 4;
        if ((size & 0xC0000000UL) == 0xC0000000UL)
            pos += 4;
        s.hunks[i].data = NULL;
        s.hunks[i].len  = 0;
    }

    while (pos + 4 <= len && hunk < s.count)
    {
        ULONG type = rt_long(file + pos) & 0x3FFFFFFFUL;
        ULONG n;

        pos += 4;

        switch (type)
        {
        case HUNK_CODE:
        case HUNK_DATA:
        case HUNK_BSS:
            if (pos + 4 > len)
                return FALSE;
            n = (rt_long(file + pos) & 0x3FFFFFFFUL) * 4;
            pos += 4;
            s.hunks[hunk].len = n;
            if (type != HUNK_BSS)
            {
                if (n > len - pos)
                    return FALSE;
                s.hunks[hunk].data = file + pos;
                pos += n;
                rt_find_tag(&s, hunk);
            }
            break;

        case HUNK_RELOC32:
            for (;;)
            {
                ULONG target;

                if (pos + 4 > len)
                    return FALSE;
                n = rt_long(file + pos);
                pos += 4;
                if (n == 0)
                    break;
                if (pos + 4 > len || n > (len - pos - 4) / 4)
                    return FALSE;
                target = rt_long(file + pos);
                pos += 4;
                for (i = 0; i < n; i++, pos += 4)
                    rt_reloc(&s, hunk, target, rt_long(file + pos));
            }
            break;

        case HUNK_RELOC32SHORT:
        case HUNK_DREL32:
            for (;;)
            {
                ULONG target;

                if (pos + 2 > len)
                    return FALSE;
                n = rt_word(file + pos);
                pos += 2;
                if (n == 0)
                    break;
                if (pos + 2 > len || n > (len - pos - 2) / 2)
                    return FALSE;
                target = rt_word(file + pos);
                pos += 2;
                for (i = 0; i < n; i++, pos += 2)
                    rt_reloc(&s, hunk, target, rt_word(file + pos));
            }
            pos = (pos + 3) & ~3UL;
            break;

        case HUNK_SYMBOL:
            for (;;)
            {
                if (pos + 4 > len)
                    return FALSE;
                n = rt_long(file + pos) & 0x00FFFFFFUL;
                pos += 4;
                if (n == 0)
                    break;
                if ((len - pos) / 4 < n + 1)
                    return FALSE;
                pos += n * 4 + 4;
            }
            break;

        case HUNK_DEBUG:
            if (pos + 4 > len)
                return FALSE;
            n = rt_long(file + pos);
            pos += 4;
            if (n > (len - pos) / 4)
                return FALSE;
            pos += n * 4;
            break;

        case HUNK_END:
            hunk++;
            break;

        default:
            return FALSE;               /* overlays, or not a load file */
        }
    }

    if (s.tag_hunk == NO_HUNK || s.match_target != s.tag_hunk ||
        s.name_target >= s.count || s.hunks[s.name_target].data == NULL)
        return FALSE;

    {
        const RtHunk *h   = &s.hunks[s.name_target];
        ULONG         off = rt_long(s.hunks[s.tag_hunk].data + s.tag_off +
                                    RT_NAME);
        ULONG         out = 0;

        for (; off < h->len && h->data[off] != '\0'; off++)
        {
            if (out + 1 < namelen)
                name[out++] = (char)h->data[off];
        }
        name[out] = '\0';

        return (BOOL)(off < h->len && out > 0);
    }
}
