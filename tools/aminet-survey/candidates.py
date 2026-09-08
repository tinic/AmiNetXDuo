"""Rank surveyed applications by what they would ADD to the test harness.

tests/HARNESSES drives our own tools, LhA and a Workbench install; it contains
NO third-party network application, so every entry here is net-new coverage.

Ranking is a greedy set cover over the 143 bsdsocket vectors: repeatedly take
the binary that exercises the most vectors nothing chosen so far exercises.
That answers "which handful of programs covers the most API", which is the
question a harness cares about -- not "which program is biggest".

Flags a candidate carries:
  H  socket handoff (ObtainSocket/ReleaseSocket/ReleaseCopyOfSocket/Dup2Socket)
  R  SOCK_RAW -- breaks under AMINETXDUO_RAWSOCKET=OFF with no missing LVO
  D  gethostby* -- degrades to hosts-file only under the micro profile

DUAL_STACK_AS225 binaries are excluded: their LVO set mixes two libraries, so
adding one to the harness would test a claim that is not true of it.
"""
import sys

MICRO_HANDOFF = {'ObtainSocket', 'ReleaseSocket', 'ReleaseCopyOfSocket', 'Dup2Socket'}

rows = []
for line in open('/home/turo/anxd-aminet/results.tsv'):
    f = line.rstrip('\n').split('\t')
    if len(f) < 6 or f[0] == 'archive':
        continue
    archive, path, verdict, _d, calls, lvos = f[0], f[1], f[2], f[3], f[4], f[5]
    if not verdict.startswith('OK'):
        continue                      # excludes DUAL_STACK_AS225 by design
    s = set(x for x in lvos.split(',') if x and not x.startswith('?'))
    if not s:
        continue
    rows.append((archive, path, s, 'SOCK_RAW' in verdict, int(calls.split('=')[1])))

covered = set()
order = []
pool = list(rows)
while pool:
    pool.sort(key=lambda r: (-len(r[2] - covered), -r[4]))
    best = pool.pop(0)
    gain = best[2] - covered
    if not gain:
        break
    covered |= gain
    order.append((best, len(gain)))

print("rank\tnew\tcum\tflags\tlvos\tarchive\tbinary")
cum = 0
for rank, ((archive, path, s, raw, calls), gain) in enumerate(order, 1):
    cum += gain
    flags = ''.join([
        'H' if s & MICRO_HANDOFF else '-',
        'R' if raw else '-',
        'D' if any(x.startswith('gethostby') for x in s) else '-',
    ])
    print(f"{rank}\t{gain}\t{cum}\t{flags}\t{len(s)}\t{archive}\t{path}")
print(f"\n# {len(rows)} attributed binaries cover {len(covered)} of 143 vectors "
      f"({100*len(covered)//143}%); {len(order)} of them are non-redundant.")
