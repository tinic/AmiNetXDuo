#!/usr/bin/env python3
"""Unit checks for stallgaps.py's ACK-clock attribution."""

import os
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lossrate import ACK  # noqa: E402
from stallgaps import ack_clock_gaps, progress, useful_acks  # noqa: E402


PORT = 17712
EPH = 49152


def seg(t, sport, seq=0, ack=0, win=1000, plen=0, flags=ACK):
    return SimpleNamespace(t=t, sport=sport, seq=seq, ack=ack, win=win,
                           plen=plen, flags=flags)


class AckClockTests(unittest.TestCase):
    def test_ack_releases_each_sender_gap(self):
        ss = [
            seg(0.000, PORT, seq=100, plen=100),
            seg(0.009, EPH, ack=200),
            seg(0.010, PORT, seq=200, plen=100),
            seg(0.019, EPH, ack=300),
            seg(0.020, PORT, seq=300, plen=100),
        ]
        span = (0.0, 0.020, True)
        prog = progress(ss, PORT, span, True)
        acks = useful_acks(ss, PORT, span, True)
        got = ack_clock_gaps(prog, acks, 0.005, 0.002)

        self.assertEqual(len(got), 2)
        self.assertTrue(all(r[3] for r in got))
        self.assertAlmostEqual(got[0][1], 0.009)
        self.assertAlmostEqual(got[0][2], 0.001)
        self.assertEqual([a[3] for a in acks], [0, 100])

    def test_window_update_is_useful_without_ack_advance(self):
        ss = [
            seg(0.000, EPH, ack=100, win=100),
            seg(0.005, EPH, ack=100, win=100),  # says nothing new
            seg(0.010, EPH, ack=100, win=200),  # opens the window
        ]
        got = useful_acks(ss, PORT, (0.0, 0.020, True), True)
        self.assertEqual([(a[0], a[2]) for a in got],
                         [(0.000, 100), (0.010, 200)])

    def test_gap_without_ack_stays_unattributed(self):
        prog = [(0.0, 100), (0.010, 100)]
        got = ack_clock_gaps(prog, [], 0.005, 0.002)
        self.assertEqual(got, [(0.010, None, None, False, None)])

    def test_ack_wrap_is_forward_progress(self):
        ss = [
            seg(0.000, EPH, ack=0xfffffff0),
            seg(0.010, EPH, ack=0x00000020),
        ]
        got = useful_acks(ss, PORT, (0.0, 0.020, True), True)
        self.assertEqual(len(got), 2)
        self.assertEqual(got[1][3], 0x30)


if __name__ == "__main__":
    unittest.main()
