#!/usr/bin/env python3
"""Unit checks for the Roadshow RX-gap harness: rsgap_verdict.py's three
outcomes, the null control and the input rules on synthetic boots, and
rsgappeer.py's loopback selftest.  No emulator, no LAN."""

import io
import os
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from rsgap_verdict import classify, parse, run  # noqa: E402


def boots(values):
    """rsgap_boot lines in R/A/A/R order; values is [(R, A), ...]."""
    out, n = [], 0
    for i, (r, a) in enumerate(values):
        for arm in ("RA" if i % 2 == 0 else "AR"):
            n += 1
            v = r if arm == "R" else a
            out.append("rsgap_boot boot=%d arm=%s mode=measure valid=1 "
                       "mean_kbps=%.2f" % (n, arm, v))
    return out


def verdict_of(lines, **kw):
    buf = io.StringIO()
    rc = run(lines, out=buf, **kw)
    last = [l for l in buf.getvalue().splitlines() if "verdict=" in l][-1]
    kv = dict(t.split("=", 1) for t in last.split()[1:])
    return rc, kv, buf.getvalue()


class ClassifyTests(unittest.TestCase):
    def test_rule_edges(self):
        self.assertEqual(classify(1.6, 3.0, 1.5), ("gap", "roadshow_faster"))
        self.assertEqual(classify(-3.0, -1.6, 1.5), ("gap", "aminetxduo_faster"))
        self.assertEqual(classify(-1.5, 1.5, 1.5), ("no_material_gap", "none"))
        # Excludes zero but reaches inside the margin: not a gap.
        self.assertEqual(classify(0.5, 2.0, 1.5), ("inconclusive", "none"))
        # Contains zero but reaches past the margin: not equivalence.
        self.assertEqual(classify(-0.5, 2.0, 1.5), ("inconclusive", "none"))


class VerdictTests(unittest.TestCase):
    def test_gap(self):
        # Roadshow ~3.4% faster every pair, small scatter.
        pairs = [(949 + d, 918 - d) for d in (0.0, 1.0, -1.0, 0.5, -0.5,
                                              0.8, -0.8, 0.2, -0.2, 0.0)]
        rc, kv, _ = verdict_of(boots(pairs))
        self.assertEqual(rc, 0)
        self.assertEqual(kv["verdict"], "gap")
        self.assertEqual(kv["direction"], "roadshow_faster")
        self.assertGreater(float(kv["ci_lo_pct"]), 1.5)

    def test_no_material_gap(self):
        pairs = [(930 + d, 930 - d) for d in (0.5, -0.5, 1.0, -1.0, 0.2,
                                              -0.2, 0.7, -0.7, 0.0, 0.3)]
        rc, kv, _ = verdict_of(boots(pairs))
        self.assertEqual(rc, 0)
        self.assertEqual(kv["verdict"], "no_material_gap")
        self.assertGreaterEqual(float(kv["ci_lo_pct"]), -1.5)
        self.assertLessEqual(float(kv["ci_hi_pct"]), 1.5)

    def test_inconclusive_wide(self):
        # Mean near +1%, scatter of several per cent: straddles the margin.
        pairs = [(930 + d, 921) for d in (40, -30, 25, -20, 35, -25)]
        rc, kv, _ = verdict_of(boots(pairs))
        self.assertEqual(rc, 0)
        self.assertEqual(kv["verdict"], "inconclusive")

    def test_too_few_pairs(self):
        rc, kv, _ = verdict_of(boots([(949, 918)]))
        self.assertEqual(kv["verdict"], "inconclusive")
        self.assertEqual(kv["reason"], "too_few_pairs")

    def test_alternation_pairs_neighbours(self):
        lines = boots([(100, 90), (110, 99)])
        self.assertEqual([b[1] for b in parse(lines)], list("RAAR"))
        _, _, text = verdict_of(lines)
        # (100-90) and (110-99) over A's median 94.5.
        self.assertIn("diffs_pct=10.582,11.640", text)

    def test_invalid_and_door_boots_are_ignored(self):
        lines = boots([(949, 918), (949, 918)]) + [
            "rsgap_boot boot=9 arm=R mode=measure valid=0 mean_kbps=1",
            "rsgap_boot boot=10 arm=A mode=door valid=1 mean_kbps=1",
            "noise line",
        ]
        self.assertEqual(len(parse(lines)), 4)

    def test_no_input(self):
        rc, kv, _ = verdict_of(["nothing here"])
        self.assertEqual(rc, 2)
        self.assertEqual(kv["verdict"], "none")

    def test_null_control(self):
        lines = ["rsgap_boot boot=%d arm=A mode=measure valid=1 mean_kbps=%s"
                 % (i + 1, v) for i, v in enumerate([920, 925, 918, 921])]
        rc, kv, text = verdict_of(lines, null_arm="A")
        self.assertEqual(rc, 0)
        self.assertEqual(kv["verdict"], "null_control")
        self.assertIn("pairs=2", text)

    def test_seeded(self):
        lines = boots([(949, 918), (940, 925), (951, 915)])
        _, a, _ = verdict_of(lines, seed=7)
        _, b, _ = verdict_of(lines, seed=7)
        self.assertEqual(a, b)


class PeerTests(unittest.TestCase):
    def test_peer_selftest(self):
        here = os.path.dirname(os.path.abspath(__file__))
        p = subprocess.run([sys.executable, os.path.join(here, "rsgappeer.py"),
                            "selftest"], capture_output=True, text=True,
                           timeout=60)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("rsgappeer_selftest=PASS", p.stdout)


if __name__ == "__main__":
    unittest.main()
