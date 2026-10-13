#!/usr/bin/env python3
"""Verify/stage unmodified PCMCIA drivers with licenses and complete source.

SPDX-License-Identifier: MIT
"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / "third_party/pcmcia"
URLS = {
    "prism2v2.lha": "https://aminet.net/driver/net/prism2v2.lha",
    "xircomce2.lha": "https://aminet.net/driver/net/xircomce2.lha",
    "pccard.lha": "https://aminet.net/util/libs/pccard.lha",
}


def verify():
    manifest = json.loads((VENDOR / "SHA256.json").read_text())
    required = {
        "prism2.device", "xircomce2.device", "pccard.library",
        "Firmware/HermesI", "Firmware/HermesII", "Docs/Hermes-LICENSE",
        "Docs/GPL", "Docs/LGPL", "Docs/Prism2.guide", "Docs/Xircom.guide",
        "Docs/PCCard.guide", *("Sources/" + name for name in URLS),
    }
    if not required.issubset(manifest):
        raise ValueError("PCMCIA manifest lacks required binaries, licenses or source")
    for rel, expected in manifest.items():
        data = (VENDOR / rel).read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f"PCMCIA checksum mismatch: {rel}")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, help="distribution's AmiNetXDuo drawer")
    parser.add_argument("--fetch", action="store_true", help="compare source archives with upstream")
    args = parser.parse_args()
    manifest = verify()
    if args.fetch:
        work = ROOT / "build"
        work.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="pcmcia-fetch-", dir=work) as directory:
            for name, url in URLS.items():
                archive = Path(directory) / name
                subprocess.run(["curl", "-fsSL", "--max-time", "60", "-o", str(archive), url], check=True)
                if archive.read_bytes() != (VENDOR / "Sources" / name).read_bytes():
                    raise ValueError(f"PCMCIA upstream archive changed: {name}")
    if args.stage:
        dest = args.stage / "PCMCIA"
        for rel in [*manifest, "README", "SHA256.json"]:
            target = dest / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(VENDOR / rel, target)
    print(f"PCMCIA: verified {len(manifest)} pinned files, licenses and corresponding source")


if __name__ == "__main__":
    main()
