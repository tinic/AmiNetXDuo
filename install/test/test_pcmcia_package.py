#!/usr/bin/env python3
"""Keep redistribution notices and complete source inseparable from the binaries.

SPDX-License-Identifier: MIT
"""

import importlib.util
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("pcmcia", ROOT / "tools/pcmcia-drivers.py")
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)


class PackageTests(unittest.TestCase):
    def test_complete_package(self):
        PACKAGE.verify()

    def test_missing_or_changed_redistribution_files_fail_closed(self):
        original = PACKAGE.VENDOR
        (ROOT / "build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="pcmcia-license-test-", dir=ROOT / "build") as directory:
            PACKAGE.VENDOR = Path(directory) / "PCMCIA"
            try:
                shutil.copytree(original, PACKAGE.VENDOR)
                for rel in ("Docs/GPL", "Docs/LGPL", "Docs/Hermes-LICENSE",
                            "Sources/prism2v2.lha", "Sources/xircomce2.lha", "Sources/pccard.lha"):
                    with self.subTest(file=rel):
                        path = PACKAGE.VENDOR / rel
                        data = path.read_bytes()
                        path.unlink()
                        with self.assertRaises(FileNotFoundError):
                            PACKAGE.verify()
                        path.write_bytes(data + b"changed")
                        with self.assertRaises(ValueError):
                            PACKAGE.verify()
                        path.write_bytes(data)
                PACKAGE.verify()
            finally:
                PACKAGE.VENDOR = original


class InstallerPackingTests(unittest.TestCase):
    def test_quotes_and_diagnostic_lines_survive_packing(self):
        sys.path.insert(0, str(ROOT / "install/tools"))
        try:
            from packscript import compact
            sample = '; comment\n  (set A "keep; this \\\"quote\\\"") ; drop\n(set B \'also; keep\')\n'
            self.assertEqual(compact(sample), '\n(set A "keep; this \\\"quote\\\"")\n(set B \'also; keep\')\n')
            source = (ROOT / "install/Install-AmiNetXDuo").read_text()
            packed = compact(source)
            self.assertLess(len(packed), len(source))
            self.assertEqual(packed.count("\n"), source.count("\n"))
        finally:
            sys.path.pop(0)


if __name__ == "__main__":
    unittest.main()
