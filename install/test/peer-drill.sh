#!/bin/sh
#
# What another machine does to the Amiga, run ON that other machine.
#
#   peer-drill.sh <address> <payload.txt> <payload.lha>
#
# install/test/run-workbench.sh copies this, the two drills and both payloads
# to AMINETXDUO_PEER and runs it there.  It is a separate file, and /bin/sh
# with nothing but python3, because the peer is whatever machine can reach the
# guest -- it is not a checkout of this repository and may have no bash, no
# curl and no repository at all.
#
# It prints key=value lines and nothing else.  Every one of them is an
# assertion about bytes or a status code; none of them is "the command
# completed".  A step that cannot run prints its own key rather than being
# absent, because an absent key reads as a step that passed.
#
# SPDX-License-Identifier: MIT

ADDR="$1"
TXT="$2"
LHA="$3"
PORT=80

python3 - "$ADDR" "$PORT" "$TXT" "$LHA" <<'PY'
import hashlib
import http.client
import os
import sys

addr, port, txt, lha = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]


def say(k, v):
    print("%s=%s" % (k, v), flush=True)


def conn():
    return http.client.HTTPConnection(addr, port, timeout=60)


def request(method, path, body=None, headers=None):
    """One request on its own connection.  Returns (status, headers, body)."""
    c = conn()
    try:
        c.request(method, path, body=body, headers=headers or {})
        r = c.getresponse()
        data = r.read()
        return r.status, dict(r.getheaders()), data
    finally:
        c.close()


# ---- 1. the terminal is served, from the -T the installer's line carries ----
#
# The head is the assertion, not the body: Content-Length is the server saying
# how big the page it found is, and it is 0 or absent on a server that started
# without -T.  The body is fetched too and its length reported, which is where
# a transport that cannot carry 400 KB shows up as a number rather than as a
# failure of the endpoint.
try:
    st, hdr, body = request("GET", "/terminal")
    say("terminal_status", st)
    say("terminal_content_length", hdr.get("Content-Length", "none"))
    say("terminal_body_bytes", len(body))
except Exception as exc:                                  # noqa: BLE001
    say("terminal_status", "error")
    say("terminal_error", type(exc).__name__)

# ---- 2. WebDAV: put a file, read it back, compare the bytes ----------------
with open(txt, "rb") as fh:
    payload = fh.read()

try:
    st, _, _ = request("PUT", "/payload.txt", body=payload,
                       headers={"Content-Length": str(len(payload))})
    say("dav_put_status", st)
except Exception as exc:                                  # noqa: BLE001
    say("dav_put_status", "error")
    say("dav_put_error", type(exc).__name__)

try:
    st, _, got = request("GET", "/payload.txt")
    say("dav_get_status", st)
    say("dav_get_bytes", len(got))
    say("dav_roundtrip_identical", "yes" if got == payload else "no")
except Exception as exc:                                  # noqa: BLE001
    say("dav_get_status", "error")
    say("dav_roundtrip_identical", "no")

# ---- 3. WebDAV: put the release archive, for the Amiga to unpack -----------
#
# Our own .lha, because it is certainly present and certainly well formed, and
# because it means the artifact being unpacked is the artifact being shipped.
with open(lha, "rb") as fh:
    arc = fh.read()
say("dav_archive_bytes_sent", len(arc))
say("dav_archive_sha256", hashlib.sha256(arc).hexdigest())

try:
    st, _, _ = request("PUT", "/payload.lha", body=arc,
                       headers={"Content-Length": str(len(arc))})
    say("dav_put_archive_status", st)
except Exception as exc:                                  # noqa: BLE001
    say("dav_put_archive_status", "error")
    say("dav_put_archive_error", type(exc).__name__)

# ---- 4. the WebDAV drill, and the terminal drill ---------------------------
#
# tests/tools/httpd-drill.py's twelve assertions and wsterm-console.py's
# console ones, against the installed machine.  Reused rather than rewritten:
# they are the ones that need a socket rather than a client, and having two of
# them would be two things to keep right.
here = os.path.dirname(os.path.abspath(sys.argv[0])) if sys.argv[0] else "/tmp"
for name, script, args in (
        ("httpd_drill", "/tmp/httpd-drill.py", ["--terminal"]),
        ("wsterm_console", "/tmp/wsterm-console.py", []),
):
    if not os.path.exists(script):
        say(name, "absent")
        continue
    rc = os.spawnv(os.P_WAIT, sys.executable,
                   [sys.executable, script] + args + [addr, str(port)])
    say(name + "_rc", rc)
PY
