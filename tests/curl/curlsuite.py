#!/usr/bin/env python3
"""The curl verification suite: the cases, and the scoring of them.

    tests/curl/curlsuite.py --emit FILE [--groups ABCDEF] [--curl SYS:curl]
    tests/curl/curlsuite.py --score DIR  [--groups ABCDEF]

WHAT THIS IS FOR, AND WHAT IT IS NOT

    curl is the adversary, not the subject.  Everything below is a transfer
    that a widely-exercised client performs against bsdsocket.library, chosen
    because it drives a part of the ABI that our own tools do not, or drives
    it harder.  A red result is a bug in the stack until somebody proves
    otherwise; "curl is wrong" is the last explanation to reach for, not the
    first.

    So the cases are grouped by what they make the STACK do, not by what they
    make curl do:

      A  HTTP mechanics        the ordinary receive path, at many sizes and
                               framings, plus the header parser's buffers
      B  connection behaviour  reuse, forced fresh connects, and curl's multi
                               interface -- which is the heavy WaitSelect()
                               path and the wakeup socketpair with it
      C  failure paths         refused, unresolvable, closed mid-body, RESET,
                               accepted and silent, every timeout, and an
                               abort with the connection still full of data.
                               This is where a stack crashes rather than
                               returning an error, so it is the point of the
                               exercise
      D  resource behaviour    many transfers in one process, many processes
                               in sequence, and a concurrency high enough to
                               matter against a 64-descriptor table
      E  TLS                   cold and resumed, chain depths 2/3/4, RSA and
                               ECDSA, the four ways verification must refuse,
                               --cacert, and a large body through the record
                               layer
      F  FTP                   two connections at once, and the client side of
                               listen()/accept() in active mode
      G  the internet          NOT A BASELINE.  Separate on purpose

    Byte-exactness is checked on the host for every body: DH0: is a host
    directory, so what the Amiga wrote is already here, and the servers' bodies
    are slices of one seeded buffer.  Nothing is taken on trust from a status
    line.

WHY THE SCORING IS HERE AND NOT ON THE AMIGA

    Because a run that dies has to be scored too.  The Amiga side appends one
    line per case as it goes; every case with no line is a failure, and the
    first missing one names the command that took the machine down.  A driver
    that scored its own run would report nothing at all in exactly that case.

SPDX-License-Identifier: MIT
"""

import argparse
import hashlib
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from curlpeer import MASTER, master                     # noqa: E402


# The one -w line every case emits.  No spaces, so it needs no quoting that
# the AmigaDOS Shell might read differently from a Unix one -- and POSITIONAL
# rather than key=value, because a Kickstart Shell reads a command line into a
# 512-byte buffer and a 40-character saving is worth having on every case.
WFIELDS = ("code", "size", "up", "conns", "redirs", "app")
WFMT = ("W:%{http_code},%{size_download},%{size_upload},"
        "%{num_connects},%{num_redirects},%{time_appconnect}\\n")

# What one generated line may cost: the driver appends " <NIL: >>DH0:w/<name>
# .txt" and the Shell's buffer is the limit nobody documents until a command
# comes back truncated.
SHELL_LINE_BUDGET = 500

HOST = "10.0.2.2"
GUEST = "10.0.2.15"


# NOTHING HERE ASSERTS ON curl's STDERR TEXT, AND THAT IS DELIBERATE.
#
#   AmigaDOS `>>file` redirects standard output only.  Our curl is newlib,
#   whose stderr lands on the same handle, so its `curl: (7) ...` lines are
#   captured; a clib2-built curl keeps stderr separate and its diagnostics go
#   to the console instead, where nothing can read them.  Two assertions on
#   that text were the only cases a third-party binary failed for a reason
#   that was neither its fault nor the stack's.  The exit code already says
#   exactly the same thing and says it for every build.
class Case:
    def __init__(self, name, group, cmd, rc=0, w=None, body=None,
                 contains=None, absent=None, note=None, internet=False):
        self.name = name
        self.group = group
        self.cmd = cmd
        self.rc = rc if isinstance(rc, (tuple, list)) else (rc,)
        self.w = w or {}
        self.body = body
        self.contains = contains
        self.absent = absent
        self.note = note
        self.internet = internet


def build_setup(curl):
    """Always run, never filtered out: without these nothing else means
    anything, and a failure here has to be legible rather than showing up as
    every case in the suite going red at once."""
    return [
        Case("s01_version", "S", "%s --version" % curl, contains="libcurl/"),
        Case("s02_interface", "S", "SYS:AddNetInterface eth0"),
        # `curl --version` twice, once either side of the interface, because
        # the first one takes SIXTEEN SECONDS and the reason is worth a line
        # in every run.  It is not the executable loading: it is the first
        # OpenLibrary("bsdsocket.library") bringing the whole stack up, and
        # AddNetInterface deliberately never closes its own handle (see
        # tool_stack_start()), so this one is fast and every curl after it is
        # too.  A machine that has not run AddNetInterface pays the sixteen
        # seconds on EVERY curl invocation, `--version` included.
        Case("s03_version_again", "S", "%s --version" % curl,
             contains="libcurl/"),
    ]


def build(base, curl, cacert="--cacert DH0:teststore"):
    """Every case, as a list.  base is the peer's base port."""
    http = "http://%s:%d" % (HOST, base)
    cases = []

    def add(name, group, cmd, **kw):
        cases.append(Case(name, group, cmd, **kw))

    def get(name, group, path, out=None, size=None, extra="", **kw):
        """A plain GET whose body is checked byte for byte."""
        args = ["%s -sS" % curl]
        if extra:
            args.append(extra)
        if out is not None:
            args.append("-o DH0:d/%s.bin" % name)
        args.append('-w "%s"' % WFMT)
        args.append('"%s%s"' % (http, path))
        add(name, group, " ".join(args), body=out, **kw)

    # ------------------------------------------------------- A: mechanics ---

    get("a01_get_1k", "A", "/bytes/1024", out=("master", 0, 1024),
        w={"code": "200", "size": "1024"})
    get("a02_empty", "A", "/empty", out=("literal", b""),
        w={"code": "200", "size": "0"})
    get("a03_get_64k", "A", "/bytes/65536", out=("master", 0, 65536),
        w={"code": "200", "size": "65536"})
    get("a04_get_1m2", "A", "/bytes/1200000", out=("master", 0, 1200000),
        w={"code": "200", "size": "1200000"},
        note="over a megabyte, the only size that exercises the receive path "
             "for long enough to drift")
    get("a05_get_1b", "A", "/bytes/1", out=("master", 0, 1),
        w={"code": "200", "size": "1"})
    get("a06_get_odd", "A", "/bytes/65537", out=("master", 0, 65537),
        w={"code": "200", "size": "65537"},
        note="one byte past a power of two, where an off-by-one in a buffer "
             "boundary lives")

    add("a07_head", "A",
        '%s -sS -I -w "%s" "%s/bytes/65536"' % (curl, WFMT, http),
        w={"code": "200", "size": "0"})
    add("a08_head_big", "A",
        '%s -sS -I -w "%s" "%s/bytes/1200000"' % (curl, WFMT, http),
        w={"code": "200", "size": "0"})

    for code in (200, 201, 202, 204, 301, 302, 307, 400, 401, 403, 404,
                 410, 418, 500, 503):
        add("a09_status_%d" % code, "A",
            '%s -sS -o DH0:d/a09_status_%d.bin -w "%s" "%s/status/%d"'
            % (curl, code, WFMT, http, code),
            w={"code": str(code)})

    add("a10_fail_on_404", "A",
        '%s -sS --fail -o DH0:d/a10_fail_on_404.bin -w "%s" "%s/status/404"'
        % (curl, WFMT, http), rc=22)
    add("a11_fail_on_500", "A",
        '%s -sS --fail -o DH0:d/a11_fail_on_500.bin -w "%s" "%s/status/500"'
        % (curl, WFMT, http), rc=22)

    get("a12_redirect_1", "A", "/redirect/1", out=("master", 0, 1024),
        extra="-L", w={"code": "200", "redirs": "1", "size": "1024"})
    get("a13_redirect_5", "A", "/redirect/5", out=("master", 0, 1024),
        extra="-L", w={"code": "200", "redirs": "5", "size": "1024"})
    get("a14_redirect_20", "A", "/redirect/20", out=("master", 0, 1024),
        extra="-L --max-redirs 30", w={"code": "200", "redirs": "20"})
    get("a15_redirect_301", "A", "/redirect/3?code=301",
        out=("master", 0, 1024), extra="-L",
        w={"code": "200", "redirs": "3"})
    get("a16_redirect_307", "A", "/redirect/3?code=307",
        out=("master", 0, 1024), extra="-L",
        w={"code": "200", "redirs": "3"})
    add("a17_redirect_capped", "A",
        '%s -sS -L --max-redirs 2 -o DH0:d/a17.bin -w "%s" "%s/redirect/5"'
        % (curl, WFMT, http), rc=47)
    add("a18_redirect_loop", "A",
        '%s -sS -L --max-redirs 8 -o DH0:d/a18.bin -w "%s" "%s/redirect-loop"'
        % (curl, WFMT, http), rc=47)
    get("a19_redirect_off", "A", "/redirect/3", out=None,
        w={"code": "302", "redirs": "0"})

    get("a20_chunked_7k", "A", "/chunked/7000", out=("master", 0, 7000),
        w={"code": "200", "size": "7000"})
    get("a21_chunked_256k", "A", "/chunked/262144",
        out=("master", 0, 262144), w={"code": "200", "size": "262144"})
    get("a22_trailer", "A", "/trailer/32768", out=("master", 0, 32768),
        w={"code": "200", "size": "32768"})

    get("a23_range_mid", "A", "/bytes/65536", out=("master", 100, 1024),
        extra="-r 100-1123", w={"code": "206", "size": "1024"})
    get("a24_range_tail", "A", "/bytes/65536",
        out=("master", 65536 - 4096, 4096), extra="-r -4096",
        w={"code": "206", "size": "4096"})
    get("a25_range_open", "A", "/bytes/65536",
        out=("master", 60000, 5536), extra="-r 60000-",
        w={"code": "206", "size": "5536"})

    get("a26_drip", "A", "/drip/8192?chunks=16&ms=150",
        out=("master", 0, 8192), w={"code": "200", "size": "8192"},
        note="16 writes 150 ms apart: a body that arrives in pieces far "
             "smaller than the receive buffer, over two and a half seconds")
    get("a27_slowheaders", "A", "/slowheaders/4096",
        out=("master", 0, 4096), w={"code": "200", "size": "4096"},
        note="the response header arrives one line at a time")

    get("a28_bigheaders", "A", "/bigheaders?count=200&size=300",
        out=("literal", b"big headers\n"), w={"code": "200"},
        note="about 62 KB of response header, which is many times any single "
             "read the stack will do")
    get("a29_hugeheader", "A", "/bigheaders?count=1&size=60000",
        out=("literal", b"big headers\n"), w={"code": "200"},
        note="ONE 60 KB header line, against curl's 100 KB per-header cap")

    for method in ("DELETE", "OPTIONS", "PATCH", "PUT", "POST"):
        add("a30_method_%s" % method.lower(), "A",
            '%s -sS -X %s -o DH0:d/a30_method_%s.bin -w "%s" "%s/echo-method"'
            % (curl, method, method.lower(), WFMT, http),
            body=("literal", (method + "\n").encode()), w={"code": "200"})

    add("a31_post_small", "A",
        '%s -sS --data-binary @DH0:up4k.bin -o DH0:d/a31_post_small.bin '
        '-w "%s" "%s/upload"' % (curl, WFMT, http),
        body=("uploadecho", 4096), w={"code": "200", "up": "4096"})
    add("a32_post_200k", "A",
        '%s -sS --data-binary @DH0:up200k.bin -o DH0:d/a32_post_200k.bin '
        '-w "%s" "%s/upload"' % (curl, WFMT, http),
        body=("uploadecho", 204800), w={"code": "200", "up": "204800"},
        note="200 KB out through send(), with a 100-continue round trip in "
             "front of it")
    add("a33_put_200k", "A",
        '%s -sS -T DH0:up200k.bin -o DH0:d/a33_put_200k.bin '
        '-w "%s" "%s/upload"' % (curl, WFMT, http),
        body=("uploadecho", 204800), w={"code": "200", "up": "204800"})

    add("a34_auth_fail", "A",
        '%s -sS -o DH0:d/a34.bin -w "%s" "%s/auth/basic"'
        % (curl, WFMT, http), w={"code": "401"})
    add("a35_auth_ok", "A",
        '%s -sS -u amiga:secret -o DH0:d/a35_auth_ok.bin -w "%s" '
        '"%s/auth/basic"' % (curl, WFMT, http),
        body=("master", 0, 1024), w={"code": "200", "size": "1024"})

    get("a36_http10", "A", "/bytes/32768", out=("master", 0, 32768),
        extra="--http1.0", w={"code": "200", "size": "32768"})
    get("a37_close_delimited", "A", "/close-delimited/131072",
        out=("master", 0, 131072), w={"code": "200", "size": "131072"},
        note="no Content-Length and no chunking: the body ends when the peer "
             "closes, so the last read has to see the FIN and not an error")

    get("a38_one_byte_chunks", "A", "/drip/4096?chunks=4096&ms=0",
        out=("master", 0, 4096), w={"code": "200", "size": "4096"},
        note="4,096 chunked writes of ONE byte each.  Whatever the stack "
             "does about coalescing, this is the worst case for it")
    get("a39_many_chunks", "A", "/drip/262144?chunks=1024&ms=0",
        out=("master", 0, 262144), w={"code": "200", "size": "262144"},
        note="256 KB in 1,024 separate writes, as fast as the host can")
    get("a40_halfclose", "A", "/halfclose/131072",
        out=("master", 0, 131072), w={"code": "200", "size": "131072"},
        note="the peer half-closes after the body and keeps reading.  A FIN "
             "arriving while our write side is still open is the case that "
             "broke shutdown(SHUT_WR) in this stack once already")
    get("a41_limit_rate", "A", "/bytes/131072", out=("master", 0, 131072),
        extra="--limit-rate 20k", w={"code": "200", "size": "131072"},
        note="curl reads slowly on purpose, so the sender fills our receive "
             "window and has to be told when it reopens")
    get("a42_long_request", "A", "/longredirect/2400",
        out=("literal", b"qlen=2400\n"), extra="-L",
        w={"code": "200", "redirs": "1"},
        note="a 2.4 KB request line, which is a single large send() before "
             "anything comes back")

    add("a43_cookies_set", "A",
        '%s -sS -c DH0:cj.txt -o DH0:d/a43.bin -w "%s" "%s/setcookies?n=60"'
        % (curl, WFMT, http), w={"code": "200"})
    add("a44_cookies_send", "A",
        '%s -sS -b DH0:cj.txt -o DH0:d/a44.bin -w "%s" "%s/needbigrequest?min=2000"'
        % (curl, WFMT, http), w={"code": "200"},
        note="60 cookies come back as one ~3 KB Cookie header; the endpoint "
             "answers 400 if the request header was under 2 KB")

    add("a45_local_port", "A",
        '%s -sS --local-port 7300-7310 -o DH0:d/a45_local_port.bin -w "%s" '
        '"%s/bytes/8192"' % (curl, WFMT, http),
        body=("master", 0, 8192), w={"code": "200", "size": "8192"},
        note="bind() on a client socket before connect(), which nothing else "
             "in this suite asks for")

    # ----------------------------------------------- B: connection reuse ----

    add("b01_reuse_4", "B",
        '%s -sS %s -w "%s"' % (
            curl,
            " ".join('-o DH0:d/b01_%d.bin "%s/bytes/%d"' % (i, http, 1000 + i)
                     for i in range(4)),
            WFMT),
        body=("multi", [("b01_%d.bin" % i, ("master", 0, 1000 + i))
                        for i in range(4)]),
        w={"conns_total": "1", "n": "4"},
        note="four transfers, one connection.  conns_total=1 is the check")
    add("b02_no_reuse_4", "B",
        '%s -sS -H "Connection:close" %s -w "%s"' % (
            curl,
            " ".join('-o DH0:d/b02_%d.bin "%s/bytes/%d"' % (i, http, 1000 + i)
                     for i in range(4)),
            WFMT),
        body=("multi", [("b02_%d.bin" % i, ("master", 0, 1000 + i))
                        for i in range(4)]),
        w={"conns_total": "4", "n": "4"},
        note="the same four with the peer closing after each: four full "
             "connect/transfer/close cycles in one process")
    add("b03_reuse_after_404", "B",
        '%s -sS -o DH0:d/b03_a.bin "%s/status/404" '
        '-o DH0:d/b03_b.bin "%s/bytes/2048" -w "%s"'
        % (curl, http, http, WFMT),
        body=("multi", [("b03_b.bin", ("master", 0, 2048))]),
        w={"conns_total": "1", "n": "2"})
    add("b04_reuse_after_head", "B",
        '%s -sS -w "%s" -I -o DH0:d/b04_a.bin "%s/bytes/65536" --next '
        '-w "%s" -o DH0:d/b04_b.bin "%s/bytes/2048"'
        % (curl, WFMT, http, WFMT, http),
        body=("multi", [("b04_b.bin", ("master", 0, 2048))]),
        w={"conns_total": "1", "n": "2"},
        note="a HEAD leaves no body behind; reusing the connection after one "
             "is where a client that miscounts hangs")

    add("b05_parallel_4", "B",
        '%s -sS -Z --parallel-max 4 %s -w "%s"' % (
            curl,
            " ".join('-o DH0:d/b05_%d.bin "%s/bytes/%d"' % (i, http, 20000 + i)
                     for i in range(4)),
            WFMT),
        body=("multi", [("b05_%d.bin" % i, ("master", 0, 20000 + i))
                        for i in range(4)]),
        note="curl's multi interface: four concurrent transfers, one "
             "WaitSelect() over all of them, and the wakeup socketpair that "
             "lib/socketpair.c builds out of bind/listen/connect/accept")
    add("b06_parallel_mixed", "B",
        '%s -sS -Z --parallel-max 8 %s %s -w "%s"' % (
            curl,
            " ".join('-o DH0:d/b06_%d.bin "%s/bytes/%d"' % (i, http, 40000 + i)
                     for i in range(3)),
            '-o DH0:d/b06_drip.bin "%s/drip/4096?chunks=8&ms=200"' % http,
            WFMT),
        body=("multi",
              [("b06_%d.bin" % i, ("master", 0, 40000 + i)) for i in range(3)]
              + [("b06_drip.bin", ("master", 0, 4096))]),
        note="a slow transfer alongside fast ones, so the fast ones must not "
             "wait on it and the slow one must not be starved")
    add("b07_parallel_chunked", "B",
        '%s -sS -Z --parallel-max 4 %s -w "%s"' % (
            curl,
            " ".join('-o DH0:d/b07_%d.bin "%s/chunked/%d"' % (i, http, 30000 + i)
                     for i in range(4)),
            WFMT),
        body=("multi", [("b07_%d.bin" % i, ("master", 0, 30000 + i))
                        for i in range(4)]))

    # -------------------------------------------------- C: failure paths ----

    add("c01_refused", "C",
        '%s -sS -o DH0:d/c01.bin -w "%s" "http://%s:%d/hello"'
        % (curl, WFMT, HOST, base + 99), rc=7)
    add("c02_refused_loopback", "C",
        '%s -sS -o DH0:d/c02.bin -w "%s" "http://127.0.0.1:%d/hello"'
        % (curl, WFMT, base + 99), rc=7)
    add("c03_dns_nxdomain", "C",
        '%s -sS -o DH0:d/c03.bin -w "%s" "http://no.such.host.invalid/"'
        % (curl, WFMT), rc=6,
        note="needs a resolver that answers; without one it still fails with "
             "(6), just slowly")
    add("c04_truncated_body", "C",
        '%s -sS -o DH0:d/c04.bin -w "%s" "%s/truncate/400000"'
        % (curl, WFMT, http), rc=18,
        note="the peer promises 400 KB and closes after 100 KB: an orderly "
             "FIN in the middle of a body curl is still counting")
    add("c05_reset_mid_body", "C",
        '%s -sS -o DH0:d/c05.bin -w "%s" "%s/reset/400000"'
        % (curl, WFMT, http), rc=(56, 18),
        note="the same, with a RESET instead of a FIN.  A stack that turns a "
             "reset into a crash rather than an error fails here")
    add("c06_empty_reply", "C",
        '%s -sS -o DH0:d/c06.bin -w "%s" "http://%s:%d/hello"'
        % (curl, WFMT, HOST, base + 20), rc=52)
    add("c07_reset_at_accept", "C",
        '%s -sS -o DH0:d/c07.bin -w "%s" "http://%s:%d/hello"'
        % (curl, WFMT, HOST, base + 21), rc=(56, 52, 55, 35, 7),
        note="accepted and reset before a byte is read")
    add("c08_not_http", "C",
        '%s -sS -o DH0:d/c08.bin -w "%s" "http://%s:%d/hello"'
        % (curl, WFMT, HOST, base + 23), rc=1)
    add("c09_silent_maxtime", "C",
        '%s -sS --max-time 10 -o DH0:d/c09.bin -w "%s" "http://%s:%d/hello"'
        % (curl, WFMT, HOST, base + 22), rc=28,
        note="accepted and never answered.  --max-time is the only thing that "
             "ends this, so it is also a test that curl's clock still runs")
    add("c10_silent_endpoint", "C",
        '%s -sS --max-time 8 -o DH0:d/c10.bin -w "%s" "%s/never"'
        % (curl, WFMT, http), rc=28)
    add("c11_maxtime_during_drip", "C",
        '%s -sS --max-time 5 -o DH0:d/c11.bin -w "%s" '
        '"%s/drip/65536?chunks=40&ms=500"' % (curl, WFMT, http), rc=28,
        note="the timeout fires with data still arriving, so curl closes a "
             "connection the peer is mid-write on")
    add("c12_connect_timeout", "C",
        '%s -sS --connect-timeout 5 -o DH0:d/c12.bin -w "%s" '
        '"http://192.0.2.1/hello"' % (curl, WFMT), rc=(28, 7),
        note="TEST-NET-1, which SLIRP has nowhere to send")
    add("c13_maxfilesize", "C",
        '%s -sS --max-filesize 4096 -o DH0:d/c13.bin -w "%s" '
        '"%s/bytes/262144"' % (curl, WFMT, http), rc=63)
    add("c14_abort_mid_large", "C",
        '%s -sS --max-time 3 -o DH0:d/c14.bin -w "%s" "%s/bytes/1900000"'
        % (curl, WFMT, http), rc=28,
        note="the Ctrl-C case: curl walks away from a connection with the "
             "peer's window full of data still queued")
    add("c15_redirect_to_refused", "C",
        '%s -sS -L -o DH0:d/c15.bin -w "%s" "%s/redirect-refused"'
        % (curl, WFMT, http), rc=7)
    add("c16_after_failures", "C",
        '%s -sS -o DH0:d/c16_after_failures.bin -w "%s" "%s/bytes/16384"'
        % (curl, WFMT, http), body=("master", 0, 16384),
        w={"code": "200", "size": "16384"},
        note="an ordinary transfer AFTER all of the above.  If the stack is "
             "damaged by a failure path, this is the case that says so")

    # ---------------------------------------------- D: resource behaviour ---

    add("d01_glob_60", "D",
        '%s -sS -o "DH0:g1/#1.bin" -w "%s" "%s/bytes/[1000-1059]"'
        % (curl, WFMT, http),
        body=("glob", "g1", 1000, 1060),
        w={"conns_total": "1", "n": "60"},
        note="60 transfers on one handle and one connection")
    add("d02_glob_60_parallel", "D",
        '%s -sS -Z --parallel-max 8 -o "DH0:g2/#1.bin" -w "%s" '
        '"%s/bytes/[2000-2059]"' % (curl, WFMT, http),
        body=("glob", "g2", 2000, 2060))
    add("d03_parallel_40", "D",
        '%s -sS -Z --parallel-max 40 -o "DH0:g3/#1.bin" -w "%s" '
        '"%s/bytes/[3000-3039]"' % (curl, WFMT, http),
        body=("glob", "g3", 3000, 3040),
        note="forty sockets at once, plus the wakeup pair, against a "
             "64-entry descriptor table and curl's FD_SETSIZE of 64")
    add("d04_no_reuse_20", "D",
        '%s -sS -H "Connection:close" -o "DH0:g4/#1.bin" -w "%s" '
        '"%s/bytes/[4000-4019]"' % (curl, WFMT, http),
        body=("glob", "g4", 4000, 4020),
        w={"conns_total": "20", "n": "20"},
        note="twenty connect/transfer/close cycles: descriptors have to come "
             "back or the twentieth fails")

    for i in range(1, 21):
        add("d05_proc_%02d" % i, "D",
            '%s -sS -o DH0:d/d05_proc_%02d.bin -w "%s" "%s/bytes/%d"'
            % (curl, i, WFMT, http, 40960),
            body=("master", 0, 40960), w={"code": "200", "size": "40960"})

    # ------------------------------------------------------------- E: TLS ---

    def https(name, port, extra="", host=None, **kw):
        h = host or name
        add(name, "E",
            '%s -sS %s --resolve %s:%d:%s %s -o DH0:d/%s.bin -w "%s" '
            '"https://%s:%d%s"'
            % (curl, cacert, h, base + port, HOST, extra, name, WFMT,
               h, base + port, kw.pop("path", "/bytes/1024")),
            **kw)

    # THE TRUST-STORE CASES COME FIRST, AND THE ORDER IS THE TEST.
    #
    # A resumed handshake verifies no certificate at all -- that is the whole
    # point of it -- so a suite that asks "is a bad trust store refused?"
    # AFTER something else has already talked to that host is not asking about
    # verification, it is asking about the session cache.  Both questions are
    # worth answering, so both are asked: cold here, warm at the end of the
    # group, with every other case to that host in between.
    add("e01_wrong_cacert_cold", "E",
        '%s -sS --cacert DH0:otherstore --resolve rsa2.test:%d:%s '
        '-o DH0:d/e01.bin -w "%s" "https://rsa2.test:%d/bytes/1024"'
        % (curl, base + 1, HOST, WFMT, base + 1), rc=60,
        note="a valid trust store containing the wrong root")
    add("e02_default_store_cold", "E",
        '%s -sS --resolve rsa2.test:%d:%s -o DH0:d/e02.bin -w "%s" '
        '"https://rsa2.test:%d/bytes/1024"' % (curl, base + 1, HOST, WFMT,
                                               base + 1), rc=60,
        note="no --cacert, so DEVS:Internet/certificates -- the real Mozilla "
             "set, which has never heard of our root")

    https("rsa2.test", 1, body=("master", 0, 1024),
          w={"code": "200", "size": "1024"},
          note="RSA leaf under one intermediate, cold")
    https("rsa2b.test", 1, host="rsa2.test", body=("master", 0, 1024),
          w={"code": "200"},
          note="the same host again in a NEW PROCESS: tls.library's cache is "
               "the library's, not curl's, so this must resume")
    https("rsa3.test", 2, body=("master", 0, 1024), w={"code": "200"})
    https("rsa4.test", 3, body=("master", 0, 1024), w={"code": "200"},
          note="four certificates sent, four signatures verified")
    https("ec2.test", 4, body=("master", 0, 1024), w={"code": "200"},
          note="ECDHE-ECDSA, and the slowest arithmetic on the machine")
    https("ec3.test", 5, body=("master", 0, 1024), w={"code": "200"})

    add("e07_wrong_host", "E",
        '%s -sS %s --resolve wrong.test:%d:%s -o DH0:d/e07.bin -w "%s" '
        '"https://wrong.test:%d/bytes/1024"'
        % (curl, cacert, base + 1, HOST, WFMT, base + 1), rc=60,
        note="the rsa2.test certificate, asked for under another name")
    add("e08_wrong_host_insecure", "E",
        '%s -sS -k %s --resolve wrong.test:%d:%s -o DH0:d/e08_wrong_host_'
        'insecure.bin -w "%s" "https://wrong.test:%d/bytes/1024"'
        % (curl, cacert, base + 1, HOST, WFMT, base + 1),
        body=("master", 0, 1024), w={"code": "200"},
        note="and with -k, because a backend that refused everything would "
             "pass the case above for the wrong reason")
    add("e09_expired", "E",
        '%s -sS %s --resolve expired.test:%d:%s -o DH0:d/e09.bin -w "%s" '
        '"https://expired.test:%d/bytes/1024"'
        % (curl, cacert, base + 6, HOST, WFMT, base + 6), rc=60)
    add("e10_selfsigned", "E",
        '%s -sS %s --resolve selfsigned.test:%d:%s -o DH0:d/e10.bin -w "%s" '
        '"https://selfsigned.test:%d/bytes/1024"'
        % (curl, cacert, base + 7, HOST, WFMT, base + 7), rc=60)
    add("e13_tls_on_plain_port", "E",
        '%s -sS %s --resolve rsa2.test:%d:%s -o DH0:d/e13.bin -w "%s" '
        '"https://rsa2.test:%d/bytes/1024"'
        % (curl, cacert, base, HOST, WFMT, base), rc=(35, 56, 28, 52),
        note="a TLS handshake into a plain HTTP server")
    add("e14_tls_refused", "E",
        '%s -sS %s --resolve rsa2.test:%d:%s -o DH0:d/e14.bin -w "%s" '
        '"https://rsa2.test:%d/bytes/1024"'
        % (curl, cacert, base + 99, HOST, WFMT, base + 99), rc=7)
    add("e15_tls_rst_at_accept", "E",
        '%s -sS %s --resolve rsa2.test:%d:%s -o DH0:d/e15.bin -w "%s" '
        '"https://rsa2.test:%d/bytes/1024"'
        % (curl, cacert, base + 21, HOST, WFMT, base + 21),
        rc=(35, 56, 52, 55, 7))
    add("e16_tls_silent", "E",
        '%s -sS %s --max-time 30 --resolve rsa2.test:%d:%s -o DH0:d/e16.bin '
        '-w "%s" "https://rsa2.test:%d/bytes/1024"'
        % (curl, cacert, base + 22, HOST, WFMT, base + 22), rc=(28, 35),
        note="accepted and silent through the handshake.  --max-time cannot "
             "fire inside a blocking TLSOpen(); TLSA_Timeout is what ends it, "
             "so the exit code says which mechanism won")

    add("e17_tls_reuse", "E",
        '%s -sS %s --resolve rsa2.test:%d:%s -o DH0:d/e17_a.bin '
        '"https://rsa2.test:%d/bytes/4096" -o DH0:d/e17_b.bin '
        '"https://rsa2.test:%d/bytes/8192" -w "%s"'
        % (curl, cacert, base + 1, HOST, base + 1, base + 1, WFMT),
        body=("multi", [("e17_a.bin", ("master", 0, 4096)),
                        ("e17_b.bin", ("master", 0, 8192))]),
        w={"conns_total": "1", "n": "2"},
        note="two requests, one TLS connection, one handshake")
    add("e18_tls_large", "E",
        '%s -sS %s --resolve rsa2.test:%d:%s -o DH0:d/e18_tls_large.bin '
        '-w "%s" "https://rsa2.test:%d/bytes/524288"'
        % (curl, cacert, base + 1, HOST, WFMT, base + 1),
        body=("master", 0, 524288), w={"code": "200", "size": "524288"},
        note="half a megabyte through the record layer, which is the only way "
             "to catch a record-boundary bug")
    add("e19_tls_chunked", "E",
        '%s -sS %s --resolve rsa2.test:%d:%s -o DH0:d/e19_tls_chunked.bin '
        '-w "%s" "https://rsa2.test:%d/chunked/131072"'
        % (curl, cacert, base + 1, HOST, WFMT, base + 1),
        body=("master", 0, 131072), w={"code": "200"})
    add("e20_tls_post", "E",
        '%s -sS %s --resolve rsa2.test:%d:%s --data-binary @DH0:up200k.bin '
        '-o DH0:d/e20_tls_post.bin -w "%s" "https://rsa2.test:%d/upload"'
        % (curl, cacert, base + 1, HOST, WFMT, base + 1),
        body=("uploadecho", 204800), w={"code": "200", "up": "204800"})
    add("e21_tls_parallel", "E",
        '%s -sS %s -Z --parallel-max 3 --resolve rsa2.test:%d:%s %s -w "%s"'
        % (curl, cacert, base + 1, HOST,
           " ".join('-o DH0:d/e21_%d.bin "https://rsa2.test:%d/bytes/%d"'
                    % (i, base + 1, 9000 + i) for i in range(3)), WFMT),
        body=("multi", [("e21_%d.bin" % i, ("master", 0, 9000 + i))
                        for i in range(3)]),
        note="three concurrent TLS transfers.  The handshake is blocking, so "
             "this is the case that says what that costs the multi loop")
    add("e23_wrong_cacert_warm", "E",
        '%s -sS --cacert DH0:otherstore --resolve rsa2.test:%d:%s '
        '-o DH0:d/e23.bin -w "%s" "https://rsa2.test:%d/bytes/1024"'
        % (curl, base + 1, HOST, WFMT, base + 1), rc=60,
        note="the same wrong trust store as e01, after the cache is warm.  "
             "A resumed handshake sends no certificate and verifies nothing, "
             "and tls.library keys its cache on the host name alone -- so "
             "this is where a trust decision made under one --cacert gets "
             "reused under another")
    add("e24_default_store_warm", "E",
        '%s -sS --resolve rsa2.test:%d:%s -o DH0:d/e24.bin -w "%s" '
        '"https://rsa2.test:%d/bytes/1024"' % (curl, base + 1, HOST, WFMT,
                                               base + 1), rc=60,
        note="and with no --cacert at all")
    add("e22_after_tls", "E",
        '%s -sS -o DH0:d/e22_after_tls.bin -w "%s" "%s/bytes/16384"'
        % (curl, WFMT, http), body=("master", 0, 16384),
        w={"code": "200", "size": "16384"},
        note="plain HTTP after all of the TLS, for the same reason as c16")

    # ------------------------------------------------------------- F: FTP ---

    ftp = "ftp://amiga:test@%s:%d" % (HOST, base + 10)

    add("f01_ftp_get", "F",
        '%s -sS -o DH0:d/f01_ftp_get.bin -w "%s" "%s/blob.bin"'
        % (curl, WFMT, ftp), body=("master", 0, 131072),
        note="a second connection for the data, opened while the control "
             "connection stays up")
    add("f02_ftp_get_big", "F",
        '%s -sS -o DH0:d/f02_ftp_get_big.bin -w "%s" "%s/big.bin"'
        % (curl, WFMT, ftp), body=("master", 0, 524288))
    add("f03_ftp_list", "F",
        '%s -sS -o DH0:d/f03.bin -w "%s" "%s/"' % (curl, WFMT, ftp))
    add("f04_ftp_nlst", "F",
        '%s -sS -l -o DH0:d/f04.bin -w "%s" "%s/"' % (curl, WFMT, ftp))
    add("f05_ftp_put", "F",
        '%s -sS -T DH0:up200k.bin -o DH0:d/f05.bin -w "%s" "%s/roundtrip.bin"'
        % (curl, WFMT, ftp), w={"up": "204800"})
    add("f06_ftp_put_back", "F",
        '%s -sS -o DH0:d/f06_ftp_put_back.bin -w "%s" "%s/roundtrip.bin"'
        % (curl, WFMT, ftp), body=("master", 0, 204800),
        note="reads back what f05 uploaded, so the upload is checked byte "
             "for byte without the host having to be asked")
    add("f07_ftp_active", "F",
        '%s -sS -P %s:%d-%d -o DH0:d/f07_ftp_active.bin -w "%s" '
        '"%s/blob.bin"' % (curl, GUEST, base + 60, base + 63, WFMT, ftp),
        body=("master", 0, 131072),
        note="active mode: the Amiga binds, listens and accepts an inbound "
             "connection.  A RANGE of four ports, not one, because the "
             "previous data connection is still in TIME_WAIT and curl gives "
             "up with (30) rather than waiting")
    add("f08_ftp_missing", "F",
        '%s -sS -o DH0:d/f08.bin -w "%s" "%s/nosuchfile.bin"'
        % (curl, WFMT, ftp), rc=78)
    add("f09_ftp_refused", "F",
        '%s -sS -o DH0:d/f09.bin -w "%s" "ftp://%s:%d/blob.bin"'
        % (curl, WFMT, HOST, base + 99), rc=7)
    add("f10_ftp_range", "F",
        '%s -sS -r 1024-5119 -o DH0:d/f10_ftp_range.bin -w "%s" "%s/blob.bin"'
        % (curl, WFMT, ftp), body=("master", 1024, 4096))
    add("f11_after_ftp", "F",
        '%s -sS -o DH0:d/f11_after_ftp.bin -w "%s" "%s/bytes/16384"'
        % (curl, WFMT, http), body=("master", 0, 16384),
        w={"code": "200", "size": "16384"})

    return cases


def build_probe(base, curl, spec):
    """A concurrency sweep, for when the suite has already found the cliff and
    the question is where exactly it is.

    Each step is the same forty-odd transfers at a different --parallel-max,
    so the only variable is how many sockets are open at once.
    """
    http = "http://%s:%d" % (HOST, base)
    cases = []
    for i, raw in enumerate(spec.split(",")):
        n = int(raw)
        lo = 5000 + i * 100
        cases.append(Case(
            "p%02d_parallel_%d" % (i, n), "P",
            '%s -sS -Z --parallel-max %d -o "DH0:g1/#1.bin" -w "%s" '
            '"%s/bytes/[%d-%d]"' % (curl, n, WFMT, http, lo, lo + n - 1),
            body=("glob", "g1", lo, lo + n),
            note="%d concurrent transfers" % n))
    return cases


AMINET_URL = "http://ftp.fau.de/aminet/comm/tcp/AmiTCP-SDK-4.3.lha"


def build_internet(curl, reference=None):
    """NOT A BASELINE.  Needs the internet and FS-UAE's SLIRP NAT."""
    cases = []

    def add(name, cmd, **kw):
        cases.append(Case(name, "G", cmd, internet=True, **kw))

    add("g01_example_http",
        '%s -sS -o DH0:d/g01.bin -w "%s" "http://example.com/"'
        % (curl, WFMT), w={"code": "200"})
    add("g02_badssl_rsa",
        '%s -sS -o DH0:d/g02.bin -w "%s" "https://tls-v1-2.badssl.com:1012/"'
        % (curl, WFMT), w={"code": "200"})
    add("g03_badssl_rsa_again",
        '%s -sS -o DH0:d/g03.bin -w "%s" "https://tls-v1-2.badssl.com:1012/"'
        % (curl, WFMT), w={"code": "200"},
        note="must resume: the cache crosses processes")
    add("g04_badssl_ecc",
        '%s -sS -o DH0:d/g04.bin -w "%s" "https://ecc256.badssl.com/"'
        % (curl, WFMT), w={"code": "200"})
    add("g05_badssl_wrong_host",
        '%s -sS -o DH0:d/g05.bin -w "%s" "https://wrong.host.badssl.com/"'
        % (curl, WFMT), rc=60)
    add("g06_aminet_lha",
        '%s -sS -o DH0:d/g06_aminet_lha.bin -w "%s" '
        '"%s"' % (curl, WFMT, AMINET_URL),
        w={"code": "200"},
        body=("hostfile", reference) if reference else None,
        note="657,797 bytes over the real internet, hashed against the copy "
             "the runner fetched here")
    add("g07_dns_nxdomain",
        '%s -sS -o DH0:d/g07.bin -w "%s" "http://no.such.host.invalid/"'
        % (curl, WFMT), rc=6)
    return cases


# ------------------------------------------------------------------ emit ----


def emit(cases, path):
    with open(path, "w") as fh:
        fh.write("# generated by tests/curl/curlsuite.py -- do not edit\n")
        group = None
        for c in cases:
            cost = len(c.cmd) + len(c.name) + 26
            if cost > SHELL_LINE_BUDGET:
                raise SystemExit(
                    "%s would be a %d-character Shell line; the budget is %d "
                    "and a truncated command fails in a way that looks like a "
                    "stack bug" % (c.name, cost, SHELL_LINE_BUDGET))
            if c.group != group:
                group = c.group
                fh.write("# ---- group %s ----\n" % group)
            fh.write("%s\t%s\n" % (c.name, c.cmd))
    return len(cases)


# ----------------------------------------------------------------- score ----

W_RE = re.compile(r"^W:(.*)$", re.M)


def parse_w(text):
    """Every -w line in one command's output, reduced to one dict.

    curl writes -w ONCE PER TRANSFER, and %{num_connects} is that transfer's
    own count -- so a command that fetches four URLs down one connection ends
    with num_connects=0 and the interesting number is the sum.  Both are
    offered: the last line's fields under their own names, plus `n` (how many
    transfers reported) and `conns_total` (how many connections the whole
    command made), which is the one that says whether reuse happened.
    """
    rows = []
    for m in W_RE.finditer(text):
        parts = m.group(1).split(",")
        if len(parts) == len(WFIELDS):
            rows.append(dict(zip(WFIELDS, parts)))
    if not rows:
        return None

    out = dict(rows[-1])
    out["n"] = str(len(rows))
    total = 0
    for row in rows:
        try:
            total += int(row["conns"])
        except ValueError:
            pass
    out["conns_total"] = str(total)
    return out


def expected_bytes(spec):
    kind = spec[0]
    if kind == "master":
        return MASTER[spec[1]:spec[1] + spec[2]]
    if kind == "literal":
        return spec[1]
    raise ValueError(kind)


def check_body(hd, name, spec, fail):
    kind = spec[0]

    if kind in ("master", "literal"):
        path = os.path.join(hd, "d", name + ".bin")
        want = expected_bytes(spec)
        if not os.path.exists(path):
            fail("no body at DH0:d/%s.bin" % name)
            return
        got = open(path, "rb").read()
        if got != want:
            fail("body differs: %d bytes, want %d, sha %s vs %s"
                 % (len(got), len(want),
                    hashlib.sha256(got).hexdigest()[:16],
                    hashlib.sha256(want).hexdigest()[:16]))
        return

    if kind == "hostfile":
        # The internet cases have no seeded buffer to compare against, so the
        # runner fetches the same URL here and the two are hashed.  Absent
        # reference, say so rather than passing silently.
        path = os.path.join(hd, "d", name + ".bin")
        ref = spec[1]
        if not os.path.exists(ref):
            fail("no host reference at %s (the runner did not fetch it)" % ref)
            return
        if not os.path.exists(path):
            fail("no body at DH0:d/%s.bin" % name)
            return
        a = hashlib.sha256(open(path, "rb").read()).hexdigest()
        b = hashlib.sha256(open(ref, "rb").read()).hexdigest()
        if a != b:
            fail("sha256 %s, the host got %s" % (a[:16], b[:16]))
        return

    if kind == "uploadecho":
        path = os.path.join(hd, "d", name + ".bin")
        if not os.path.exists(path):
            fail("no body at DH0:d/%s.bin" % name)
            return
        got = open(path, "rb").read().decode("latin-1")
        want = "len=%d\nsha256=%s\n" % (
            spec[1], hashlib.sha256(master(spec[1])).hexdigest())
        if got != want:
            fail("the server did not receive what we sent: %r" % got[:120])
        return

    if kind == "multi":
        for fname, sub in spec[1]:
            path = os.path.join(hd, "d", fname)
            want = expected_bytes(sub)
            if not os.path.exists(path):
                fail("no body at DH0:d/%s" % fname)
                continue
            if open(path, "rb").read() != want:
                fail("DH0:d/%s differs" % fname)
        return

    if kind == "glob":
        _, sub, lo, hi = spec
        bad = 0
        missing = 0
        for n in range(lo, hi):
            path = os.path.join(hd, sub, "%d.bin" % n)
            if not os.path.exists(path):
                missing += 1
                continue
            if open(path, "rb").read() != MASTER[:n]:
                bad += 1
        if missing or bad:
            fail("%d of %d missing, %d wrong in DH0:%s/"
                 % (missing, hi - lo, bad, sub))
        return

    raise ValueError(kind)


def score(cases, hd, verbose=True):
    results = {}
    order = []
    rpath = os.path.join(hd, "results.txt")
    if os.path.exists(rpath):
        for ln in open(rpath):
            parts = ln.split()
            if len(parts) >= 4:
                results[parts[0]] = (int(parts[1]), int(parts[2]),
                                     int(parts[3]))
                order.append(parts[0])

    passed = failed = 0
    failures = []

    for c in cases:
        problems = []

        def fail(msg, _p=problems):
            _p.append(msg)

        got = results.get(c.name)
        if got is None:
            fail("no result line -- the run did not reach this case")
        else:
            rc, ticks, avail = got
            if rc not in c.rc:
                fail("rc %d, want %s" % (rc, "/".join(str(r) for r in c.rc)))

            wpath = os.path.join(hd, "w", c.name + ".txt")
            text = ""
            if os.path.exists(wpath):
                text = open(wpath, "rb").read().decode("latin-1")

            if c.w:
                w = parse_w(text)
                if w is None:
                    fail("no -w line in DH0:w/%s.txt" % c.name)
                else:
                    for k, v in c.w.items():
                        if w.get(k) != v:
                            fail("%s=%s, want %s" % (k, w.get(k), v))

            if c.contains and c.contains not in text:
                fail("output does not contain %r" % c.contains)
            if c.absent and c.absent in text:
                fail("output contains %r and should not" % c.absent)

            if c.body is not None and rc in c.rc:
                check_body(hd, c.name, c.body, fail)

        if problems:
            failed += 1
            failures.append((c, problems))
            if verbose:
                print("FAIL %-24s %s" % (c.name, "; ".join(problems)))
        else:
            passed += 1
            if verbose:
                rc, ticks, avail = results[c.name]
                print("pass %-24s rc %-3d %6.2fs" % (c.name, rc, ticks / 50.0))

    return passed, failed, failures, results, order


def memory_trend(results, order):
    """AvailMem across the run, which is how a per-socket leak shows up.

    The setup cases are skipped: the first open of bsdsocket.library brings
    NetX Duo up and costs half a megabyte, which is an allocation and not a
    leak.  What matters is whether the number moves after that.
    """
    order = [n for n in order if not n.startswith("s0")]
    if len(order) < 4:
        return None
    first = results[order[0]][2]
    last = results[order[-1]][2]
    low = min(results[n][2] for n in order)
    return first, last, low


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit", metavar="FILE")
    ap.add_argument("--score", metavar="TESTHD")
    ap.add_argument("--groups", default="ABCDEF")
    ap.add_argument("--base-port", type=int, default=7100)
    ap.add_argument("--curl", default="SYS:curl")
    ap.add_argument("--cacert", default="--cacert DH0:teststore")
    ap.add_argument("--reference", metavar="FILE",
                    help="host copy of the internet suite's large download, "
                         "for the byte-for-byte comparison")
    ap.add_argument("--only", metavar="SUBSTRING",
                    help="keep only cases whose name contains this, on top "
                         "of --groups.  For chasing one failure without "
                         "paying for the other 140")
    ap.add_argument("--probe", metavar="SPEC",
                    help="generate a concurrency sweep instead of the suite: "
                         "a comma-separated list of --parallel-max values")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    if args.probe:
        cases = build_setup(args.curl) + build_probe(args.base_port,
                                                     args.curl, args.probe)
    elif "G" in args.groups:
        cases = build_setup(args.curl) + build_internet(args.curl,
                                                         args.reference)
    else:
        cases = build_setup(args.curl) + [
            c for c in build(args.base_port, args.curl, args.cacert)
            if c.group in args.groups]

    if args.only:
        cases = [c for c in cases
                 if c.group == "S" or args.only in c.name]

    if args.list:
        for c in cases:
            print("%-24s %s" % (c.name, c.cmd))
        return 0

    if args.emit:
        n = emit(cases, args.emit)
        print("==> %d cases in %s (groups %s)" % (n, args.emit, args.groups))
        return 0

    if args.score:
        passed, failed, failures, results, order = score(cases, args.score)
        print()
        trend = memory_trend(results, order)
        if trend:
            print("AvailMem: %d at the first case, %d at the last, %d lowest "
                  "(delta %+d)" % (trend[0], trend[1], trend[2],
                                   trend[1] - trend[0]))
        print("==> %d passed, %d failed, %d cases"
              % (passed, failed, passed + failed))
        return 1 if failed else 0

    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
