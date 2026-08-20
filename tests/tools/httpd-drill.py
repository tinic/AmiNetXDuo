#!/usr/bin/env python3
#
# httpd-drill, the WebDAV assertions that need a socket rather than a client.
#
#   tests/tools/httpd-drill.py [--terminal] ADDRESS [PORT]
#
# WHY THIS IS NOT curl
#
#   Every check here is about what the server does with the SOCKET, and curl
#   opens a fresh one per request and hides the framing.  The one that matters
#   most cannot be written any other way: a refused request whose body holds
#   another request's text, and then the question of whether that text was
#   executed.  curl would send the body and read the refusal and report
#   success either way.
#
#   So the requests are written out as bytes and the answers read back off the
#   same connection.  The server is whatever is listening at ADDRESS, the
#   emulated Amiga that tests/tools/run-httpd.sh puts on the wire.
#
# WHAT IT NEEDS OF THE SERVER
#
#   A writable document root.  Everything it makes it removes, under a drawer
#   of its own, so it can be run against a share somebody is using.
#
#   --terminal adds the WebSocket half, which needs a server started with
#   TERMINAL.  It is a flag rather than a probe because "the terminal is off"
#   and "the terminal is broken" look the same from out here, and a suite that
#   skips itself when the thing it tests is missing is a suite that passes on a
#   server with nothing in it.  tests/tools/run-wsterm.sh passes it.
#
#   --gz-url=<path> says where the -T page's compressed sibling can be reached
#   in the SERVED drawer, e.g. /shell.html.gz.  With it, that file is moved
#   out of the way over WebDAV and put back, which is how the state of a -T
#   page nobody compressed is produced from out here.  It needs a harness that
#   put the page inside the document root; tests/tools/run-wsterm.sh does.
#   Without it that one check is skipped and says so.
#
# SPDX-License-Identifier: MIT

import base64
import gzip
import hashlib
import os
import socket
import struct
import sys
import time

argv = [a for a in sys.argv[1:] if not a.startswith("--")]
WANT_TERMINAL = "--terminal" in sys.argv or "--ws-only" in sys.argv

# Where the -T page's .gz can be reached in the served drawer, for the one
# check that has to change what is on the guest's disk.  "" when nobody said.
GZ_URL = ""
for _a in sys.argv[1:]:
    if _a.startswith("--gz-url="):
        GZ_URL = _a[len("--gz-url="):]

# --ws-only skips the WebDAV half and its setup.  It exists so the WebSocket
# assertions can be pointed at something that is NOT this server: every one of
# them was watched to fail against a stand-in that is deliberately wrong in one
# named way, and that stand-in answers no PROPFIND, so setup() would exit(2)
# before a single WebSocket check ran.  run-wsterm.sh does not use it -- it
# passes --terminal, which runs both halves.
WS_ONLY = "--ws-only" in sys.argv

ADDR = argv[0] if len(argv) > 0 else "127.0.0.1"
PORT = int(argv[1]) if len(argv) > 1 else 8080

BASE = "/httpd-drill"
TERM = "/shell"

# How long one exchange with the guest may take.  Measured rather than
# guessed: see tests/tools/run-wsterm.sh for the figures a 68020 produced and
# the multiple applied to them.  A run that burns one of these has found a
# defect and says which exchange it was; raising it is never the fix.
WS_WAIT = float(os.environ.get("AMINETXDUO_WS_WAIT", "20"))

checks = 0
failures = []


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)
        print("  FAIL %s" % what)


class Conn:
    """One connection, held open across requests on purpose."""

    def __init__(self, timeout=10):
        self.s = socket.create_connection((ADDR, PORT), timeout=timeout)
        self.buf = b""

    def send(self, data):
        if isinstance(data, str):
            data = data.encode("latin-1")
        self.s.sendall(data)

    def fill(self):
        try:
            more = self.s.recv(4096)
        except socket.timeout:
            return False
        if not more:
            return False
        self.buf += more
        return True

    def answer(self):
        """One response: status, headers, body.  None when the peer hung up
        without sending one."""
        while b"\r\n\r\n" not in self.buf:
            if not self.fill():
                return None
        head, rest = self.buf.split(b"\r\n\r\n", 1)
        lines = head.decode("latin-1").split("\r\n")
        status = int(lines[0].split()[1])
        headers = {}
        for line in lines[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                headers[k.strip().lower()] = v.strip()

        length = headers.get("content-length")
        if headers.get("transfer-encoding", "").lower() == "chunked":
            # Read to the terminating zero chunk; the bodies here are small.
            while b"0\r\n\r\n" not in rest:
                self.buf = rest
                if not self.fill():
                    break
                rest = self.buf
            body = rest
            rest = b""
        elif length is not None:
            n = int(length)
            while len(rest) < n:
                self.buf = rest
                if not self.fill():
                    break
                rest = self.buf
            body, rest = rest[:n], rest[n:]
        else:
            body, rest = rest, b""

        self.buf = rest
        return status, headers, body

    def closed(self):
        """True when the peer has hung up."""
        self.s.settimeout(3)
        try:
            return self.s.recv(1) == b""
        except socket.timeout:
            return False
        except OSError:
            return True

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def once(request, body=b""):
    """A request on a connection of its own."""
    c = Conn()
    c.send(request)
    if body:
        c.send(body)
    a = c.answer()
    c.close()
    return a


def parts(method, path, headers=None, body=b"", keepalive=True):
    """The head and the body separately, so a test can put a segment boundary
    between them.  That boundary is what the desync needs: a body that arrives
    WITH the head is already in the server's buffer and gets discarded with
    it, and only a body still in the socket is read as the next request."""
    if isinstance(body, str):
        body = body.encode("latin-1")
    lines = ["%s %s HTTP/1.1" % (method, path),
             "Host: %s:%d" % (ADDR, PORT)]
    if keepalive:
        lines.append("Connection: keep-alive")
    for k, v in (headers or {}).items():
        lines.append("%s: %s" % (k, v))
    if body:
        lines.append("Content-Length: %d" % len(body))
    return "\r\n".join(lines).encode("latin-1") + b"\r\n\r\n", body


def req(method, path, headers=None, body=b"", keepalive=True):
    head, tail = parts(method, path, headers, body, keepalive)
    return head + tail


# --------------------------------------------------------------- the ground --

def setup():
    once(req("DELETE", BASE))
    a = once(req("MKCOL", BASE))
    if a is None or a[0] not in (201, 405):
        print("!! cannot make %s on the server: %s" % (BASE, a))
        sys.exit(2)
    a = once(req("PUT", BASE + "/keepme.txt", body="this file must survive\n"))
    if a is None or a[0] not in (200, 201, 204):
        print("!! cannot write into %s: %s" % (BASE, a))
        sys.exit(2)


def teardown():
    once(req("DELETE", BASE))


# ------------------------------------------------------- the refused bodies --

def test_desync():
    """A refused request's body must not be read as the next request.

    The body here IS a DELETE of a file that exists.  Before the fix the
    server answered 405, left the body in the socket, and parsed it as a
    fresh request, so the file went."""
    print("a refused body is not the next request")

    poison = ("DELETE %s/keepme.txt HTTP/1.1\r\n"
              "Host: %s:%d\r\n\r\n" % (BASE, ADDR, PORT))

    for method, why in (("POST", "a verb this server has not"),
                        ("GET", "a body on a method that takes none")):
        c = Conn()
        head, tail = parts(method, BASE + "/keepme.txt", body=poison)
        c.send(head)
        time.sleep(1.5)             # the body is its own segment
        c.send(tail)
        a = c.answer()
        check(a is not None, "%s with a body was answered" % method)
        if a is not None:
            check(a[0] in (400, 405),
                  "%s with a body is refused, not %s (%s)"
                  % (method, a[0] if a else "?", why))

        # Whatever the server does with the connection, the DELETE in the
        # body must not have been answered on it.
        second = c.answer()
        check(second is None,
              "%s: nothing after the refusal on that connection (got %s)"
              % (method, second[0] if second else "nothing"))

        c.close()
        time.sleep(0.2)
        a = once(req("HEAD", BASE + "/keepme.txt"))
        check(a is not None and a[0] == 200,
              "the DELETE inside a refused %s body did not run" % method)
        if a is None or a[0] != 200:
            # Put it back, so the rest of the drill has its ground.
            once(req("PUT", BASE + "/keepme.txt",
                     body="this file must survive\n"))

    # An address the server will not open is the third refusal that used to
    # keep the connection: a colon is an AmigaOS device reference.
    c = Conn()
    head, tail = parts("PUT", "/RAM:drill", body=poison)
    c.send(head)
    time.sleep(1.5)
    c.send(tail)
    a = c.answer()
    check(a is not None and a[0] in (400, 403),
          "a refused address answers 403")
    second = c.answer()
    check(second is None,
          "and nothing after it (got %s)" % (second[0] if second else "nothing"))
    c.close()
    time.sleep(0.2)
    a = once(req("HEAD", BASE + "/keepme.txt"))
    check(a is not None and a[0] == 200,
          "the DELETE inside a refused address's body did not run")


def test_refusal_keeps_the_connection():
    """A refusal the client can recover from should not cost a reconnect."""
    print("a drainable refusal keeps the connection")

    c = Conn()
    head, tail = parts("POST", BASE + "/keepme.txt", body="a" * 64)
    c.send(head)
    time.sleep(1.5)                 # so the body has to be drained from the
    c.send(tail)                    # socket rather than from the buffer
    a = c.answer()
    check(a is not None and a[0] == 405, "POST is 405")
    if a is not None:
        check(a[1].get("connection", "") == "keep-alive",
              "and says keep-alive")

    # The body has to be gone from the socket, so this next request is read as
    # a request and answered.
    c.send(req("HEAD", BASE + "/keepme.txt"))
    a = c.answer()
    check(a is not None and a[0] == 200,
          "the next request on that connection is answered")
    c.close()


def test_unframed_refusal_closes():
    """A refusal made before the headers were read cannot know whether there
    is a body, so it has to close."""
    print("a refusal before the headers closes")

    c = Conn()
    c.send(b"THISMETHODNAMEISFARTOOLONGTOBEONE / HTTP/1.1\r\n"
           b"Host: x\r\nContent-Length: 4\r\n\r\n")
    time.sleep(1.5)
    c.send(b"abcd")
    a = c.answer()
    check(a is not None and a[0] == 501, "a long method is 501")
    if a is not None:
        check(a[1].get("connection", "") == "close", "and closes")
    check(c.closed(), "and the server hung up")
    c.close()


# ------------------------------------------------------------------- etags --

def test_etags():
    print("entity tags")

    a = once(req("HEAD", BASE + "/keepme.txt"))
    check(a is not None and "etag" in a[1], "HEAD carries an ETag")
    etag = a[1].get("etag", "") if a else ""
    check(etag.startswith('"') and etag.endswith('"'),
          "and it is quoted: %r" % etag)

    a = once(req("GET", BASE + "/keepme.txt"))
    check(a is not None and a[1].get("etag") == etag,
          "GET agrees with HEAD")

    a = once(req("PROPFIND", BASE + "/keepme.txt", {"Depth": "0"}))
    check(a is not None and b"getetag" in a[2], "PROPFIND reports getetag")
    if a is not None and etag:
        check(etag.encode("latin-1") in a[2],
              "and it is the same tag")

    # The tag has to change when the bytes do.
    once(req("PUT", BASE + "/keepme.txt", body="this file must survive!!\n"))
    a = once(req("HEAD", BASE + "/keepme.txt"))
    check(a is not None and a[1].get("etag") != etag,
          "and it changes when the file does")


# ---------------------------------------------------------------------- If --

def test_if_header():
    print("If: is evaluated")

    # A token nobody is holding.  Both of these answered 201 before.
    a = once(req("PUT", BASE + "/cond.txt",
                 {"If": "(<opaquelocktoken:deadbeef>)"}, body="no"))
    check(a is not None and a[0] == 412,
          "a token nobody holds is 412, not %s" % (a[0] if a else "?"))

    a = once(req("PUT", BASE + "/cond.txt",
                 {"If": "(Not <opaquelocktoken:deadbeef>)"}, body="yes"))
    check(a is not None and a[0] in (200, 201, 204),
          "Not, against a token nobody holds, goes through")

    # An entity tag that is right, and one that is not.
    a = once(req("HEAD", BASE + "/cond.txt"))
    etag = a[1].get("etag", "") if a else ""

    if etag:
        a = once(req("PUT", BASE + "/cond.txt", {"If": "([%s])" % etag},
                     body="still yes"))
        check(a is not None and a[0] in (200, 201, 204),
              "the file's own tag goes through")

        a = once(req("PUT", BASE + "/cond.txt", {"If": "([\"0-0-0-0\"])"},
                     body="no"))
        check(a is not None and a[0] == 412, "somebody else's tag is 412")

        # The PUT above moved the tag, so read it again rather than asserting
        # about the one the file no longer has.
        a = once(req("HEAD", BASE + "/cond.txt"))
        etag = a[1].get("etag", "") if a else ""

        a = once(req("PUT", BASE + "/cond.txt",
                     {"If": "(Not [%s])" % etag}, body="no"))
        check(a is not None and a[0] == 412, "Not its own tag is 412")

    # A held token, and Not against it.
    lock = once(req("LOCK", BASE + "/cond.txt", {"Timeout": "Second-60"},
                    body='<?xml version="1.0" encoding="utf-8"?>'
                         '<D:lockinfo xmlns:D="DAV:">'
                         '<D:lockscope><D:exclusive/></D:lockscope>'
                         '<D:locktype><D:write/></D:locktype>'
                         '<D:owner>drill</D:owner></D:lockinfo>'))
    token = ""
    if lock is not None and lock[0] in (200, 201):
        token = lock[1].get("lock-token", "").strip("<>")

    check(token != "", "LOCK hands out a token")

    if token:
        a = once(req("PUT", BASE + "/cond.txt", {"If": "(<%s>)" % token},
                     body="held"))
        check(a is not None and a[0] in (200, 201, 204),
              "the held token goes through")

        a = once(req("PUT", BASE + "/cond.txt", {"If": "(Not <%s>)" % token},
                     body="no"))
        check(a is not None and a[0] == 412, "Not the held token is 412")

        once(req("UNLOCK", BASE + "/cond.txt", {"Lock-Token": "<%s>" % token}))

    once(req("DELETE", BASE + "/cond.txt"))


# ------------------------------------------------------------------- locks --

def test_delete_destroys_locks():
    print("DELETE destroys the locks on what it removed")

    once(req("PUT", BASE + "/gone.txt", body="temporary"))

    lock = once(req("LOCK", BASE + "/gone.txt", {"Timeout": "Second-600"},
                    body='<?xml version="1.0" encoding="utf-8"?>'
                         '<D:lockinfo xmlns:D="DAV:">'
                         '<D:lockscope><D:exclusive/></D:lockscope>'
                         '<D:locktype><D:write/></D:locktype>'
                         '<D:owner>drill</D:owner></D:lockinfo>'))
    token = ""
    if lock is not None and lock[0] in (200, 201):
        token = lock[1].get("lock-token", "").strip("<>")
    check(token != "", "the file locks")

    if not token:
        return

    a = once(req("DELETE", BASE + "/gone.txt",
                 {"If": "(<%s>)" % token}))
    check(a is not None and a[0] in (204, 207),
          "DELETE with the token goes through")

    # The name has to be usable again.  Before the fix this was 423 for the
    # whole timeout, for everybody including the client that deleted it.
    a = once(req("PUT", BASE + "/gone.txt", body="new"))
    check(a is not None and a[0] in (200, 201, 204),
          "and the name is usable again, not %s" % (a[0] if a else "?"))

    once(req("DELETE", BASE + "/gone.txt"))


def test_lock_below_stops_delete():
    print("a lock inside a drawer stops the drawer's DELETE")

    once(req("MKCOL", BASE + "/tree"))
    once(req("PUT", BASE + "/tree/inside.txt", body="locked"))

    lock = once(req("LOCK", BASE + "/tree/inside.txt",
                    {"Timeout": "Second-60"},
                    body='<?xml version="1.0" encoding="utf-8"?>'
                         '<D:lockinfo xmlns:D="DAV:">'
                         '<D:lockscope><D:exclusive/></D:lockscope>'
                         '<D:locktype><D:write/></D:locktype>'
                         '<D:owner>drill</D:owner></D:lockinfo>'))
    token = ""
    if lock is not None and lock[0] in (200, 201):
        token = lock[1].get("lock-token", "").strip("<>")

    if token:
        a = once(req("DELETE", BASE + "/tree"))
        check(a is not None and a[0] == 423,
              "DELETE of the drawer above is 423, not %s"
              % (a[0] if a else "?"))

        # Tagged, because an untagged list is about the request target and
        # the drawer itself is not what is locked, RFC 4918 10.4.1.
        a = once(req("DELETE", BASE + "/tree",
                     {"If": "<http://%s:%d%s/tree/inside.txt> (<%s>)"
                            % (ADDR, PORT, BASE, token)}))
        check(a is not None and a[0] in (204, 207),
              "and goes through with the token, tagged at what holds it")
    else:
        check(False, "the file inside locks")

    once(req("DELETE", BASE + "/tree"))


# --------------------------------------------------------------- proppatch --

def test_proppatch_atomic():
    print("PROPPATCH is all or none")

    once(req("PUT", BASE + "/props.txt", body="dated"))

    a = once(req("PROPFIND", BASE + "/props.txt", {"Depth": "0"}))
    before = a[2] if a else b""

    body = ('<?xml version="1.0" encoding="utf-8"?>'
            '<D:propertyupdate xmlns:D="DAV:" xmlns:Z="urn:drill">'
            '<D:set><D:prop>'
            '<D:getlastmodified>Mon, 01 Jan 2001 00:00:00 GMT'
            '</D:getlastmodified>'
            '<Z:nonsense>x</Z:nonsense>'
            '</D:prop></D:set></D:propertyupdate>')

    a = once(req("PROPPATCH", BASE + "/props.txt",
                 {"Content-Type": "text/xml"}, body=body))
    check(a is not None and a[0] == 207, "a mixed PROPPATCH is a 207")

    if a is not None:
        check(b"403" in a[2], "the unsettable name is 403")
        check(b"424" in a[2],
              "and the settable one is 424, not applied beside it")
        check(b"200 OK" not in a[2].replace(b"HTTP/1.1 200 OK</D:status>", b"")
              or b"HTTP/1.1 200 OK" not in a[2],
              "nothing in it reports 200")

    a = once(req("PROPFIND", BASE + "/props.txt", {"Depth": "0"}))
    after = a[2] if a else b""

    def modified(xml):
        i = xml.find(b"<D:getlastmodified>")
        return xml[i:i + 60] if i >= 0 else b""

    check(modified(before) == modified(after) and modified(before) != b"",
          "and the date did not move")

    # On its own it still works, or PROPPATCH would be useless.
    body = ('<?xml version="1.0" encoding="utf-8"?>'
            '<D:propertyupdate xmlns:D="DAV:">'
            '<D:set><D:prop>'
            '<D:getlastmodified>Mon, 01 Jan 2001 00:00:00 GMT'
            '</D:getlastmodified>'
            '</D:prop></D:set></D:propertyupdate>')

    a = once(req("PROPPATCH", BASE + "/props.txt",
                 {"Content-Type": "text/xml"}, body=body))
    check(a is not None and a[0] == 207 and b"200 OK" in a[2],
          "a request naming only settable properties is applied")

    once(req("DELETE", BASE + "/props.txt"))


# ------------------------------------------------------- overlapping moves --

def test_overlapping_moves():
    """Two MOVEs in flight at once used to share one resolved destination.

    The destinations are made to EXIST first, on purpose.  A MOVE onto a name
    nothing is using is one Rename inside the handler and never spans a pass
    of the event loop; one that has to clear the destination first walks, and
    the walk reads the destination back after the handler has returned, so
    that is the shape where a shared destination is two clients' at once."""
    print("two MOVEs at once keep their own destinations")

    for n in (1, 2):
        once(req("MKCOL", "%s/src%d" % (BASE, n)))
        once(req("MKCOL", "%s/dst%d" % (BASE, n)))
        for k in range(6):
            once(req("PUT", "%s/src%d/f%d.txt" % (BASE, n, k),
                     body="x" * 200))
            once(req("PUT", "%s/dst%d/old%d.txt" % (BASE, n, k),
                     body="y" * 200))

    a = Conn()
    b = Conn()
    a.send(req("MOVE", BASE + "/src1",
               {"Destination": "http://%s:%d%s/dst1" % (ADDR, PORT, BASE)}))
    b.send(req("MOVE", BASE + "/src2",
               {"Destination": "http://%s:%d%s/dst2" % (ADDR, PORT, BASE)}))

    ra = a.answer()
    rb = b.answer()
    a.close()
    b.close()

    check(ra is not None and ra[0] in (201, 204),
          "the first MOVE answered %s" % (ra[0] if ra else "nothing"))
    check(rb is not None and rb[0] in (201, 204),
          "the second MOVE answered %s" % (rb[0] if rb else "nothing"))

    for n in (1, 2):
        got = once(req("PROPFIND", "%s/dst%d" % (BASE, n), {"Depth": "1"}))
        check(got is not None and got[0] == 207,
              "dst%d is there" % n)
        if got is not None:
            check(got[2].count(b"<D:response>") == 7,
                  "dst%d has its own six files, not %d"
                  % (n, got[2].count(b"<D:response>") - 1))
            check(b"old0.txt" not in got[2],
                  "and what was at dst%d is gone" % n)
            check(("f0.txt" in got[2].decode("latin-1")),
                  "and src%d's files are the ones in it" % n)
        gone = once(req("HEAD", "%s/src%d" % (BASE, n)))
        check(gone is not None and gone[0] == 404,
              "src%d is gone" % n)

    for n in (1, 2):
        once(req("DELETE", "%s/dst%d" % (BASE, n)))


def test_destination_contains_source():
    """Overwrite must not erase the source before COPY/MOVE reads it."""
    print("a destination containing its source is refused intact")

    root = BASE + "/ancestor"
    source = root + "/source"
    keep = source + "/keep.txt"

    once(req("MKCOL", root))
    once(req("MKCOL", source))
    once(req("PUT", keep, body="must survive\n"))

    dest = {"Destination": "http://%s:%d%s" % (ADDR, PORT, root)}
    a = once(req("COPY", source, dest))
    check(a is not None and a[0] == 409,
          "COPY onto its ancestor is 409, not %s"
          % (a[0] if a else "nothing"))

    a = once(req("GET", keep))
    check(a is not None and a[0] == 200 and b"must survive" in a[2],
          "and refusing it left the source bytes intact")

    once(req("DELETE", root))


# ------------------------------------------------------ names that collide --

def test_name_truncation():
    """Two addresses that reach one file, which is what a filesystem storing
    30 characters of a name does with 44 of them.

    Only an OFS volume shows this; a directory filesystem keeps both names
    apart and the case reports itself as not applicable.  It is here because
    the failure it guards against is unrecoverable and invisible everywhere
    else: a DELETE of one long name removing the file behind the other."""
    print("names the filesystem cannot tell apart")

    shared = "abcdefghijklmnopqrstuvwxyz0123"        # exactly 30
    a = "%s/%sAAAAAAAAAA.txt" % (BASE, shared)
    b = "%s/%sBBBBBBBBBB.txt" % (BASE, shared)

    once(req("DELETE", a))
    once(req("DELETE", b))

    made = once(req("PUT", a, body="this is A and must survive\n"))

    if made is not None and made[0] == 400:
        # The volume truncates and the server refused to make a name it could
        # not then serve.  Nothing was created, so nothing can be reached.
        for path, what in ((a, "the name that was asked for"),
                           (b, "the one that would collide with it")):
            got = once(req("GET", path))
            check(got is not None and got[0] == 404,
                  "nothing was left behind under %s" % what)
        return

    check(made is not None and made[0] in (200, 201, 204),
          "the first long name was written")

    got = once(req("GET", b))

    if got is not None and got[0] == 404:
        print("  (this filesystem keeps both names apart; nothing to test)")
        once(req("DELETE", a))
        return

    # Two addresses, one file.  Every one of these used to act on A.
    check(got is not None and got[0] == 400,
          "GET of the colliding name is refused, not %s"
          % (got[0] if got else "?"))
    check(once(req("PROPFIND", b, {"Depth": "0"}))[0] == 400,
          "PROPFIND of it is refused")
    check(once(req("PUT", b, body="this is B\n"))[0] == 400,
          "PUT to it is refused")
    check(once(req("DELETE", b))[0] == 400,
          "DELETE of it is refused")

    survived = once(req("GET", a))
    check(survived is not None and survived[0] == 200 and
          b"this is A" in survived[2],
          "and the file behind both names is untouched")

    once(req("DELETE", a))


# --------------------------------------------- oversized listing members --

def test_listing_omission_yields():
    """An unrepresentable child does not turn enumeration into a busy loop.

    A row containing this many ampersands expands once in its href and again
    in its XML display name, past the server's one-chunk scratch on filesystems
    that retain long Amiga names.  Each omitted row is represented by its own
    harmless comment chunk.  That detail is intentional: one chunk means one
    return from the producer, and therefore one scheduling point for the
    server's other connections.  Merely completing the listing would not catch
    the old `continue`, which completed too after monopolising the event loop.
    """
    print("oversized listing members yield between directory entries")

    paths = [BASE + "/" + ("&" * 96) + ("%02d" % i) for i in range(4)]
    made = []

    for path in paths:
        answer = once(req("PUT", path, body="x"))
        if answer is not None and answer[0] in (200, 201, 204):
            made.append(path)

    if len(made) != len(paths):
        print("  (this filesystem cannot retain names long enough to expand "
              "past one listing chunk; not applicable)")
        for path in made:
            once(req("DELETE", path))
        return

    try:
        answer = once(req("PROPFIND", BASE, {"Depth": "1"}))
        marker = b"<!-- listing entry omitted: representation too large -->"
        check(answer is not None and answer[0] == 207,
              "the drawer remains listable with oversized members")
        if answer is not None:
            check(answer[2].count(marker) == len(paths),
                  "each omitted member consumes its own producer pass")
            check(b"keepme.txt" in answer[2],
                  "ordinary members remain in the same listing")
    finally:
        for path in paths:
            once(req("DELETE", path))


# ------------------------------------------------------- propfind bodies ---

def test_propfind_body():
    print("PROPFIND answers the body it was given")

    once(req("PUT", BASE + "/pf.txt", body="body"))

    # <propname/>: the names, and no values.
    body = ('<?xml version="1.0" encoding="utf-8"?>'
            '<D:propfind xmlns:D="DAV:"><D:propname/></D:propfind>')
    a = once(req("PROPFIND", BASE + "/pf.txt", {"Depth": "0",
                 "Content-Type": "text/xml"}, body=body))
    check(a is not None and a[0] == 207, "propname is a 207")
    if a is not None:
        check(b"getcontentlength" in a[2], "propname names getcontentlength")
        check(b"<D:getcontentlength>4</D:getcontentlength>" not in a[2],
              "and does not carry its value")

    # A named list: what was asked for, and nothing else.
    body = ('<?xml version="1.0" encoding="utf-8"?>'
            '<D:propfind xmlns:D="DAV:"><D:prop>'
            '<D:getcontentlength/></D:prop></D:propfind>')
    a = once(req("PROPFIND", BASE + "/pf.txt", {"Depth": "0",
                 "Content-Type": "text/xml"}, body=body))
    check(a is not None and a[0] == 207, "a named prop list is a 207")
    if a is not None:
        check(b"getcontentlength" in a[2], "the named property is reported")
        check(b"getcontenttype" not in a[2],
              "and one that was not named is not")

    # A property this server does not keep is 404, not silence.
    body = ('<?xml version="1.0" encoding="utf-8"?>'
            '<D:propfind xmlns:D="DAV:" xmlns:Z="urn:drill"><D:prop>'
            '<D:getcontentlength/><Z:nosuchprop/></D:prop></D:propfind>')
    a = once(req("PROPFIND", BASE + "/pf.txt", {"Depth": "0",
                 "Content-Type": "text/xml"}, body=body))
    if a is not None:
        check(b"404" in a[2], "an unknown named property draws a 404 propstat")
        check(b"nosuchprop" in a[2], "and the propstat names it")

    # No body still means everything, which is what every client sends.
    a = once(req("PROPFIND", BASE + "/pf.txt", {"Depth": "0"}))
    if a is not None:
        check(b"getcontenttype" in a[2] and b"getcontentlength" in a[2],
              "an empty body is still allprop")

    once(req("DELETE", BASE + "/pf.txt"))


def test_depth0_collection_lock():
    print("a Depth: 0 lock on a drawer holds its membership")

    once(req("MKCOL", BASE + "/locked"))

    lock = once(req("LOCK", BASE + "/locked", {"Timeout": "Second-600",
                     "Depth": "0"},
                    body='<?xml version="1.0" encoding="utf-8"?>'
                         '<D:lockinfo xmlns:D="DAV:">'
                         '<D:lockscope><D:exclusive/></D:lockscope>'
                         '<D:locktype><D:write/></D:locktype>'
                         '<D:owner>drill</D:owner></D:lockinfo>'))
    token = ""
    if lock is not None and lock[0] in (200, 201):
        i = lock[2].find(b"opaquelocktoken:")
        if i >= 0:
            j = lock[2].find(b"<", i)
            token = lock[2][i:j].decode("ascii", "replace").strip()
    check(token != "", "the collection lock hands out a token")

    # Somebody without the token may not add a name to it.
    a = once(req("PUT", BASE + "/locked/intruder.txt", body="x"))
    check(a is not None and a[0] == 423,
          "a PUT into it without the token is 423")

    a = once(req("MKCOL", BASE + "/locked/sub"))
    check(a is not None and a[0] == 423,
          "and so is a MKCOL inside it")

    # The holder may.  The list is TAGGED with the collection: RFC 4918
    # 10.4.1 evaluates an untagged list against the Request-URI, and the file
    # being created is not what the token locks, so untagged is a 412 and
    # correctly so.  This is the form a client that locked a drawer to add to
    # it actually sends.
    if token:
        tag = "<http://%s:%d%s/locked/>" % (ADDR, PORT, BASE)
        a = once(req("PUT", BASE + "/locked/mine.txt",
                     {"If": "%s (<%s>)" % (tag, token)}, body="x"))
        check(a is not None and a[0] in (201, 204),
              "the token holder may add one (got %s)"
              % (a[0] if a else "nothing"))

        once(req("DELETE", BASE + "/locked/mine.txt",
                 {"If": "%s (<%s>)" % (tag, token)}))
        once(req("UNLOCK", BASE + "/locked", {"Lock-Token": "<%s>" % token}))

    # And with the lock gone, anybody may again.
    a = once(req("PUT", BASE + "/locked/after.txt", body="x"))
    check(a is not None and a[0] in (201, 204),
          "with the lock released it is open again")

    once(req("DELETE", BASE + "/locked"))


# ================================================================= WebSocket ==
#
# The terminal, at the level of bytes.  A library client would hide exactly the
# things that have to be checked here -- the accept value, the mask, where a
# fragment ends -- so the frames are built and read by hand.

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

# RFC 6455 1.3's own pair.  Written out rather than computed, so a server that
# agreed with this file's arithmetic and not with the specification would still
# be caught.
RFC_KEY    = "dGhlIHNhbXBsZSBub25jZQ=="
RFC_ACCEPT = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="


def ws_accept_of(key):
    return base64.b64encode(
        hashlib.sha1((key + WS_GUID).encode("ascii")).digest()).decode("ascii")


def ws_frame(opcode, payload, fin=True, mask=b"\x37\xfa\x21\x3d", masked=True):
    """One frame, exactly as a client puts it on the wire."""
    if isinstance(payload, str):
        payload = payload.encode("latin-1")

    b0 = (0x80 if fin else 0x00) | opcode
    n = len(payload)

    if n < 126:
        head = struct.pack("!BB", b0, (0x80 if masked else 0) | n)
    elif n < 65536:
        head = struct.pack("!BBH", b0, (0x80 if masked else 0) | 126, n)
    else:
        head = struct.pack("!BBQ", b0, (0x80 if masked else 0) | 127, n)

    if not masked:
        return head + payload

    body = bytes(payload[i] ^ mask[i % 4] for i in range(n))
    return head + mask + body


class WsConn(Conn):
    """A connection that has been upgraded, and the frame reader for it."""

    def __init__(self, key=RFC_KEY, timeout=None, path=TERM, headers=None,
                 first=b""):
        Conn.__init__(self, timeout=(timeout or WS_WAIT))
        lines = ["GET %s HTTP/1.1" % path,
                 "Host: %s:%d" % (ADDR, PORT)]
        h = {"Upgrade": "websocket",
             "Connection": "Upgrade",
             "Sec-WebSocket-Key": key,
             "Sec-WebSocket-Version": "13"}
        if headers is not None:
            h.update(headers)
        for k, v in h.items():
            if v is not None:
                lines.append("%s: %s" % (k, v))
        self.send("\r\n".join(lines).encode("latin-1") + b"\r\n\r\n" + first)

        self.status = None
        self.headers = {}
        while b"\r\n\r\n" not in self.buf:
            if not self.fill():
                return
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        lines = head.decode("latin-1").split("\r\n")
        self.status = int(lines[0].split()[1])
        for line in lines[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                self.headers[k.strip().lower()] = v.strip()

    def frame(self, deadline=None):
        """The next frame: (fin, opcode, payload).  None on hang-up or on the
        deadline passing, and the caller says which of the two it wanted."""
        if deadline is None:
            deadline = time.time() + WS_WAIT

        def need(n):
            while len(self.buf) < n:
                if time.time() > deadline:
                    return False
                self.s.settimeout(max(0.2, deadline - time.time()))
                try:
                    more = self.s.recv(4096)
                except socket.timeout:
                    continue
                except OSError:
                    return False
                if not more:
                    return False
                self.buf += more
            return True

        if not need(2):
            return None

        b0, b1 = self.buf[0], self.buf[1]
        n = b1 & 0x7f
        at = 2
        if n == 126:
            if not need(4):
                return None
            n = struct.unpack("!H", self.buf[2:4])[0]
            at = 4
        elif n == 127:
            if not need(10):
                return None
            n = struct.unpack("!Q", self.buf[2:10])[0]
            at = 10

        # A server frame is never masked, and that is itself an assertion.
        masked = (b1 & 0x80) != 0
        if masked:
            at += 4

        if not need(at + n):
            return None

        payload = self.buf[at:at + n]
        self.buf = self.buf[at + n:]

        return ((b0 & 0x80) != 0, b0 & 0x0f, payload, masked)

    def gather(self, seconds, want=None):
        """Everything the server says for `seconds`, or until `want` appears in
        what it has said.  Data frames only; control frames are returned
        separately so a test can assert on both."""
        text = b""
        control = []
        deadline = time.time() + seconds
        while time.time() < deadline:
            f = self.frame(deadline=deadline)
            if f is None:
                break
            fin, op, payload, masked = f
            if op in (0x1, 0x2, 0x0):
                text += payload
                if want is not None and want.encode("latin-1") in text:
                    break
            else:
                control.append((op, payload))
                if op == 0x8:
                    break
        return text, control


def ws_wait_free(seconds=20.0):
    """Wait for the terminal to come back, and say how long it took.

    Slept-for rather than measured is how a release that takes twenty seconds
    passes as one that takes two: the number is the point, so it is returned
    and printed rather than hidden in a sleep."""
    began = time.time()
    while time.time() - began < seconds:
        c = WsConn()
        status = c.status
        c.close()
        if status == 101:
            # Give it straight back; this was a probe.
            time.sleep(0.3)
            return time.time() - began
        time.sleep(0.5)
    return None


def test_ws_handshake():
    """RFC 6455 4.2.2, against the specification's own key and accept."""
    print("the upgrade handshake")

    c = WsConn(key=RFC_KEY)
    check(c.status == 101, "the upgrade is 101, not %s" % c.status)
    check(c.headers.get("upgrade", "").lower() == "websocket",
          "and says Upgrade: websocket (got %r)" % c.headers.get("upgrade"))
    check(c.headers.get("connection", "").lower() == "upgrade",
          "and Connection: Upgrade (got %r)" % c.headers.get("connection"))
    check(c.headers.get("sec-websocket-accept") == RFC_ACCEPT,
          "and the accept RFC 6455 1.3 prints, %s (got %r)"
          % (RFC_ACCEPT, c.headers.get("sec-websocket-accept")))
    # A 101 must not claim a body length: there is no body, there is a
    # protocol.
    check("content-length" not in c.headers,
          "a 101 carries no Content-Length (got %r)"
          % c.headers.get("content-length"))

    # And a second key, so the accept cannot be a constant.
    c.close()
    ws_wait_free()

    other = base64.b64encode(b"0123456789abcdef").decode("ascii")
    c = WsConn(key=other)
    check(c.status == 101, "a second key upgrades too")
    check(c.headers.get("sec-websocket-accept") == ws_accept_of(other),
          "and its accept is that key's, not the last one's")
    c.close()
    ws_wait_free()


def test_ws_refusals():
    """A request that is not an upgrade must be refused cleanly, and nothing
    may be spawned on the strength of one."""
    print("upgrades this server will not complete")

    cases = [
        ({"Sec-WebSocket-Key": None}, (400,), "no key at all"),
        ({"Sec-WebSocket-Key": "short"}, (400,), "a key that is not 16 bytes"),
        ({"Sec-WebSocket-Key": "AAAAAAAAAAAAAAAAAAAAAAAA"}, (400,),
         "24 characters that decode to 18 bytes"),
        ({"Upgrade": None}, (200,), "no Upgrade header: that is the page"),
        ({"Connection": "keep-alive"}, (400,),
         "Upgrade without the Connection token"),
        ({"Sec-WebSocket-Version": "8"}, (426,), "an older version"),
    ]

    for headers, want, why in cases:
        c = WsConn(headers=headers)
        check(c.status in want,
              "%s is %s, not %s" % (why, "/".join(str(w) for w in want),
                                    c.status))
        if c.status == 426:
            check(c.headers.get("sec-websocket-version") == "13",
                  "and 426 names the version there is")
        c.close()
        time.sleep(0.5)

    # Whatever those did, the terminal must still be free: a refused upgrade
    # that had already started a Shell would show up here as a 503.
    c = WsConn()
    check(c.status == 101,
          "the terminal is still free after the refusals (got %s)" % c.status)
    c.close()
    ws_wait_free()


def term_page(headers=None):
    """GET /shell, and nothing about what came back taken on trust: the
    length is checked against the bytes, because a Content-Length that
    disagrees with the body is the failure that looks like a hang."""
    a = once(req("GET", TERM, headers=headers))
    if a is None:
        return None
    said = a[1].get("content-length")
    check(said is not None and int(said) == len(a[2]),
          "Content-Length %s is the %d bytes that arrived" % (said, len(a[2])))
    return a


def test_ws_page():
    """The same address, without an upgrade, is the page."""
    print("the terminal's page")

    a = term_page()
    check(a is not None and a[0] == 200, "GET /shell is 200")
    if a is not None:
        check("text/html" in a[1].get("content-type", ""),
              "and is HTML (got %r)" % a[1].get("content-type"))
        check(b"WebSocket" in a[2],
              "and is the terminal's page rather than something else")
        # A client that said nothing about encodings gets bytes it can read.
        # This is the half of the feature that has to keep working, and the
        # half a compressed sibling could quietly break.
        check("content-encoding" not in a[1],
              "a request that did not ask for gzip gets no Content-Encoding "
              "(got %r)" % a[1].get("content-encoding"))
        check(a[1].get("cache-control") == "no-cache",
              "and Cache-Control is still no-cache (got %r)"
              % a[1].get("cache-control"))

    a = once(req("PUT", TERM, body="x"))
    check(a is not None and a[0] == 405,
          "PUT /shell is 405, not a write into the drawer (got %s)"
          % (a[0] if a else "nothing"))


def test_term_gzip():
    """The page, compressed at build time and served as it lies.

    The assertion that matters is not that a header was set: it is that what
    a browser receives, once it has been through the browser's own inflater,
    is the SAME PAGE.  So the plain one is fetched too and the two are
    compared byte for byte.  A server that gzipped the wrong file, truncated
    it, or sent it with a length off by one would pass every header check and
    fail this."""
    print("the terminal's page, gzipped")

    plain = term_page()
    if plain is None or plain[0] != 200:
        check(False, "cannot fetch the plain page to compare against")
        return

    a = term_page({"Accept-Encoding": "gzip"})
    if a is None or a[0] != 200:
        check(False, "GET /shell with Accept-Encoding: gzip is 200 (got %s)"
              % (a[0] if a else "nothing"))
        return

    check(a[1].get("content-encoding") == "gzip",
          "Content-Encoding is gzip (got %r)" % a[1].get("content-encoding"))
    check("text/html" in a[1].get("content-type", ""),
          "and the type is still the page's own, not the container's (got %r)"
          % a[1].get("content-type"))
    check("accept-encoding" in a[1].get("vary", "").lower(),
          "and Vary names Accept-Encoding (got %r)" % a[1].get("vary"))
    check(a[1].get("cache-control") == "no-cache",
          "and Cache-Control is still no-cache (got %r)"
          % a[1].get("cache-control"))

    check(len(a[2]) < len(plain[2]),
          "the compressed page is smaller: %d against %d"
          % (len(a[2]), len(plain[2])))

    try:
        same = gzip.decompress(a[2])
    except Exception as e:                        # noqa: BLE001
        same = None
        check(False, "what came back is gzip at all (%s)" % e)

    if same is not None:
        check(same == plain[2],
              "and it inflates to the plain page exactly: %d bytes against "
              "%d, %s" % (len(same), len(plain[2]),
                          "same bytes" if same == plain[2] else "DIFFERENT"))

    # `q=0` is a refusal, not a preference, and a server that read it as one
    # more way of saying gzip would send a body the client will not inflate.
    a = term_page({"Accept-Encoding": "gzip;q=0"})
    check(a is not None and "content-encoding" not in a[1],
          "gzip;q=0 is a refusal and gets the plain page (got %r)"
          % (a[1].get("content-encoding") if a else "nothing"))

    # The shape a browser really sends.
    a = term_page({"Accept-Encoding": "deflate, gzip;q=1.0, *;q=0.5"})
    check(a is not None and a[1].get("content-encoding") == "gzip",
          "a real browser's list is read (got %r)"
          % (a[1].get("content-encoding") if a else "nothing"))

    # And one that offers something else entirely.
    a = term_page({"Accept-Encoding": "br, deflate"})
    check(a is not None and "content-encoding" not in a[1],
          "a client offering only codings we do not have gets the page plain "
          "(got %r)" % (a[1].get("content-encoding") if a else "nothing"))


def test_term_etag():
    """A reload asks, and is told nothing has changed.

    This is what keeps `Cache-Control: no-cache` from costing what it used to.
    no-cache means "ask me every time", not "do not keep it", so the browser
    still comes back -- and 304 with no body is the same freshness guarantee
    for a few hundred bytes."""
    print("the terminal's page, revalidated")

    tags = {}

    for label, headers in (("plain", None), ("gzip", {"Accept-Encoding": "gzip"})):
        a = term_page(headers)
        if a is None or a[0] != 200:
            check(False, "cannot fetch the %s page" % label)
            continue

        etag = a[1].get("etag")
        check(etag is not None and etag.startswith('"'),
              "the %s page carries an ETag (got %r)" % (label, etag))
        if etag is None:
            continue
        tags[label] = etag

        h = dict(headers or {})
        h["If-None-Match"] = etag
        b = once(req("GET", TERM, headers=h))

        check(b is not None and b[0] == 304,
              "asking again with it gets 304 for the %s page (got %s)"
              % (label, b[0] if b else "nothing"))
        if b is None:
            continue
        check(b[2] == b"",
              "and no body at all (got %d bytes)" % len(b[2]))
        check("content-length" not in b[1],
              "and no Content-Length either (got %r)"
              % b[1].get("content-length"))
        check(b[1].get("etag") == etag,
              "and the 304 names the version (got %r)" % b[1].get("etag"))
        check(b[1].get("cache-control") == "no-cache",
              "and no-cache survives the 304 (got %r)"
              % b[1].get("cache-control"))

        # A validator that is not the one there must not be believed.
        h["If-None-Match"] = '"0-0-0-0"'
        b = once(req("GET", TERM, headers=h))
        check(b is not None and b[0] == 200 and len(b[2]) > 0,
              "a stale validator gets the %s page and not a 304 (got %s)"
              % (label, b[0] if b else "nothing"))

    # The two forms are different bytes.  A shared validator would let a
    # browser holding the plain page be told the gzipped one is what it has.
    if "plain" in tags and "gzip" in tags:
        check(tags["plain"] != tags["gzip"],
              "the two forms do not share a validator (both %r)"
              % tags["plain"])


def test_ws_shell():
    """A command, typed, and its output.  This is the whole feature."""
    print("a command through the Shell")

    c = WsConn()
    if c.status != 101:
        check(False, "cannot upgrade to run a command (got %s)" % c.status)
        c.close()
        return

    # The prompt.  A Shell that thought its input was a file would print none,
    # so this is the ACTION_IS_FILESYSTEM answer being checked from outside.
    banner, _ = c.gather(WS_WAIT, want=">")
    check(b">" in banner,
          "the Shell prints a prompt (got %r)" % banner[-80:])

    c.send(ws_frame(0x2, "Echo AMIGADOSLIVES\n"))
    said, _ = c.gather(WS_WAIT, want="AMIGADOSLIVES")
    check(b"AMIGADOSLIVES" in said,
          "Echo's output comes back (got %r)" % said[-120:])

    # Two frames, one message: RFC 6455 5.4 fragmentation, and the Shell must
    # see one command and not two.
    c.send(ws_frame(0x2, "Echo FRAG", fin=False))
    time.sleep(0.5)
    c.send(ws_frame(0x0, "MENTED\n", fin=True))
    said, _ = c.gather(WS_WAIT, want="FRAGMENTED")
    check(b"FRAGMENTED" in said,
          "a fragmented command is one command (got %r)" % said[-120:])

    # A ping, answered with a pong carrying the same payload.
    c.send(ws_frame(0x9, "areyouthere"))
    got = None
    deadline = time.time() + WS_WAIT
    while time.time() < deadline:
        f = c.frame(deadline=deadline)
        if f is None:
            break
        if f[1] == 0xa:
            got = f[2]
            break
    check(got == b"areyouthere",
          "a ping is answered with its own payload (got %r)" % got)

    # The close handshake: the server echoes the code and then lets go.
    c.send(ws_frame(0x8, struct.pack("!H", 1000) + b"bye"))
    code = None
    deadline = time.time() + WS_WAIT
    while time.time() < deadline:
        f = c.frame(deadline=deadline)
        if f is None:
            break
        if f[1] == 0x8:
            code = struct.unpack("!H", f[2][:2])[0] if len(f[2]) >= 2 else 0
            break
    check(code == 1000, "a close is answered with its own code (got %r)" % code)
    check(c.closed(), "and the server lets go of the socket")
    c.close()
    check(ws_wait_free() is not None,
          "and the Shell is free again after a clean close")

    # More than the terminal's 512-byte decoder hold, in the same send as the
    # upgrade.  The HTTP request buffer can hand almost 2 KB across here; none
    # of it may disappear merely because normal WebSocket reads are smaller.
    commands = b"".join(
        ("Echo PIPE%03d\n" % i).encode("ascii") for i in range(48))
    c = WsConn(first=ws_frame(0x2, commands))
    check(c.status == 101, "an upgrade with pipelined input succeeds")
    said, _ = c.gather(WS_WAIT, want="PIPE047")
    check(b"PIPE047" in said,
          "pipelined input beyond 512 bytes reaches the Shell (got %r)"
          % said[-120:])
    c.close()
    check(ws_wait_free() is not None,
          "and the pipelined session gives the Shell back")


def test_ws_one_session():
    """One Shell at a time, and the second asker is told so rather than
    getting a second Shell or a hung socket."""
    print("one session at a time")

    first = WsConn()
    check(first.status == 101, "the first upgrade succeeds")
    first.gather(WS_WAIT, want=">")

    second = WsConn()
    check(second.status == 503,
          "the second is 503, not %s" % second.status)
    second.close()

    first.close()

    # And with the first gone, the terminal is free again.  This is the check
    # that a browser closing its tab does not cost the machine its shell -- and
    # the SECOND time it was run it was the check that found it did.
    took = ws_wait_free()
    check(took is not None,
          "closing the first frees it again, within %.0fs" % WS_WAIT)
    if took is not None:
        print("  (the Shell came back in %.1fs)" % took)


def test_ws_unmasked():
    """RFC 6455 5.1.  A client frame that is not masked ends the connection,
    with 1002, and the Shell must be given back."""
    print("an unmasked frame from a client")

    c = WsConn()
    check(c.status == 101, "upgraded")
    c.gather(WS_WAIT, want=">")

    c.send(ws_frame(0x2, "Echo NEVER\n", masked=False))

    code = None
    deadline = time.time() + WS_WAIT
    while time.time() < deadline:
        f = c.frame(deadline=deadline)
        if f is None:
            break
        if f[1] == 0x8:
            code = struct.unpack("!H", f[2][:2])[0] if len(f[2]) >= 2 else 0
            break
    check(code == 1002,
          "an unmasked frame is closed 1002 (got %r)" % code)
    c.close()

    check(ws_wait_free() is not None,
          "and the Shell is given back afterwards")


def test_term_no_gz():
    """A -T page with no compressed copy beside it is served, uncompressed.

    MADE, not found.  httpd looks for the sibling when a browser asks and
    remembers no answer between requests, so "nobody ever compressed this
    page" and "the compressed one is not there any more" are one state, and
    moving it out of the way is how this reaches that state without a second
    server.  WebDAV does the moving, which is why the harness puts the -T page
    inside the document root: a MOVE is instant and needs nothing on the guest
    that the drill has not already used.

    Put back afterwards, and the putting back is CHECKED -- a restore that
    silently failed would leave a later run measuring the wrong thing and
    calling it a pass."""
    if not GZ_URL:
        print("the terminal's page with no .gz beside it: SKIPPED, "
              "no --gz-url=<served path> was given")
        return

    print("the terminal's page with no .gz beside it")

    away = GZ_URL + "away"
    dest = {"Destination": "http://%s:%d%s" % (ADDR, PORT, away)}

    a = once(req("MOVE", GZ_URL, dest))
    moved = a is not None and a[0] in (201, 204)
    check(moved, "the compressed sibling moves out of the way (got %s)"
                 % (a[0] if a else "nothing"))
    if not moved:
        return

    try:
        a = term_page({"Accept-Encoding": "gzip"})
        check(a is not None and a[0] == 200,
              "the page is still served with no .gz beside it (got %s)"
              % (a[0] if a else "nothing"))
        if a is not None:
            check("content-encoding" not in a[1],
                  "and comes back uncompressed (got %r)"
                  % a[1].get("content-encoding"))
            check(b"WebSocket" in a[2],
                  "and is the page itself, %d bytes" % len(a[2]))
    finally:
        back = {"Destination": "http://%s:%d%s" % (ADDR, PORT, GZ_URL)}
        b = once(req("MOVE", away, back))
        check(b is not None and b[0] in (201, 204),
              "and it is put back (got %s)" % (b[0] if b else "nothing"))

    a = term_page({"Accept-Encoding": "gzip"})
    check(a is not None and a[1].get("content-encoding") == "gzip",
          "with it back, gzip is served again -- nothing was remembered "
          "(got %r)" % (a[1].get("content-encoding") if a else "nothing"))


def main():
    print("httpd-drill against http://%s:%d/\n" % (ADDR, PORT))

    if not WS_ONLY:
        setup()
    try:
        if WS_ONLY:
            test_ws_shell()
            test_ws_page()
            test_term_gzip()
            test_term_etag()
            test_ws_handshake()
            test_ws_refusals()
            test_ws_one_session()
            test_ws_unmasked()
            test_term_no_gz()
            print("\n%d checks, %d failure(s)" % (checks, len(failures)))
            for f in failures:
                print("  %s" % f)
            return 0 if not failures else 1

        test_desync()
        test_refusal_keeps_the_connection()
        test_unframed_refusal_closes()
        test_etags()
        test_if_header()
        test_delete_destroys_locks()
        test_lock_below_stops_delete()
        test_proppatch_atomic()
        test_overlapping_moves()
        test_destination_contains_source()
        test_name_truncation()
        test_listing_omission_yields()
        test_propfind_body()
        test_depth0_collection_lock()

        if WANT_TERMINAL:
            # The Shell FIRST.  It is the feature; everything else here is
            # about the door in front of it.  Run in any other order, a
            # terminal that never came back from the first session reported
            # itself as six failures about 503 and said nothing at all about
            # whether a command works, which is what happened.
            test_ws_shell()
            test_ws_page()
            test_term_gzip()
            test_term_etag()
            test_ws_handshake()
            test_ws_refusals()
            test_ws_one_session()
            test_ws_unmasked()
            # Last, because it is the only one that changes the guest's disk.
            # A run that dies before it puts the sibling back leaves a state
            # every later check here would read as a defect, so nothing later
            # reads it.
            test_term_no_gz()
    finally:
        if not WS_ONLY:
            teardown()

    print("\n%d checks, %d failure(s)" % (checks, len(failures)))
    for f in failures:
        print("  %s" % f)

    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
