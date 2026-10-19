#!/usr/bin/env python3
"""Stop every process a CI job in this checkout left running.

    tools/ci-reap.py [--dry-run] [WORKSPACE]

WORKSPACE defaults to $GITHUB_WORKSPACE.  A process is this checkout's when
its environment holds GITHUB_WORKSPACE=WORKSPACE.  Every process a step
starts inherits that variable, so the match still holds after a harness has
been re-parented to init.  The caller and its ancestors are spared.

Run 39112947759 (2026-10-18): the job hit its time limit inside
install/test/run-all.sh.  The runner signalled the step's shell only, the
harness under it was re-parented and kept going, and the "amiberry .*build/"
sweep killed one guest after another for ten seconds while run-all.sh
started the next row.  The last row started after the sweep, and its
amiberry ran for 8 h 47 m.  Matching by path finds the guests but not the
script that keeps starting them; matching by environment finds both.

SIGTERM first, so a harness trap can give back its port and its locks; then
SIGKILL for anything still there after three seconds.  The scan repeats
until two scans in a row, a second apart, find nothing.

Output is key=value: reaped=N leftover=N.  Exit 0 when nothing is left.
"""

import os
import signal
import sys
import time


def ppid_of(pid):
    try:
        with open(f"/proc/{pid}/stat") as f:
            return int(f.read().rsplit(")", 1)[1].split()[1])
    except (OSError, ValueError, IndexError):
        return 0


def ancestors():
    keep = set()
    pid = os.getpid()
    while pid > 1 and pid not in keep:
        keep.add(pid)
        pid = ppid_of(pid)
    return keep


def matches(workspace, keep):
    want = b"GITHUB_WORKSPACE=" + os.fsencode(workspace)
    uid = os.getuid()
    found = []
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        pid = int(name)
        if pid in keep:
            continue
        try:
            if os.stat(f"/proc/{pid}").st_uid != uid:
                continue
            with open(f"/proc/{pid}/environ", "rb") as f:
                env = f.read().split(b"\0")
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                cmd = f.read().replace(b"\0", b" ").decode(errors="replace")
        except OSError:
            continue
        if want in env:
            found.append((pid, cmd.strip()))
    return found


def send(pid, sig):
    try:
        os.kill(pid, sig)
        return True
    except OSError:
        return False


def main():
    args = sys.argv[1:]
    dry = "--dry-run" in args
    args = [a for a in args if a != "--dry-run"]
    workspace = args[0] if args else os.environ.get("GITHUB_WORKSPACE", "")
    if not workspace:
        print("ci-reap: no workspace (argument or GITHUB_WORKSPACE)",
              file=sys.stderr)
        return 2

    keep = ancestors()

    if dry:
        found = matches(workspace, keep)
        for pid, cmd in found:
            print(f"pid={pid} cmd={cmd[:160]}")
        print(f"reaped=0 leftover={len(found)}")
        return 0

    reaped = set()
    quiet_scans = 0
    deadline = time.monotonic() + 60
    while quiet_scans < 2 and time.monotonic() < deadline:
        found = matches(workspace, keep)
        if not found:
            quiet_scans += 1
            time.sleep(1)
            continue
        quiet_scans = 0
        for pid, cmd in found:
            if send(pid, signal.SIGTERM) and pid not in reaped:
                reaped.add(pid)
                print(f"term pid={pid} cmd={cmd[:160]}")
        time.sleep(3)
        for pid, _ in matches(workspace, keep):
            if send(pid, signal.SIGKILL):
                print(f"kill pid={pid}")
        time.sleep(1)

    left = matches(workspace, keep)
    for pid, cmd in left:
        print(f"left pid={pid} cmd={cmd[:160]}")
    print(f"reaped={len(reaped)} leftover={len(left)}")
    return 0 if not left else 1


if __name__ == "__main__":
    sys.exit(main())
