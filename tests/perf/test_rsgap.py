#!/usr/bin/env python3
"""Unit checks for the Roadshow RX-gap harness: rsgap_verdict.py's three
outcomes, the sample floor, pairing and the null control on synthetic boots;
rsgap_collect.py's validity rules on synthetic results directories; and
rsgappeer.py's loopback selftest.  No emulator, no LAN."""

import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from rsgap_verdict import classify, parse, run  # noqa: E402


HERE = os.path.dirname(os.path.abspath(__file__))


def boots(values, invalid=(), drop=(), expected_ok=1):
    """rsgap_boot lines in R/A/A/R order, as the collector pairs them, and
    the run record; values is [(R, A), ...].  Boot numbers in invalid are
    written valid=0, those in drop are not written."""
    out, n = [], 0
    for i, (r, a) in enumerate(values):
        for arm in ("RA" if i % 2 == 0 else "AR"):
            n += 1
            if n in drop:
                continue
            v = r if arm == "R" else a
            out.append("rsgap_boot boot=%d arm=%s mode=measure pos=%d pair=%d "
                       "mean_kbps=%.2f valid=%d"
                       % (n, arm, n, i + 1, v, 0 if n in invalid else 1))
    last = max([int(l.split()[1].split("=")[1]) for l in out] or [0])
    missing = ",".join(str(p) for p in range(last + 1, 2 * len(values) + 1))
    out.append("rsgap_run boots=%d measure=%d door=1 scheduled=%d fault_boot=0 "
               "pairs_scheduled=%d missing_pos=%s expected_ok=%d"
               % (len(out) + 1, len(out), 2 * len(values), len(values),
                  missing or "-", expected_ok))
    return out


def null_boots(values):
    out = ["rsgap_boot boot=%d arm=A mode=measure pos=%d pair=%d valid=1 "
           "mean_kbps=%s" % (i + 1, i + 1, i // 2 + 1, v)
           for i, v in enumerate(values)]
    out.append("rsgap_run scheduled=%d pairs_scheduled=%d missing_pos=- "
               "expected_ok=1" % (len(values), len(values) // 2))
    return out


GAP = [(949 + d, 918 - d) for d in (0.0, 1.0, -1.0, 0.5, -0.5,
                                    0.8, -0.8, 0.2, -0.2, 0.0)] * 2
EVEN = [(930 + d, 930 - d) for d in (0.5, -0.5, 1.0, -1.0, 0.2,
                                     -0.2, 0.7, -0.7, 0.0, 0.3)] * 2


def last_verdict(text):
    last = [l for l in text.splitlines() if "verdict=" in l][-1]
    return dict(t.split("=", 1) for t in last.split()[1:])


def verdict_of(lines, **kw):
    buf = io.StringIO()
    rc = run(lines, out=buf, **kw)
    return rc, last_verdict(buf.getvalue()), buf.getvalue()


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
        rc, kv, _ = verdict_of(boots(GAP))
        self.assertEqual(rc, 0)
        self.assertEqual(kv["verdict"], "gap")
        self.assertEqual(kv["direction"], "roadshow_faster")
        self.assertGreater(float(kv["ci_lo_pct"]), 1.5)

    def test_no_material_gap(self):
        rc, kv, _ = verdict_of(boots(EVEN))
        self.assertEqual(rc, 0)
        self.assertEqual(kv["verdict"], "no_material_gap")
        self.assertGreaterEqual(float(kv["ci_lo_pct"]), -1.5)
        self.assertLessEqual(float(kv["ci_hi_pct"]), 1.5)

    def test_inconclusive_wide(self):
        # Mean near +1%, scatter of several per cent: straddles the margin.
        pairs = [(930 + d, 921) for d in (40, -30, 25, -20, 35, -25)]
        rc, kv, _ = verdict_of(boots(pairs), min_pairs=1)
        self.assertEqual(rc, 0)
        self.assertEqual(kv["verdict"], "inconclusive")
        self.assertNotIn("reason", kv)

    def test_nineteen_pairs_is_too_few(self):
        rc, kv, text = verdict_of(boots(GAP[:19]))
        self.assertEqual(rc, 0)
        self.assertEqual(kv, {"verdict": "inconclusive",
                              "reason": "too_few_pairs", "pairs": "19",
                              "min_pairs": "20"})
        self.assertIn("dry_run_classification=gap direction=roadshow_faster",
                      text)
        self.assertNotIn("verdict=gap", text)

    def test_twenty_pairs_is_enough(self):
        rc, kv, text = verdict_of(boots(GAP[:20]))
        self.assertEqual(kv["verdict"], "gap")
        self.assertNotIn("dry_run_classification", text)

    def test_cli_default_min_pairs(self):
        with tempfile.NamedTemporaryFile("w", suffix=".kv",
                                         delete=False) as fh:
            fh.write("\n".join(boots(GAP[:2])) + "\n")
        try:
            p = subprocess.run([sys.executable,
                                os.path.join(HERE, "rsgap_verdict.py"),
                                fh.name], capture_output=True, text=True,
                               timeout=60)
        finally:
            os.unlink(fh.name)
        self.assertEqual(p.returncode, 0, p.stderr)
        kv = last_verdict(p.stdout)
        self.assertEqual(kv["verdict"], "inconclusive")
        self.assertEqual(kv["reason"], "too_few_pairs")
        self.assertEqual(kv["min_pairs"], "20")
        self.assertIn("dry_run_classification=gap", p.stdout)

    def test_alternation_pairs_neighbours(self):
        lines = boots([(100, 90), (110, 99)])
        self.assertEqual([b["arm"] for b in parse(lines)[0]], list("RAAR"))
        _, _, text = verdict_of(lines)
        # (100-90) and (110-99) over A's median 94.5.
        self.assertIn("diffs_pct=10.582,11.640", text)
        self.assertIn("pair_boots=1-2,3-4", text)

    def test_invalid_middle_boot(self):
        # Boot 3 of R,A,A,R,R,A,... invalid: no re-pairing of 2 with 4.
        rc, kv, text = verdict_of(boots(GAP, invalid=(3,)))
        self.assertEqual(rc, 0)
        self.assertEqual(kv["verdict"], "inconclusive")
        self.assertEqual(kv["reason"], "invalid_or_missing_boot")
        self.assertEqual(kv["boots"], "3")
        self.assertEqual(kv["incomplete_pairs"], "-")
        self.assertNotIn("diffs_pct", text)
        self.assertNotIn("2-4", text)

    def test_missing_boot(self):
        rc, kv, text = verdict_of(boots(GAP, drop=(3,)))
        self.assertEqual(kv["verdict"], "inconclusive")
        self.assertEqual(kv["reason"], "invalid_or_missing_boot")
        self.assertEqual(kv["incomplete_pairs"], "2")
        self.assertNotIn("diffs_pct", text)

    def test_missing_tail(self):
        rc, kv, _ = verdict_of(boots(GAP, drop=(39, 40)))
        self.assertEqual(kv["reason"], "invalid_or_missing_boot")
        self.assertEqual(kv["missing_pos"], "39,40")

    def test_run_not_ok(self):
        _, kv, _ = verdict_of(boots(GAP, expected_ok=0))
        self.assertEqual(kv["verdict"], "inconclusive")
        self.assertEqual(kv["reason"], "invalid_or_missing_boot")

    def test_no_run_record(self):
        rc, kv, _ = verdict_of(boots(GAP)[:-1])
        self.assertEqual(rc, 2)
        self.assertEqual(kv["reason"], "no_run_record")

    def test_door_boots_are_ignored(self):
        lines = boots([(949, 918), (949, 918)]) + [
            "rsgap_boot boot=10 arm=A mode=door valid=1 mean_kbps=1",
            "noise line",
        ]
        self.assertEqual(len(parse(lines)[0]), 4)

    def test_no_input(self):
        rc, kv, _ = verdict_of(["nothing here"])
        self.assertEqual(rc, 2)
        self.assertEqual(kv["verdict"], "none")

    def test_null_control(self):
        rc, kv, text = verdict_of(null_boots([920, 925, 918, 921]),
                                  null_arm="A")
        self.assertEqual(rc, 0)
        self.assertEqual(kv["verdict"], "null_control")
        self.assertIn("pairs=2", text)

    def test_null_control_invalid_boot(self):
        lines = null_boots([920, 925, 918, 921])
        lines[1] = lines[1].replace("valid=1", "valid=0")
        _, kv, text = verdict_of(lines, null_arm="A")
        self.assertEqual(kv["verdict"], "inconclusive")
        self.assertEqual(kv["boots"], "2")
        self.assertNotIn("verdict=null_control", text)

    def test_seeded(self):
        lines = boots([(949, 918), (940, 925), (951, 915)])
        _, a, _ = verdict_of(lines, seed=7)
        _, b, _ = verdict_of(lines, seed=7)
        self.assertEqual(a, b)


# ------------------------------------------------------------- collector --

IMAGES = {  # (arm, mode, guest) -> md5
    ("R", "*", "RSGAPARM:libs/bsdsocket.library"): "11" * 16,
    ("A", "*", "RSGAPARM:libs/bsdsocket.library"): "22" * 16,
    ("A", "door", "RSGAPARM:C/httpd"): "33" * 16,
    ("*", "*", "C:RsGapBoot"): "44" * 16,
}


def md5_lines(arm, mode):
    return ["rec=md5 file=%s md5=%s size=1" % (g, m)
            for (a, mo, g), m in IMAGES.items()
            if a in (arm, "*") and mo in (mode, "*")]


class Session:
    """A synthetic run-rsgap.sh results directory: R and A measurement boots
    then an A door boot, one 100-byte transfer each."""

    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="rsgap-test-")
        self.boot = {1: self.measure(1, "R"), 2: self.measure(2, "A"),
                     3: self.door(3)}

    @staticmethod
    def select(n, arm, mode):
        present = mode == "measure"
        return ("rec=select boot=%d arm=%s mode=%s selector=%s consumed=%d "
                "reason=none bsdsocket_preloaded=0 driver_preloaded=0"
                % (n, arm, mode, "present" if present else "absent",
                   1 if present else 0))

    def measure(self, n, arm):
        tasks = "AmiNetXDuo_stack" if arm == "A" else "Workbench"
        return [self.select(n, arm, "measure"),
                "rec=stacklib count=1 id=x", "rec=tasks names=%s" % tasks,
                "rec=ports names=-"] + md5_lines(arm, "measure") + [
                "rec=xfer tag=1 result=ok rx_bytes=100 rx_kbps=900 "
                "local=10.0.0.%d:1024" % n]

    def door(self, n):
        return [self.select(n, "A", "door"), "rec=stacklib count=1 id=x",
                "rec=tasks names=AmiNetXDuo_stack", "rec=ports names=-"] + \
            md5_lines("A", "door") + [
                "rec=xfer tag=door result=ok rx_bytes=4096 rx_kbps=1 "
                "local=10.0.0.%d:1025" % n]

    def collect(self):
        for n, lines in self.boot.items():
            name = "cur" if n == 3 else "boot-%d" % n
            with open(os.path.join(self.dir, name + ".kv"), "w") as fh:
                fh.write("\n".join(lines) + "\n")
        with open(os.path.join(self.dir, "cur.wd"), "w") as fh:
            fh.write("rec=wd state=cancelled via=file elapsed_s=7\n")
        with open(os.path.join(self.dir, "images.kv"), "w") as fh:
            for (a, mo, g), m in IMAGES.items():
                fh.write("rsgap_image arm=%s mode=%s guest=%s md5=%s\n"
                         % (a, mo, g, m))
        with open(os.path.join(self.dir, "peer.log"), "w") as fh:
            for n in (1, 2):
                fh.write("event=conn kind=data client=10.0.0.%d:1024 "
                         "bytes=100\n" % n)
            fh.write("event=conn kind=door client=10.0.0.3:1025 bytes=4096\n")
        with open(os.path.join(self.dir, "door.kv"), "w") as fh:
            fh.write("door=up cancel_confirmed=1\n")
        j = lambda f: os.path.join(self.dir, f)  # noqa: E731
        p = subprocess.run(
            [sys.executable, os.path.join(HERE, "rsgap_collect.py"),
             "--results", self.dir, "--images", j("images.kv"),
             "--peer-log", j("peer.log"), "--door", j("door.kv"),
             "--schedule", "R,A", "--transfers", "1", "--bytes", "100",
             "--wd-secs", "150"], capture_output=True, text=True, timeout=60)
        shutil.rmtree(self.dir)
        recs = {}
        for line in p.stdout.splitlines():
            kv = dict(t.split("=", 1) for t in line.split()[1:] if "=" in t)
            recs[kv.get("boot", line.split()[0])] = kv
        return p.returncode, recs


class CollectTests(unittest.TestCase):
    def check(self, edit, boot="2", **want):
        s = Session()
        edit(s)
        rc, recs = s.collect()
        for k, v in want.items():
            self.assertEqual(recs[boot][k], v, (k, recs[boot]))
        if want.get("valid") == "0":
            self.assertEqual(rc, 1)
            self.assertEqual(recs["rsgap_run"]["expected_ok"], "0")
        return rc, recs

    def test_complete_inventory(self):
        rc, recs = self.check(lambda s: None, valid="1", hash_match="1",
                              hashes_expected="2", hash_missing="0")
        self.assertEqual(recs["1"]["valid"], "1")
        self.assertEqual(recs["1"]["pair"], "1")
        self.assertEqual(recs["2"]["pair"], "1")
        self.assertEqual(recs["3"]["valid"], "1")
        self.assertEqual(recs["3"]["hashes_expected"], "3")
        self.assertEqual(recs["rsgap_run"]["expected_ok"], "1")
        self.assertEqual(rc, 0)

    def drop_md5(self, s, n, guest):
        s.boot[n] = [l for l in s.boot[n] if "file=%s " % guest not in l]

    def test_missing_hash(self):
        self.check(lambda s: self.drop_md5(s, 2, "C:RsGapBoot"),
                   valid="0", hash_match="0", hash_missing="1")

    def test_duplicate_hash(self):
        self.check(lambda s: s.boot[2].append(md5_lines("A", "measure")[0]),
                   valid="0", hash_match="0", hash_dup="1")

    def test_unexpected_hash(self):
        # httpd belongs to the door boot's set, not a measure boot's.
        self.check(lambda s: s.boot[2].append(
            "rec=md5 file=RSGAPARM:C/httpd md5=%s size=1" % ("33" * 16)),
            valid="0", hash_match="0", hash_unexpected="1")

    def test_wrong_md5(self):
        def edit(s):
            s.boot[1] = [l.replace("11" * 16, "22" * 16) for l in s.boot[1]]
        self.check(edit, boot="1", valid="0", hash_match="0", hash_bad="1")

    def test_missing_file_on_guest(self):
        def edit(s):
            s.boot[2] = [l.replace("22" * 16, "missing") for l in s.boot[2]]
        self.check(edit, valid="0", hash_match="0", hash_bad="1")

    def test_selector_not_consumed(self):
        def edit(s):
            s.boot[2][0] = s.boot[2][0].replace("consumed=1", "consumed=0")
        self.check(edit, valid="0", hash_match="1")

    def test_bsdsocket_preloaded(self):
        def edit(s):
            s.boot[2][0] = s.boot[2][0].replace("bsdsocket_preloaded=0",
                                                "bsdsocket_preloaded=1")
        self.check(edit, valid="0")

    def test_driver_preloaded(self):
        def edit(s):
            s.boot[1][0] = s.boot[1][0].replace("driver_preloaded=0",
                                                "driver_preloaded=1")
        self.check(edit, boot="1", valid="0")

    def test_door_missing_httpd(self):
        self.check(lambda s: self.drop_md5(s, 3, "RSGAPARM:C/httpd"),
                   boot="3", valid="0", hash_match="0", hash_missing="1")


class PeerTests(unittest.TestCase):
    def test_peer_selftest(self):
        p = subprocess.run([sys.executable, os.path.join(HERE, "rsgappeer.py"),
                            "selftest"], capture_output=True, text=True,
                           timeout=60)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("rsgappeer_selftest=PASS", p.stdout)


if __name__ == "__main__":
    unittest.main()
