#!/usr/bin/env python3
"""Regression tests for every accepted __argv startup shape."""

import importlib.util
import pathlib
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "fix_toolchain_crt0", ROOT / "tools" / "fix-toolchain-crt0.py")
CRT0 = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CRT0)

START = "_____start"


def insn(offset, word, relocs, text):
    return [offset, word, relocs, text, START]


class ArgvContractGateTest(unittest.TestCase):
    def classify(self, instructions, location=(".bss", 0), backed=False):
        with mock.patch.object(CRT0, "instruction_details",
                               return_value=instructions), \
             mock.patch.object(CRT0, "symbol_location",
                               return_value=location), \
             mock.patch.object(CRT0, "_argv_has_static_backing",
                               return_value=backed):
            return CRT0.argv_init_sites("objdump", pathlib.Path("crt0.o"))

    def test_upstream_null_pointer_shape_is_buggy(self):
        instructions = [
            insn(0x2A, 0x2079, [".bss"],
                 "movea.l 0 0 ___argv,a0"),
            insn(0x30, 0x20B9, ["___commandline"],
                 "move.l 4 4 ___commandline,(a0)"),
            insn(0x8C, 0x2079, [".bss"],
                 "movea.l 0 0 ___argv,a0"),
            insn(0x92, 0x2080, [], "move.l d0,(a0)"),
        ]
        sites = self.classify(instructions)
        self.assertEqual([(old, new) for _, old, new in sites],
                         [(0x2079, 0x41F9), (0x2079, 0x41F9)])

    def test_baserel_displacements_are_decimal(self):
        self.assertEqual(CRT0._addend("move.l 16(a4),-(sp)"), 0x10)
        self.assertEqual(CRT0._addend("move.l (16,a4),-(sp)"), 0x10)
        self.assertEqual(CRT0._addend("move.l a4@(16),-(sp)"), 0x10)

    def test_repaired_address_load_shape_is_safe(self):
        instructions = [
            insn(0x2A, 0x41F9, [".bss"], "lea 0 0 ___argv,a0"),
            insn(0x30, 0x20B9, ["___commandline"],
                 "move.l 4 4 ___commandline,(a0)"),
            insn(0x8C, 0x41F9, [".bss"], "lea 0 0 ___argv,a0"),
            insn(0x92, 0x2080, [], "move.l d0,(a0)"),
        ]
        sites = self.classify(instructions)
        self.assertTrue(all(old == new for _, old, new in sites))
        self.assertEqual(len(sites), 2)

    def test_optimized_direct_stores_are_proven_per_object(self):
        instructions = [
            insn(0x3E, 0x296C, ["___commandline", ".bss"],
                 "move.l 0(a4),0(a4)"),
            # Loading argv for main is not an initializer.
            insn(0x56, 0x2F2C, [".bss"], "move.l 0(a4),-(sp)"),
            insn(0x8C, 0x2940, [".bss"], "move.l d0,0(a4)"),
        ]
        sites = self.classify(instructions)
        self.assertEqual([offset for offset, _, _ in sites], [0x3E, 0x8C])
        self.assertTrue(all(old == new for _, old, new in sites))

    def test_source_fix_with_relocated_backing_storage_is_safe(self):
        instructions = [
            insn(0x2A, 0x2079, [".data"],
                 "movea.l 0 0 ___argv,a0"),
            insn(0x30, 0x20B9, ["___commandline"],
                 "move.l 4 4 ___commandline,(a0)"),
            insn(0x8C, 0x2079, [".data"],
                 "movea.l 0 0 ___argv,a0"),
            insn(0x92, 0x2080, [], "move.l d0,(a0)"),
        ]
        sites = self.classify(instructions, location=(".data", 0), backed=True)
        self.assertEqual(len(sites), 2)
        self.assertTrue(all(old == new for _, old, new in sites))

    def test_parser_owned_source_fix_is_safe(self):
        with mock.patch.object(CRT0, "argv_init_sites", return_value=[]), \
             mock.patch.object(CRT0, "_parser_owns_argv", return_value=True):
            state, message = CRT0.repair_argv_init(
                "objdump", pathlib.Path("crt0.o"), True)
        self.assertEqual(state, "immune")
        self.assertIn("exclusively owns", message)

    def test_no_writes_without_parser_contract_fails_closed(self):
        with mock.patch.object(CRT0, "argv_init_sites", return_value=[]), \
             mock.patch.object(CRT0, "_parser_owns_argv", return_value=False):
            state, message = CRT0.repair_argv_init(
                "objdump", pathlib.Path("crt0.o"), True)
        self.assertEqual(state, "refused")
        self.assertIn("did not prove parser ownership", message)

    def test_parser_ownership_requires_only_the_main_argv_reference(self):
        details = [insn(0x58, 0x2F39, [".bss"],
                        "move.l 10 10 ___argv,-(sp)")]
        with mock.patch.object(CRT0, "symbol_location",
                               return_value=(".bss", 0x10)), \
             mock.patch.object(CRT0, "_has_undefined_symbol",
                               return_value=True), \
             mock.patch.object(CRT0, "argv_sites",
                               return_value=([(0x58, 0x2F39, 0x2F39)], 1)), \
             mock.patch.object(CRT0, "instruction_details",
                               return_value=details):
            self.assertTrue(CRT0._parser_owns_argv(
                "objdump", pathlib.Path("crt0.o")))

    def test_parser_ownership_rejects_missing_link_anchor(self):
        with mock.patch.object(CRT0, "symbol_location",
                               return_value=(".bss", 0x10)), \
             mock.patch.object(CRT0, "_has_undefined_symbol",
                               return_value=False):
            self.assertFalse(CRT0._parser_owns_argv(
                "objdump", pathlib.Path("crt0.o")))

    def test_unrecognized_partial_shape_fails_closed(self):
        with mock.patch.object(CRT0, "argv_init_sites",
                               return_value=[(0x2A, 0x41F9, 0x41F9)]):
            state, message = CRT0.repair_argv_init(
                "objdump", pathlib.Path("crt0.o"), True)
        self.assertEqual(state, "refused")
        self.assertIn("1 of 2", message)

    def test_old_array_call_indirection_is_still_rejected(self):
        with mock.patch.object(CRT0, "argv_sites",
                               return_value=([(0x4A, 0x4879, 0x2F39)], 1)):
            state, message = CRT0.repair_argv(
                "objdump", pathlib.Path("crt0.o"), True)
        self.assertEqual(state, "buggy")
        self.assertIn("push &__argv", message)

    def test_correct_argv_call_is_accepted(self):
        with mock.patch.object(CRT0, "argv_sites",
                               return_value=([(0x4A, 0x2F39, 0x2F39)], 1)):
            state, _ = CRT0.repair_argv(
                "objdump", pathlib.Path("crt0.o"), True)
        self.assertEqual(state, "immune")

    def test_static_backing_requires_a_loader_relocation(self):
        path = pathlib.Path("crt0.o")
        with mock.patch.object(CRT0, "section_relocations",
                               return_value={".data": [(0, ".bss")]}):
            self.assertTrue(CRT0._argv_has_static_backing(
                "objdump", path, (".data", 0)))
        with mock.patch.object(CRT0, "section_relocations",
                               return_value={".data": []}):
            self.assertFalse(CRT0._argv_has_static_backing(
                "objdump", path, (".data", 0)))


if __name__ == "__main__":
    unittest.main()
