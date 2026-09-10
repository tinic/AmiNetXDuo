"""Re-derive every ledger row with the current scanner, in one pass.

WHY NOT rescan.sh.  It rewrites the whole ledger once per archive
(`awk '$1!=a' ledger > tmp && mv tmp ledger`), which is O(archives x rows):
5,900 rewrites of a 1 MB file.  Measured 2026-09-09: ~100 archives in several
minutes, so a full pass would have run for over an hour.  Nothing was wrong
with its verdicts -- it is the file handling that does not scale -- so this
does the same work, in parallel, writing once at the end.

Rows whose archive is not unpacked locally are LEFT EXACTLY AS THEY WERE and
counted out loud, the same rule rescan.sh states: this machine's /tmp is not
evidence about the corpus.

SPDX-License-Identifier: MIT
"""
import multiprocessing as mp
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scan
import survey_io

OUT = os.environ.get('ANXD_SURVEY_OUT', '/tmp/anxd-survey')
LEDGER = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'results.tsv')


def path_for(archive, f):
    """The on-disk path for a ledger row.

    The ledger is latin-1 (an Amiga path is bytes, see survey_io), and the
    filesystem is UTF-8, so re-encoding a decoded name gives DIFFERENT bytes:
    `Spitfire\xb2 Install` becomes `Spitfire\xc2\xb2 Install` and the file
    reads as missing.  os.fsdecode of the original bytes carries them through
    unchanged via surrogateescape, which is what the OS calls expect."""
    name = os.path.join(OUT, archive + '.d', f)
    return os.fsdecode(name.encode('latin-1', 'surrogateescape'))


def redo(fields):
    """New (verdict, distinct, calls, lvos) for one row, or None to keep it."""
    archive, f = fields[0], fields[1]
    if f == '-':
        # An archive-level row: the claim is "no HUNK binary here mentions
        # bsdsocket.library".  Re-test it the way the pipeline does rather than
        # carrying it forward on trust.
        d = os.path.join(OUT, archive + '.d')
        if not os.path.isdir(d):
            return None
        # The claim is about HUNK BINARIES, not about the archive's text.  A
        # plain `grep -rl` re-flagged 242 archives, of which 240 were READMEs,
        # AmigaGuides and C source mentioning the library -- an alarm that
        # would have read as a 242-archive coverage hole.  Two were real
        # (iiNST_151, interInstal: `TCP3ADD/bin/hostname`), and they are the
        # reason to run the strict test rather than trust the row.
        for root, _ds, fs in os.walk(d):
            for fn in fs:
                fp = os.path.join(root, fn)
                try:
                    with open(fp, 'rb') as fh:
                        if fh.read(4) != b'\x00\x00\x03\xf3':
                            continue
                        fh.seek(0)
                        if b'bsdsocket.library' not in fh.read():
                            continue
                except OSError:
                    continue
                return ('RECHECK_BINARY_FOUND:' + os.path.relpath(fp, d), 0, 0, '')
        return (fields[2], 0, 0, '')
    p = path_for(archive, f)
    if not os.path.exists(p):
        return None
    try:
        verdict, offs, total = scan.scan(p)
    except Exception as e:
        return ('SCAN_ERROR:%s' % type(e).__name__, 0, 0, '')
    names = ','.join(scan.LVO.get(o, '?%d' % o) for o in offs)
    return (verdict, len(offs), total, names)


def main():
    rows = [l.split('\t') for l in survey_io.lines(LEDGER)]
    head, data = rows[0], [r for r in rows[1:] if len(r) >= 7]
    with mp.Pool(int(os.environ.get('ANXD_JOBS', '8'))) as pool:
        out = pool.map(redo, data, chunksize=16)
    kept = changed = missing = 0
    for r, new in zip(data, out):
        if new is None:
            missing += 1
            continue
        verdict, ndist, ncalls, names = new
        before = (r[2], r[3], r[4], r[5])
        r[2] = verdict
        # ARCHIVE-LEVEL ROWS CARRY BARE COUNTS, per-file rows carry labelled
        # ones -- that is how tick.sh writes them, and rewriting 5,795 rows
        # into the other shape is a diff that hides the 200 real changes.
        if r[1] == '-':
            r[3], r[4] = '0', '0'
        else:
            r[3] = 'distinct=%d' % ndist
            r[4] = 'calls=%d' % ncalls
        r[5] = names
        r[6] = 'scanner=%d' % scan.SCANNER_VERSION
        if before != (r[2], r[3], r[4], r[5]):
            changed += 1
        kept += 1
    with survey_io.out(LEDGER) as fh:
        fh.write('\t'.join(head) + '\n')
        for r in data:
            fh.write('\t'.join(r) + '\n')
    print('bulk-rescan: %d rows re-derived, %d changed, %d not unpacked locally'
          % (kept, changed, missing))


if __name__ == '__main__':
    main()
