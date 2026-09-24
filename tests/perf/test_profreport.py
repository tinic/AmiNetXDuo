#!/usr/bin/env python3
"""Unit checks for profreport.py's section attribution.

profreport.py had the same collision prof-report.py's test guards, and it had
its own take on the same bug: where prof-report.py once matched only the bare
".text", profreport.py COLLAPSED every ".text.<name>" to ".text" (base_section)
before it could be used.  Either way, under -ffunction-sections one object
contributes several .text sections at several output addresses, and nm reports
every symbol at offset 0 -- of its OWN section.  Adding `addr + 0` for each
symbol at each contribution therefore put every symbol of the object at every
one of its addresses, and the report handed out whichever name sorted last.

Two functions out of ONE object, both at nm offset 0, is the smallest case
that shows it: foo at 0x1000 and bar at 0x2000.  Before the fix both names
land at both addresses; after it .text.foo owns 0x1000 and .text.bar owns
0x2000, because ld wrote the name in the section header.

Hermetic: a synthetic map and a stubbed nm, no toolchain, no build, no
emulator.
"""

import importlib.util
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_spec = importlib.util.spec_from_file_location(
    "profreport", os.path.join(ROOT, "tools", "profiler", "profreport.py"))
prof = importlib.util.module_from_spec(_spec)
sys.modules["profreport"] = prof
_spec.loader.exec_module(prof)


# Two functions out of ONE object, exactly as ld lays them out, and both nm
# values zero -- which is the whole trap.
MAP = """
 .text.foo
                0x0000000000001000       0x40 single.o
                0x0000000000001000                _foo
 .text.bar
                0x0000000000002000       0x40 single.o
                0x0000000000002000                _bar
"""

# What nm says about that object: every one of them at offset zero.
NM_SYMS = [
    (0, "T", "_foo"),
    (0, "T", "_bar"),
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
        self.objdir = self.tmp
        open(os.path.join(self.tmp, "single.o"), "w").close()

    def tearDown(self):
        prof.nm_symbols = self._real_nm

    def table(self):
        rows = prof.build_symbol_table("nm", self.map, self.objdir)[".text"]
        # The [module] marker row is real bookkeeping, not a symbol.
        return [(a, n, m) for a, n, m in rows if not n.startswith("[")]

    def test_the_section_name_picks_the_symbol(self):
        got = {addr: name for addr, name, _mod in self.table()}
        self.assertEqual(got.get(0x1000), "_foo")
        self.assertEqual(got.get(0x2000), "_bar")

    def test_no_name_at_the_other_address(self):
        """Before the fix both names landed at both addresses."""
        got = {addr: name for addr, name, _mod in self.table()}
        self.assertNotEqual(got.get(0x1000), "_bar")
        self.assertNotEqual(got.get(0x2000), "_foo")

    def test_parse_map_reports_the_section_suffix(self):
        rows = prof.parse_map(self.map)
        self.assertEqual([r[1] for r in rows], ["foo", "bar"])
        self.assertEqual([r[0] for r in rows], [".text", ".text"])

    def test_a_bare_text_section_still_parses(self):
        """-ffunction-sections is not on everywhere; the plain form must keep
        working, and it has no single symbol to name."""
        p = os.path.join(self.tmp, "bare.map")
        with open(p, "w") as fh:
            fh.write(" .text          0x0000000000001000       0x40 "
                     "single.o\n")
        rows = prof.parse_map(p)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0][0], ".text")
        self.assertIsNone(rows[0][1])


if __name__ == "__main__":
    unittest.main()
