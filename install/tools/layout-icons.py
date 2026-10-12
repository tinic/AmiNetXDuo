#!/usr/bin/env python3
"""Lay out archive icons without giving reusable templates overlapping positions.

Usage: layout-icons.py <distribution-directory>
The directory contains AmiNetXDuo/ and its sibling AmiNetXDuo.info.
SPDX-License-Identifier: MIT
"""

from pathlib import Path
import shutil
import struct
import sys


def edit(path, position=None, window=None):
    data = bytearray(path.read_bytes())
    if len(data) < 78 or data[:4] != b"\xe3\x10\x00\x01":
        raise ValueError(f"not a version-1 DiskObject: {path}")
    if position is not None:
        struct.pack_into(">ii", data, 58, *position)
    if window is not None:
        if not struct.unpack_from(">I", data, 66)[0] or len(data) < 134:
            raise ValueError(f"icon has no DrawerData: {path}")
        struct.pack_into(">hhhh", data, 78, *window)
    path.write_bytes(data)


def layout(outdir):
    tree = Path(outdir) / "AmiNetXDuo"
    positions = {
        "Install-AmiNetXDuo.info": (358, 10),
        "Uninstall-AmiNetXDuo.info": (210, 146),
        "ReadMe.info": (506, 10),
        "Docs.info": (62, 78),
        "Examples.info": (210, 78),
        "Terminal.info": (358, 78),
        "Developer.info": (506, 78),
    }
    for name, position in positions.items():
        path = tree / name
        if path.exists():
            edit(path, position)
    # Leave the parent drawer icon and reusable document/drawer icons
    # unsnapped. Their destination can already contain unrelated icons.
    # Icons/ holds the tool icons a private install puts in the drawer root.
    for name, xy in (("NetPrefs.info", (62, 10)),
                     ("NetMeter.info", (210, 10)),
                     ("NetWireless.info", (62, 146)),
                     ("NetWirelessPrefs.info", (358, 146))):
        path = tree / "Icons" / name
        if path.exists():
            edit(path, xy)

if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: layout-icons.py <distribution-directory>")
    layout(sys.argv[1])
