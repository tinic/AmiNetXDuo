"""Reading and writing the survey ledger.

THE LEDGER IS NOT UTF-8 AND CANNOT BE.  Its second column is a path out of a
real Amiga archive, and Amiga filesystems store whatever bytes the local code
page produced.  `Spitfire2.lha` ships a drawer called `Spitfire<b2> Install`
-- 0xb2 is a superscript two in Latin-1 -- and that one byte crashed every
derived-file generator with UnicodeDecodeError.

Latin-1 is the right decoder, and not as a guess about the code page: it maps
0x00-0xFF one to one onto the first 256 codepoints, so decode and encode round
trip byte for byte whatever the bytes actually meant.  A path is an identifier
here, to be carried and compared, never interpreted -- so preserving it exactly
is the whole requirement, and errors='replace' would silently corrupt the
identity of the row instead.

`out()` exists because of what the crash did downstream.  The generators print
to stdout and the caller redirects -- `rare.py > lvo-rare.tsv` -- so the shell
truncated the published file to zero bytes BEFORE python ran and failed.  The
traceback went to a log nobody was reading and the committed table became
empty.  Writing to a temporary and renaming only on success means a generator
that dies leaves the previous good table in place.

SPDX-License-Identifier: MIT
"""
import contextlib
import os
import sys
import tempfile

ENCODING = 'latin-1'


def lines(path):
    """Every line of a ledger-format file, decoded losslessly."""
    with open(path, encoding=ENCODING) as fh:
        for line in fh:
            yield line.rstrip('\n')


def rows(path, want_ok=True):
    """Ledger rows as split fields.

    Skips the header, short rows, and -- with want_ok -- anything whose verdict
    is not an OK variant.  `OK+SOCK_RAW` is an OK, which is why this is a
    prefix test and not equality; an exact match on 'OK' dropped every raw
    socket row once already.
    """
    for line in lines(path):
        f = line.split('\t')
        if len(f) < 6 or f[0] == 'archive':
            continue
        if want_ok and not f[2].startswith('OK'):
            continue
        yield f


@contextlib.contextmanager
def out(path=None):
    """Write a generated table, atomically, or to stdout if path is None."""
    if path is None:
        yield sys.stdout
        return
    d = os.path.dirname(os.path.abspath(path)) or '.'
    fd, tmp = tempfile.mkstemp(dir=d, suffix='.tmp')
    try:
        with os.fdopen(fd, 'w', encoding=ENCODING) as fh:
            yield fh
        os.replace(tmp, path)
    except BaseException:
        with contextlib.suppress(OSError):
            os.unlink(tmp)
        raise
