#!/usr/bin/env python3
"""Unit checks for prof-report.py's symbol attribution.

WHY THIS EXISTS.  Under -ffunction-sections one object contributes SEVERAL
.text sections at several output addresses, and nm reports every symbol at
offset 0 -- of its OWN section.  Adding `addr + value` for each symbol at each
contribution therefore put every symbol of the object at every one of its
addresses, and the report handed out whichever name sorted last.

The measured consequence: _nx_tcp_socket_state_data_check runs on every
received segment, and the profile named _nx_tcp_socket_state_data_trim_front
instead, at 2.4-2.6%, leaving data_check out of the table entirely.  Reading
that as "the peer is sending duplicate data" survived until the stack's own
counters said 0 retransmitted, 0 dropped, 0 out of order.

ld already wrote which function each contribution is.  ".text._nx_tcp_..." is
not a hint, it is the name.  These checks hold the parser to using it.

Hermetic: a synthetic map and a stubbed nm, so it needs no toolchain, no
build and no emulator.
"""

import importlib.util
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_spec = importlib.util.spec_from_file_location(
    "prof_report", os.path.join(ROOT, "tools", "prof-report.py"))
prof = importlib.util.module_from_spec(_spec)
sys.modules["prof_report"] = prof
_spec.loader.exec_module(prof)


# Three functions out of ONE object, exactly as ld lays them out, and exactly
# the trio that produced the wrong answer on the real image.
MAP = """
 .text._nx_tcp_socket_state_data_trim
                0x000000000004cd64       0x64 ../../libnetxduo.a(nx_tcp.c.obj)
                0x000000000004cd64                _nx_tcp_socket_state_data_trim
 .text._nx_tcp_socket_state_data_trim_front
                0x000000000004cdc8       0xa8 ../../libnetxduo.a(nx_tcp.c.obj)
                0x000000000004cdc8                _nx_tcp_socket_state_data_trim_front
 .text._nx_tcp_socket_state_data_check
                0x000000000004ce70      0x534 ../../libnetxduo.a(nx_tcp.c.obj)
                0x000000000004ce70                _nx_tcp_socket_state_data_check
"""

# What nm says about that object: every one of them at offset zero, which is
# the whole trap.  The leading underscore is the assembler's, on top of the C
# name's own, so these carry two and the section names carry one.
NM_SYMS = [
    (0, "T", "__nx_tcp_socket_state_data_trim"),
    (0, "T", "__nx_tcp_socket_state_data_trim_front"),
    (0, "T", "__nx_tcp_socket_state_data_check"),
]


class SectionAttributionTests(unittest.TestCase):
    def setUp(self):
        self._real_nm = prof.nm_symbols
        prof.nm_symbols = lambda nm, path, member=None: list(NM_SYMS)
        self.tmp = tempfile.mkdtemp()
        self.map = os.path.join(self.tmp, "t.map")
        with open(self.map, "w") as fh:
            fh.write(MAP)
        # build_symbol_table checks the object exists before asking nm.
        os.makedirs(os.path.join(self.tmp, "../.."), exist_ok=True)
        self.objdir = self.tmp
        open(os.path.join(self.tmp, "libnetxduo.a"), "w").close()

    def tearDown(self):
        prof.nm_symbols = self._real_nm

    def table(self):
        mp = MAP.replace("../../libnetxduo.a", "libnetxduo.a")
        with open(self.map, "w") as fh:
            fh.write(mp)
        return prof.build_symbol_table("nm", self.map, self.objdir)[".text"]

    def test_the_section_name_picks_the_symbol(self):
        got = {addr: name for addr, name, _mod in self.table()}
        self.assertEqual(got.get(0x4cd64), "__nx_tcp_socket_state_data_trim")
        self.assertEqual(got.get(0x4cdc8),
                         "__nx_tcp_socket_state_data_trim_front")
        self.assertEqual(got.get(0x4ce70),
                         "__nx_tcp_socket_state_data_check")

    def test_no_symbol_is_joined(self):
        """A '|' here is the report saying it cannot tell them apart."""
        joined = [n for _a, n, _m in self.table() if "|" in n]
        self.assertEqual(joined, [])

    def test_data_check_is_not_swallowed(self):
        """The regression itself: data_check must appear, at its own address.

        Before the fix every name landed at all three addresses and the last
        one sorted won, so the function that runs on every received segment
        was absent from the report."""
        names = [n for _a, n, _m in self.table()]
        self.assertIn("__nx_tcp_socket_state_data_check", names)

    def test_parse_map_reports_the_section_suffix(self):
        rows = prof.parse_map(self.map)
        self.assertEqual([r[1] for r in rows],
                         ["_nx_tcp_socket_state_data_trim",
                          "_nx_tcp_socket_state_data_trim_front",
                          "_nx_tcp_socket_state_data_check"])

    def test_a_bare_text_section_still_parses(self):
        """-ffunction-sections is not on everywhere; the plain form must keep
        working, and it has no single symbol to name."""
        p = os.path.join(self.tmp, "bare.map")
        with open(p, "w") as fh:
            fh.write(" .text          0x0000000000001000       0x40 "
                     "libnetxduo.a(nx_tcp.c.obj)\n")
        rows = prof.parse_map(p)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0][0], ".text")
        self.assertIsNone(rows[0][1])


if __name__ == "__main__":
    unittest.main()
