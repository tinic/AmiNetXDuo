#!/usr/bin/env bash
# Isolated A2065 Ethernet loopback: no installed stack or LAN is touched.
# Build anxnet_device, netmulticast_test and netmulticast_launcher first.
# Needs Copperline with station/LADRF filtering (fix/a2065-receive-filter).
# AMINETXDUO_COPPERLINE selects that optimized emulator executable.
# This is a hand-run regression, not a scheduled CI or hardware test.
# SPDX-License-Identifier: MIT
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
exec python3 - "$ROOT" "$@" <<'PY'
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
parser = argparse.ArgumentParser(description="Isolated Copperline A2065 multicast regression")
parser.add_argument("-b", "--build", default=os.environ.get("AMINETXDUO_BUILD", "build/fat"))
parser.add_argument("-t", "--timeout", type=int, default=180)
parser.add_argument("--copperline", default=os.environ.get("AMINETXDUO_COPPERLINE", "copperline"))
args = parser.parse_args(sys.argv[2:])
if args.timeout <= 0:
    parser.error("timeout must be positive")
emulator = shutil.which(args.copperline)
if emulator is None:
    parser.error("Copperline not found; set AMINETXDUO_COPPERLINE or --copperline")
build = root / args.build
artifacts = {
    "netmulticast_test": build / "tests/netdev/netmulticast_test",
    "launcher": build / "tests/netdev/netmulticast_launcher",
    "devs/Networks/anxnet.device": build / "src/netdev/anxnet.device",
}
for source in artifacts.values():
    if not source.is_file():
        parser.error(f"build {source} first")
(root / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="multicast-", dir=root / "build") as scratch:
    stage = Path(scratch)
    for name, source in artifacts.items():
        destination = stage / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    config = stage / "copperline.toml"
    config.write_text('[cpu]\nmodel = "68020"\nclock_mhz = 50.0\n'
                      '[memory]\nfast = "8M"\n[a2065]\nnet = "loopback"\n'
                      '[[filesys]]\nvolume = "DH0"\npath = ' + json.dumps(str(stage)) + '\n')
    try:
        result = subprocess.run([emulator, "--config", str(config), "--model", "A1200",
                                 "--run", str(stage / "launcher"), "--run-stack", "65536",
                                 "--exit-on-return", "--noaudio"],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        print("multicast: guest timed out", file=sys.stderr)
        sys.exit(1)
    output = stage / "stdout.txt"
    transcript = output.read_text(errors="replace") if output.exists() else ""
    print(transcript, end="")
    if result.returncode != 0 or "21 checks, 0 failures, PASS" not in transcript.splitlines():
        print(result.stdout, file=sys.stderr)
        print(f"multicast: incomplete or failing guest (emulator exit {result.returncode})", file=sys.stderr)
        sys.exit(1)
PY
