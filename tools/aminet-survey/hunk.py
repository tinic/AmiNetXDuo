"""AmigaOS HUNK reader: yields (offset, bytes) for HUNK_CODE sections only.

The old harness scanned whole files and stepped i+=2 from byte 0, which admits
matches inside DATA, inside HUNK_DEBUG, and at offsets no instruction can
start at.  Every scan here is anchored to a real code hunk.
"""
import struct

HUNK_CODE, HUNK_DATA, HUNK_BSS = 0x3E9, 0x3EA, 0x3EB
HUNK_RELOC32, HUNK_SYMBOL, HUNK_DEBUG = 0x3EC, 0x3F0, 0x3F1
HUNK_END, HUNK_HEADER = 0x3F2, 0x3F3

def _u32(b, i):
    return struct.unpack_from('>I', b, i)[0]

def hunks(blob):
    """Yield (index, type, file_offset, payload_bytes, reloc) for every hunk.

    `reloc` maps target_hunk_index -> [offsets within THIS hunk that hold a
    pointer into it].  Needed to tell which OpenLibrary() call names
    "bsdsocket.library": the name is a pointer into a DATA hunk, and before
    relocation the instruction operand is just the offset within that hunk, so
    the operand alone is meaningless without knowing which hunk it targets.
    """
    if len(blob) < 8 or _u32(blob, 0) != HUNK_HEADER:
        return
    i = 4
    while True:
        n = _u32(blob, i); i += 4
        if n == 0:
            break
        i += n * 4
    _table_size = _u32(blob, i); i += 4
    first = _u32(blob, i); i += 4
    last = _u32(blob, i); i += 4
    i += (last - first + 1) * 4
    idx = -1
    cur = None
    while i + 4 <= len(blob):
        htype = _u32(blob, i) & 0x3FFFFFFF; i += 4
        if htype in (HUNK_CODE, HUNK_DATA):
            if cur is not None:
                yield cur
            idx += 1
            n = _u32(blob, i); i += 4
            size = n * 4
            cur = [idx, htype, i, blob[i:i + size], {}]
            i += size
        elif htype == HUNK_BSS:
            if cur is not None:
                yield cur
                cur = None
            idx += 1
            i += 4
        elif htype == HUNK_RELOC32:
            while i + 4 <= len(blob):
                cnt = _u32(blob, i); i += 4
                if cnt == 0:
                    break
                target = _u32(blob, i); i += 4
                offs = [_u32(blob, i + 4 * k) for k in range(cnt)]
                i += cnt * 4
                if cur is not None:
                    cur[4].setdefault(target, []).extend(offs)
        elif htype == HUNK_SYMBOL:
            while i + 4 <= len(blob):
                ln = _u32(blob, i); i += 4
                if ln == 0:
                    break
                i += ln * 4 + 4
        elif htype == HUNK_DEBUG:
            n = _u32(blob, i); i += 4
            i += n * 4
        elif htype == HUNK_END:
            continue
        else:
            break
    if cur is not None:
        yield cur


def code_hunks(blob):
    """Yield (file_offset, code_bytes). Returns [] for anything not a HUNK exe."""
    if len(blob) < 8 or _u32(blob, 0) != HUNK_HEADER:
        return
    i = 4
    # resident library names (usually none)
    while True:
        n = _u32(blob, i); i += 4
        if n == 0:
            break
        i += n * 4
    table_size = _u32(blob, i); i += 4
    first = _u32(blob, i); i += 4
    last = _u32(blob, i); i += 4
    i += (last - first + 1) * 4          # hunk sizes
    while i + 4 <= len(blob):
        htype = _u32(blob, i) & 0x3FFFFFFF; i += 4
        if htype in (HUNK_CODE, HUNK_DATA):
            n = _u32(blob, i); i += 4
            size = n * 4
            if htype == HUNK_CODE:
                yield (i, blob[i:i + size])
            i += size
        elif htype == HUNK_BSS:
            i += 4
        elif htype == HUNK_RELOC32:
            while i + 4 <= len(blob):
                cnt = _u32(blob, i); i += 4
                if cnt == 0:
                    break
                i += 4 + cnt * 4
        elif htype in (HUNK_SYMBOL,):
            while i + 4 <= len(blob):
                ln = _u32(blob, i); i += 4
                if ln == 0:
                    break
                i += ln * 4 + 4
        elif htype == HUNK_DEBUG:
            n = _u32(blob, i); i += 4
            i += n * 4
        elif htype == HUNK_END:
            continue
        else:
            return                        # unknown: stop rather than guess
