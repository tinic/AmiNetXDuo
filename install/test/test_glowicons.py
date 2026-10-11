#!/usr/bin/env python3
"""Validate shipping GlowIcons, classic fallback and malformed IFF/RLE input.

SPDX-License-Identifier: MIT
"""

import contextlib
import importlib.util
import io
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("showicon", ROOT / "install/tools/showicon.py")
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)


class GlowIcons(unittest.TestCase):
    def test_shipping_and_classic(self):
        # Keep test scratch under the repository, including when TMPDIR points
        # elsewhere. Test the actual CLI paths used by packaging.
        (ROOT / "build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="glowicons-host-", dir=ROOT / "build") as tmp:
            paths = [Path(tmp) / name for name in ("classic", "glow")]
            for path in paths:
                subprocess.run([sys.executable, str(ROOT / "install/tools/makeicon.py"),
                                str(path), "--style", path.name], check=True, stdout=subprocess.DEVNULL)
            classic, glow = paths
            for path in sorted(glow.glob("*.info")):
                with self.subTest(icon=path.name):
                    fallback = (classic / path.name).read_bytes()
                    shipping = path.read_bytes()
                    self.assertEqual(shipping[:len(fallback)], fallback)
                    with contextlib.redirect_stdout(io.StringIO()):
                        self.assertTrue(reader.show(path))
                        self.assertTrue(reader.show(classic / path.name))
                    states = reader.read_glow(shipping[len(fallback):])
                    self.assertEqual(len(states), 2)
                    for state in states:
                        self.assertEqual((state["width"], state["height"]), (48, 48))
                        self.assertEqual(len(state["palette"]), 128 * 3)
                        self.assertIn(0, state["pixels"])
                        self.assertTrue(any(state["pixels"]))
                    self.assertNotEqual(states[0]["pixels"], states[1]["pixels"])
                    self.assertEqual(shipping, (ROOT / "install" / path.name).read_bytes())

    def test_reject_malformed_forms(self):
        data = (ROOT / "install/icons/glow/NetPrefs.glow").read_bytes()
        # Missing selected state, unknown chunk, truncation and trailing bytes
        # must not be silently accepted by the archive validator.
        off = 12
        chunks = []
        while off < len(data):
            size = struct.unpack(">I", data[off + 4:off + 8])[0]
            end = off + 8 + size + (size & 1)
            chunks.append(data[off:end])
            off = end
        body = b"ICON" + b"".join(chunks[:2])
        missing_selected = b"FORM" + struct.pack(">I", len(body)) + body
        unknown = bytearray(data)
        unknown[12:16] = b"BOGUS"[:4]
        invalid_palette = bytearray(data)
        first_imag = 12 + len(chunks[0])
        invalid_palette[first_imag + 8] = 255
        for broken in (data[:-1], data + b"junk", missing_selected,
                       bytes(unknown), bytes(invalid_palette)):
            with self.subTest(length=len(broken)):
                with self.assertRaises(ValueError):
                    reader.read_glow(broken)

    def test_rle_bounds(self):
        self.assertEqual(reader.unpack_bytes(b"\x02abc\xfeZ", 6, True), b"abcZZZ")
        for data, size in ((b"\x02a", 3), (b"\xfe", 3), (b"\xfeZ", 2),
                           (b"\x00a", 2), (b"\x80", 1)):
            with self.subTest(data=data, size=size):
                with self.assertRaises(ValueError):
                    reader.unpack_bytes(data, size, True)


if __name__ == "__main__":
    unittest.main()
