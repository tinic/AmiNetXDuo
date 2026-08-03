#!/usr/bin/env python3
#
# httpd-drill -- the WebDAV assertions that need a socket rather than a client.
#
#   tests/tools/httpd-drill.py ADDRESS [PORT]
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
#   same connection.  The server is whatever is listening at ADDRESS -- the
#   emulated Amiga that tests/tools/run-httpd.sh puts on the wire.
#
# WHAT IT NEEDS OF THE SERVER
#
#   A writable document root.  Everything it makes it removes, under a drawer
#   of its own, so it can be run against a share somebody is using.
#
# SPDX-License-Identifier: MIT

import socket
import sys
import time

ADDR = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080

BASE = "/httpd-drill"

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
    fresh request -- so the file went."""
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
        # the drawer itself is not what is locked -- RFC 4918 10.4.1.
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
    the walk reads the destination back after the handler has returned -- so
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


def main():
    print("httpd-drill against http://%s:%d/\n" % (ADDR, PORT))

    setup()
    try:
        test_desync()
        test_refusal_keeps_the_connection()
        test_unframed_refusal_closes()
        test_etags()
        test_if_header()
        test_delete_destroys_locks()
        test_lock_below_stops_delete()
        test_proppatch_atomic()
        test_overlapping_moves()
        test_name_truncation()
    finally:
        teardown()

    print("\n%d checks, %d failure(s)" % (checks, len(failures)))
    for f in failures:
        print("  %s" % f)

    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
