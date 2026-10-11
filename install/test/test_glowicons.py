#!/usr/bin/env python3
"""Validate shipping GlowIcons, classic fallback and malformed IFF/RLE input.

SPDX-License-Identifier: MIT
"""

import contextlib
import importlib.util
import io
from pathlib import Path
import shutil
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
    def test_archive_layout(self):
        (ROOT / "build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="icon-layout-", dir=ROOT / "build") as tmp:
            out = Path(tmp)
            tree = out / "AmiNetXDuo"
            (tree / "C/fat").mkdir(parents=True)
            for target, source in {
                    "Install-AmiNetXDuo.info": "Install-AmiNetXDuo",
                    "ReadMe.info": "Document", "Docs.info": "Drawer",
                    "Examples.info": "Drawer", "Terminal.info": "Drawer",
                    "Developer.info": "Drawer", "AmiNetXDuo.info": "AmiNetXDuo",
                    "C/NetMeter.info": "NetMeter", "C/NetPrefs.info": "NetPrefs",
                    "C/fat/NetMeter.info": "NetMeter", "C/fat/NetPrefs.info": "NetPrefs"}.items():
                shutil.copyfile(ROOT / "install" / (source + ".info"), tree / target)
            original = {p: p.read_bytes() for p in tree.rglob("*.info")}
            subprocess.run([sys.executable, str(ROOT / "install/tools/layout-icons.py"),
                            str(out)], check=True)
            occupied = set()
            for path, before in original.items():
                after = path.read_bytes()
                # Layout changes must preserve launch settings and artwork.
                self.assertEqual(after[:58], before[:58])
                self.assertEqual(after[66:], before[66:])
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertTrue(reader.show(path))
                if path.parent == tree and path.name != "AmiNetXDuo.info":
                    x, y = struct.unpack_from(">ii", after, 58)
                    self.assertNotIn((x, y), occupied)
                    occupied.add((x, y))
                    self.assertGreaterEqual(x - 48, 0)  # room for long labels
                    self.assertLess(x + 92, 620 - 18)
                    self.assertLess(y + 48 + 12, 236 - 20)
            # The parent icon is reused at the installation destination:
            # leave its position to Workbench instead of overlapping other apps.
            self.assertEqual((tree / "AmiNetXDuo.info").read_bytes(),
                             original[tree / "AmiNetXDuo.info"])
            tools = (tree / "C.info").read_bytes()
            left, top, width, height = struct.unpack_from(">hhhh", tools, 78)
            self.assertLessEqual(left + width, 640)
            self.assertLessEqual(top + height, 256)
            for path in (tree / "C").glob("*.info"):
                x, y = struct.unpack_from(">ii", path.read_bytes(), 58)
                self.assertGreaterEqual(x, 0)
                self.assertLess(x + 48 + 12, width - 18)
                self.assertLess(y + 48 + 12, height - 20)
            # Re-running packaging must not drift positions or data.
            once = {p: p.read_bytes() for p in tree.rglob("*.info")}
            subprocess.run([sys.executable, str(ROOT / "install/tools/layout-icons.py"),
                            str(out)], check=True)
            self.assertEqual(once, {p: p.read_bytes() for p in tree.rglob("*.info")})

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
                    # Selecting an icon cannot change its opaque silhouette.
                    self.assertEqual([bool(v) for v in states[0]["pixels"]],
                                     [bool(v) for v in states[1]["pixels"]])
                    normal, selected = (state["pixels"] for state in states)
                    for y in range(1, 47):
                        for x in range(1, 47):
                            i = y * 48 + x
                            if all(normal[j] for j in
                                   (i, i - 1, i + 1, i - 48, i + 48)):
                                self.assertEqual(normal[i], selected[i])
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
