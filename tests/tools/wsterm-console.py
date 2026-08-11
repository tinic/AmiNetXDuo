#!/usr/bin/env python3
"""The console assertions, at the level of bytes.

    tests/tools/wsterm-console.py ADDRESS [PORT]

WHAT THIS IS FOR, AS AGAINST httpd-drill.py --terminal

  httpd-drill.py asks whether a Shell answers through a WebSocket.  This asks
  whether the thing answering is a CONSOLE: whether ACTION_SCREEN_MODE reaches
  it, whether RAW mode is announced to the page before a password prompt can
  be answered, whether a keystroke is delivered on its own rather than held
  until a Return, and whether a program that asks how big the window is gets
  told.

  The frame reader and the connection are httpd-drill.py's, imported rather
  than copied: they are the part that has to be right at the level of bytes,
  and two of them would be two things to keep right.

WHAT IT DELIBERATELY DOES NOT CLAIM

  It does not echo.  The echo is the page's, so a drill asserting "the
  password did not appear on my screen" would be asserting something about
  itself.  What it asserts is the SERVER's half of the contract -- `mode raw`
  arrives, and nothing of what was typed comes back -- and the page's half is
  tests/tools/wsconsole-page.mjs, which drives the built page in a browser.

SPDX-License-Identifier: MIT
"""

import importlib.util
import os
import re
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

_spec = importlib.util.spec_from_file_location(
    "httpd_drill", os.path.join(HERE, "httpd-drill.py"))
d = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(d)

check = d.check
WsConn = d.WsConn
ws_frame = d.ws_frame
WS_WAIT = d.WS_WAIT

WANT_ED = os.environ.get("AMINETXDUO_WSCONSOLE_ED") == "yes"
WANT_MORE = os.environ.get("AMINETXDUO_WSCONSOLE_MORE") == "yes"
WANT_SSH = os.environ.get("AMINETXDUO_WSCONSOLE_SSH") == "yes"
WANT_VIM = os.environ.get("AMINETXDUO_WSCONSOLE_VIM") == "yes"
SSHD_PORT = os.environ.get("AMINETXDUO_WSCONSOLE_SSHD_PORT", "2224")

# Where the guest reaches the build host.  Behind NAT that is always 10.0.2.2;
# bridged it is this machine's own address on the LAN, and the runner passes
# it.  Not guessable from in here, so it is given rather than assumed.
HOST_FROM_GUEST = os.environ.get("AMINETXDUO_WSCONSOLE_HOST", "10.0.2.2")

# A string that exists nowhere else in this run.  If it comes back from the
# server, something echoed it.
SECRET = "Zx9Qv-notthepassword-Kw3"

# The user the guest logs in as.  The runner resolves it, because $USER here
# is whoever is driving the page and that is not always whoever owns the
# account on the server.
USER = os.environ.get("AMINETXDUO_WSCONSOLE_SSHD_USER",
                      os.environ.get("USER", "nobody"))

# An identity is staged at DH0:sshkey, so the interactive arm can log in.
HAVE_KEY = os.environ.get("AMINETXDUO_WSCONSOLE_SSHKEY") == "yes"

# A string that exists nowhere else, echoed by the REMOTE shell.  It is in two
# halves in the source so that the command line, which comes back from the
# far end as its own echo, is not mistaken for the output of running it.
MARK = "AMIGA-" + "REMOTE-7QF2"


class Session:
    """One upgraded socket, with the two channels kept apart.

    gather() in httpd-drill.py folds text and binary frames into one buffer,
    which is right for a suite that only ever saw one of them.  Here they are
    the two halves of the protocol and the whole question is which arrived
    when, so they are kept separate and time-stamped.
    """

    def __init__(self, path=None, retry=True):
        """Upgraded, RETRYING a 503 the way the page does.

        Ending a session is not instant -- the Shell has to notice its end of
        file -- so the ask that comes straight after one is answered "not yet"
        and the ask a second later is answered 101.  The page retries once for
        exactly this reason; a drill that did not would be asserting that the
        server is quicker than it promises to be rather than that it works."""
        deadline = time.time() + (25.0 if retry else 0.0)
        while True:
            self.c = WsConn(path=(path or d.TERM))
            if self.c.status == 101 or time.time() >= deadline:
                break
            self.c.close()
            time.sleep(1.0)

        self.out = b""          # everything the Shell has printed
        self.words = []         # (seconds, word) from the server
        self.frames = 0         # binary frames seen, which is the perf number
        self.closed = False

    @property
    def status(self):
        return self.c.status

    def pump(self, seconds, want=None, quiet=None):
        """Read for `seconds`, or until `want` appears, or until `quiet`
        seconds pass with nothing arriving.

        `quiet` is how you time something whose END you cannot name.  A
        redraw has no sentinel -- you cannot wait for a string, because what
        it paints is whatever was on the screen -- so the way to know it
        finished is that the far side stopped talking.  Without it a timing
        arm reports the length of its own deadline, which is what the first
        version of this did: every redraw took exactly 30.00s."""
        deadline = time.time() + seconds
        start = len(self.out)
        last = time.time()
        while time.time() < deadline:
            if quiet is not None and time.time() - last > quiet:
                break
            stop = deadline
            if quiet is not None:
                stop = min(stop, last + quiet)
            f = self.c.frame(deadline=stop)
            if f is None:
                if quiet is not None:
                    continue
                break
            fin, op, payload, masked = f
            last = time.time()
            if op in (0x2, 0x0):
                self.frames += 1
                self.out += payload
            elif op == 0x1:
                self.words.append((time.time(), payload.decode("latin-1")))
            elif op == 0x8:
                self.closed = True
                break
            if want is not None and want.encode("latin-1") in self.out[start:]:
                break
        return self.out[start:]

    def keys(self, text):
        self.c.send(ws_frame(0x2, text))

    def word(self, w):
        self.c.send(ws_frame(0x1, w))

    def said(self, w):
        return any(x == w for _, x in self.words)

    def last_word(self):
        return self.words[-1][1] if self.words else None

    def close(self):
        try:
            self.c.send(ws_frame(0x8, struct.pack("!H", 1000)))
        except OSError:
            pass
        self.c.close()


def free():
    """Wait for the Shell to come back between arms."""
    return d.ws_wait_free()


# --------------------------------------------------------------- the arms --

def test_mode_is_announced():
    """A session opens COOKED and says so, before anybody types.

    Without this a page has to assume, and the assumption is the one that
    draws a password on the screen."""
    print("the mode, announced")

    s = Session()
    if s.status != 101:
        check(False, "cannot upgrade (got %s)" % s.status)
        return

    s.pump(WS_WAIT, want=">")
    check(b">" in s.out, "the Shell still prints a prompt (got %r)"
          % s.out[-80:])
    check(s.said("mode cooked"),
          "the server says `mode cooked` when the session opens (heard %r)"
          % [w for _, w in s.words])

    # And the ordinary case is untouched: a command, and its output.
    s.keys("Echo CONSOLELIVES\n")
    got = s.pump(WS_WAIT, want="CONSOLELIVES")
    check(b"CONSOLELIVES" in got,
          "a command still runs and answers (got %r)" % got[-120:])

    # An unknown word is ignored, not an error and not a close: that is the
    # whole of the compatibility rule, and it is cheaper to assert than to
    # rediscover.
    s.word("wumpus 3")
    s.keys("Echo AFTERWUMPUS\n")
    got = s.pump(WS_WAIT, want="AFTERWUMPUS")
    check(b"AFTERWUMPUS" in got,
          "an unrecognised word is ignored, not fatal (got %r)" % got[-120:])

    s.close()
    check(free() is not None, "and the Shell is free again")


def test_size_is_taken():
    """`size` is accepted and does not disturb the Shell."""
    print("the window size, sent")

    s = Session()
    if s.status != 101:
        check(False, "cannot upgrade (got %s)" % s.status)
        return
    s.pump(WS_WAIT, want=">")

    s.word("size 100 40")
    s.keys("Echo AFTERSIZE\n")
    got = s.pump(WS_WAIT, want="AFTERSIZE")
    check(b"AFTERSIZE" in got,
          "a size word does not disturb the Shell (got %r)" % got[-120:])

    s.close()
    check(free() is not None, "and the Shell is free again")


def test_ssh_password():
    """The prompt-and-read phase: raw mode arrives before a secret can be typed.

    ssh's getpass() calls SetMode(Input(), 1).  Two things must follow: the
    server must tell the page it is now in raw mode BEFORE the password can be
    typed, and nothing of what is typed may come back.

    This is NOT the interactive session; see test_ssh_interactive() below,
    which is.  A password prompt that does not echo is a program still in its
    own prompt-and-read loop, and it passed for as long as the session after
    it hung."""
    print("ssh, and the password")

    if not WANT_SSH:
        print("  SKIPPED: no ssh client staged, or no sshd to point it at")
        return

    s = Session()
    if s.status != 101:
        check(False, "cannot upgrade (got %s)" % s.status)
        return
    s.pump(WS_WAIT, want=">")

    # Execute() gives the Shell the system default stack, the same as any
    # other Shell on the machine; an ssh client needs more, and `stack` is how
    # a person would ask for it too.
    s.keys("stack 65536\n")
    s.pump(5.0)

    s.keys("ssh -y -p %s %s@%s\n" % (SSHD_PORT, USER, HOST_FROM_GUEST))

    # The prompt.  Generous, because this is a key exchange on a 68020.
    got = s.pump(120.0, want="assword")
    reached = b"assword" in got

    # A server with PasswordAuthentication off never offers one, and
    # clients/dropbear/sshd-testserver.sh is such a server on purpose.  That is
    # a server this arm cannot ask its question of, not a failure of the
    # console, and it is said rather than scored.
    if not reached and b"No auth methods" in s.out[-400:]:
        print("  SKIPPED: the server offers no password authentication")
        s.close()
        free()
        return

    check(reached, "ssh reaches a password prompt (last 200 bytes: %r)"
          % s.out[-200:])
    if not reached:
        s.close()
        free()
        return

    # getpass() writes the prompt and THEN calls SetMode(), so the word can be
    # a frame behind the prompt.  A moment, not a race: what matters is that
    # it arrives before a person could have finished reading the prompt.
    s.pump(5.0)

    raw_at = [t for t, w in s.words if w == "mode raw"]
    check(len(raw_at) > 0,
          "the server says `mode raw` for the prompt (heard %r)"
          % [w for _, w in s.words])
    if not raw_at:
        s.close()
        free()
        return
    check(s.last_word() == "mode raw",
          "and raw is the mode in force when the prompt is on the screen "
          "(last word was %r)" % s.last_word())

    # Typed the way a person types: one keystroke, one frame.  A handler still
    # holding reads until a Return would answer none of these until the last.
    before = len(s.out)
    for ch in SECRET:
        s.keys(ch)
        time.sleep(0.02)
    s.keys("\n")

    s.pump(30.0)
    echoed = s.out[before:]
    check(SECRET.encode("latin-1") not in s.out,
          "the password is never sent back (found it in %r)"
          % echoed[:200])

    # And the mode goes back, or every prompt after this one would be silent.
    s.pump(20.0)
    check(s.said("mode cooked") and
          any(t for t, w in s.words if w == "mode cooked" and t > raw_at[0]),
          "the server says `mode cooked` again afterwards (heard %r)"
          % [w for _, w in s.words])

    s.close()
    check(free() is not None, "and the Shell is free again after ssh")


def test_ssh_interactive():
    """THE ACCEPTANCE TEST: a shell on the far end, driven from the page.

    Log in with a key, get the REMOTE shell's prompt, run a command there, read
    its output, leave, and be back at the Amiga's own prompt with the console
    cooked again.  Every one of those is a different direction through the
    console handler and the shim, and the ones after the prompt are the ones
    nothing tested: `ssh -t` hung the moment the far end spoke, because
    Dropbear writes zero bytes when its channel buffer is empty and the handler
    parked the packet.

    Each step has its own bounded wait and its own message.  A step that does
    not happen says which one it was and what the last bytes on the screen
    were; it does not sit until the runner's ceiling, which would be an
    infrastructure verdict for a defect in the thing under test."""
    print("ssh, and an interactive session")

    if not WANT_SSH:
        print("  SKIPPED: no ssh client staged, or no sshd to point it at")
        return
    if not HAVE_KEY:
        print("  SKIPPED: no identity staged (AMINETXDUO_WSCONSOLE_SSHKEY)")
        return

    s = Session()
    if s.status != 101:
        check(False, "cannot upgrade (got %s)" % s.status)
        return
    s.pump(WS_WAIT, want=">")

    s.keys("stack 65536\n")
    s.pump(5.0)

    # -t forces the pty even though the console already earns one, so the arm
    # asserts the pty path and not whatever IsInteractive() happens to answer.
    # -y -y accepts the host key and does not write it down; there is no home
    # directory here to write it to.
    s.keys("ssh -t -y -y -i DH0:sshkey -p %s %s@%s\n"
           % (SSHD_PORT, USER, HOST_FROM_GUEST))

    # The far end's prompt.  Generous: a key exchange and an ed25519 signature
    # on a 14 MHz 68020 is most of this.  '$' is the sh/bash prompt and '%' is
    # csh/zsh; a login shell of some other kind is a server this cannot drive
    # and it says so rather than guessing.
    deadline = time.time() + 150.0
    while time.time() < deadline:
        s.pump(5.0)
        if b"$ " in s.out[-400:] or b"% " in s.out[-400:] or b"# " in s.out[-400:]:
            break
    got_prompt = (b"$ " in s.out[-400:] or b"% " in s.out[-400:]
                  or b"# " in s.out[-400:])
    check(got_prompt,
          "the remote shell prints a prompt (last 200 bytes: %r)"
          % s.out[-200:])
    check(s.said("mode raw"),
          "and the page was told the console went raw for it (heard %r)"
          % [w for _, w in s.words])
    if not got_prompt:
        # Leave nothing running on the far end, then let the runner say the
        # arm failed rather than time out.
        s.keys("\003")
        s.pump(5.0)
        s.close()
        free()
        return

    # A command on the FAR end, and its output back.  Typed one keystroke at a
    # time, because that is how a person types into a raw console and because
    # a handler holding reads until a Return would deliver none of it.
    before = len(s.out)
    for ch in "echo " + MARK:
        s.keys(ch)
        time.sleep(0.02)
    s.keys("\n")

    # Twice: once as the far end's echo of what was typed, once as the output
    # of running it.  One occurrence is an echo and no shell, so the wait is
    # for the SECOND -- `want` would stop at the first, which is the keystrokes
    # coming back and proves only that the line reached the far end.
    deadline = time.time() + 45.0
    while time.time() < deadline:
        s.pump(3.0)
        if s.out[before:].count(MARK.encode("latin-1")) >= 2:
            break
    seen = s.out[before:].count(MARK.encode("latin-1"))
    check(seen >= 2,
          "the command runs on the far end and its output comes back "
          "(saw %d occurrence(s) of the marker in %r)"
          % (seen, s.out[before:][-200:]))

    # And out again, cleanly: the remote shell exits, ssh exits, the console
    # goes back to cooked, and the Amiga's own Shell prompts.
    s.keys("exit\n")
    got = s.pump(45.0, want="DH0:")
    check(b"DH0:" in got,
          "the Amiga Shell prompts again after the session (got %r)"
          % got[-200:])
    check(s.last_word() == "mode cooked",
          "and the console is cooked again (last word was %r)"
          % s.last_word())

    s.close()
    check(free() is not None, "and the Shell is free again after ssh")


def test_ed():
    """Ed: raw mode, a keystroke on its own, and a window it had to ask for.

    Ed cannot start without an answer to its window bounds request -- it sits
    in `while (rdch() != 0x9B)` until one arrives -- so Ed drawing anything at
    all is the assertion that the request was answered.  A keystroke with no
    Return after it reaching Ed is the assertion that RAW delivers one
    character rather than holding for a line."""
    print("Ed")

    if not WANT_ED:
        print("  SKIPPED: no Ed staged (AMINETXDUO_WBC)")
        return

    s = Session()
    if s.status != 101:
        check(False, "cannot upgrade (got %s)" % s.status)
        return
    s.pump(WS_WAIT, want=">")

    s.word("size 100 40")

    # WINDOW=* and not a bare `Ed`.  Ed's default window is a RAW: of its own
    # -- `RAW:0/0/639/199/Ed 2.00/CLOSE`, in ed.h -- so a bare Ed opens an
    # Intuition window on the Amiga's own screen and draws there, and the
    # browser sees nothing at all.  `*` is the console it was started from,
    # which is this one, and reaching it needs the FIND packet a console
    # answers.  Measured: without WINDOW=*, forty seconds and zero bytes.
    s.keys("Ed WINDOW=* DH0:Public/pagefile.txt\n")

    got = s.pump(90.0, want="quick brown fox")
    check(b"quick brown fox" in got,
          "Ed opens the file and draws it, which means the window bounds "
          "request was answered (got %r)" % got[-200:])
    if b"quick brown fox" not in got:
        s.close()
        free()
        return

    check(s.said("mode raw"), "Ed puts the console in raw mode (heard %r)"
          % [w for _, w in s.words])

    # And the report carried the size we SENT, not a default.  Ed positions
    # the cursor absolutely as it fills the window, so the largest row it
    # addresses is the window it thinks it has.  At 80x25 it would stop at 24.
    rows = [int(m) for m in re.findall(rb"\x1b\[([0-9]+);1H", s.out)]
    check(len(rows) > 0 and max(rows) > 25,
          "and lays out to the 40 rows the page reported, not a default "
          "(the lowest row it drew on was %s)" % (max(rows) if rows else None))

    # One keystroke, no Return.  In cooked mode the handler holds it.
    s.keys("Z")
    got = s.pump(20.0, want="Z")
    check(b"Z" in got,
          "one keystroke with no Return reaches Ed (got %r)" % got[-80:])

    # A cursor key, in the 8-bit form the page sends in raw mode.
    s.keys("\u009BB")
    s.pump(5.0)

    # ESC then `x`: Ed's command line, and the command that saves and exits.
    s.keys("\u001B")
    s.pump(5.0)
    s.keys("x\r")
    got = s.pump(30.0, want=">")
    check(b">" in got, "Ed saves and exits back to the Shell (got %r)"
          % got[-160:])

    check(s.said("mode cooked"),
          "and the console goes back to cooked (heard %r)"
          % [w for _, w in s.words])

    s.close()
    free()

    # The file is in the drawer httpd serves, so the edit can be read back
    # over the same port it was typed through.
    a = d.once(d.req("GET", "/pagefile.txt", keepalive=False))
    body = a[2] if a is not None else b""
    first = body.split(b"\n")[0]
    check(b"Z" in first,
          "and the edit is in the file on the disk (first line %r)"
          % first[:80])


def test_more():
    """More pages rather than pouring."""
    print("More")

    if not WANT_MORE:
        print("  SKIPPED: no More staged (AMINETXDUO_WBC)")
        return

    s = Session()
    if s.status != 101:
        check(False, "cannot upgrade (got %s)" % s.status)
        return
    s.pump(WS_WAIT, want=">")

    s.keys("More DH0:Public/pagefile.txt\n")
    got = s.pump(60.0, want="line 010")
    check(b"line 010" in got, "More prints the start of the file (got %r)"
          % got[-160:])

    check(s.said("mode raw"), "More puts the console in raw mode (heard %r)"
          % [w for _, w in s.words])

    # It must have STOPPED.  A More that poured the whole file out would have
    # reached the last line without being asked.
    s.pump(6.0)
    check(b"line 120" not in s.out,
          "and stops for a keypress rather than pouring (it reached the end)")

    # A space is the next page.
    before = len(s.out)
    s.keys(" ")
    got = s.pump(20.0)
    check(len(got) > 0,
          "a space with no Return after it turns the page (got %d bytes)"
          % len(got))

    s.keys("q")
    s.pump(20.0, want=">")
    s.close()
    free()


def test_dead_client_is_reclaimed():
    """A client that stops answering does not hold the Shell for ever.

    THE BUG THIS IS ABOUT

      A tab that goes away when the NETWORK does sends no close frame and no
      FIN.  Nothing downstream of a silent socket concludes anything, so the
      session stayed held and every later visitor was refused -- until the
      machine was restarted.  Measured on a live one.

    HOW IT IS PROVOKED HERE

      By doing nothing.  This drill never answers a ping, so a session it
      leaves idle is, from the server's side, exactly a browser that has
      vanished: the socket is open, the peer is quiet, and the ping goes
      unanswered.  No firewall rule and no killed process is needed."""
    print("a client that stops answering")

    first = Session()
    if first.status != 101:
        check(False, "cannot upgrade (got %s)" % first.status)
        return
    first.pump(WS_WAIT, want=">")

    # It really is held: a second asker is refused while the first is fresh.
    second = WsConn()
    check(second.status == 503,
          "a live session still holds the terminal (got %s)" % second.status)
    second.close()

    # Now go quiet.  The server pings after TIMEOUT and gives up TIMEOUT after
    # that, so the bound is two of them plus a pass.  Reported as what it
    # actually cost, because a bound nobody looks at is a bound that creeps.
    bound = float(os.environ.get("AMINETXDUO_WSCONSOLE_DEAD", "80"))
    began = time.time()
    took = None
    while time.time() - began < bound:
        time.sleep(2.0)
        c = WsConn()
        got = c.status
        c.close()
        if got == 101:
            took = time.time() - began
            break

    check(took is not None,
          "a quiet session is let go of within %.0fs, so the next visitor "
          "gets a Shell" % bound)
    if took is not None:
        print("  (the terminal came back in %.0fs)" % took)

    first.close()
    free()


def test_takeover():
    """?take=1 claims the terminal from a session that is still answering.

    The automatic path above needs the old client to have stopped answering.
    This is the other half: a person at a keyboard who knows the other tab is
    theirs and wants it back NOW."""
    print("taking the terminal over")

    first = Session()
    if first.status != 101:
        check(False, "cannot upgrade (got %s)" % first.status)
        return
    first.pump(WS_WAIT, want=">")

    plain = WsConn()
    check(plain.status == 503,
          "without asking, a live session is not taken (got %s)" % plain.status)
    plain.close()

    taken = WsConn(path=d.TERM + "?take=1")
    check(taken.status == 503,
          "the asking upgrade is answered 503 while the old one goes "
          "(got %s)" % taken.status)
    taken.close()

    # The old session was told why, rather than simply dropped.
    first.pump(10.0)
    check(first.closed,
          "and the session that had it is closed rather than left hanging")

    # And the retry the page makes gets a Shell.
    got = None
    began = time.time()
    while time.time() - began < 20.0:
        time.sleep(1.0)
        s = Session()
        if s.status == 101:
            got = s.pump(WS_WAIT, want=">")
            s.close()
            break
    check(got is not None and b">" in got,
          "the asker gets a working Shell on its retry (got %r)"
          % (got[-80:] if got else None))

    first.close()
    free()


def test_short_command_is_cheap():
    """THE WORKLOAD, AND THE THING NOT TO REGRESS.

    This terminal is for `Dir`, `lha x`, `Copy`, checking something -- short
    commands -- with WebDAV doing the file moving.  That path is good: 44 ms
    to upgrade, 33 ms to a prompt, 23 ms echo round trip, and `Echo HELLO`
    costs TWO WebSocket frames and fifteen bytes.

    Asserted in FRAMES as well as seconds, because frames is the number that
    catches the regression this is guarding against.  A change that sends a
    frame per byte instead of a frame per write would still be fast enough to
    pass a wall-clock bound on an idle machine, and would be ruinous on a
    busy one; sixty frames for `Echo HELLO` is wrong no matter how quickly
    they arrive."""
    print("a short command stays cheap")

    s = Session()
    if s.status != 101:
        check(False, "cannot upgrade (got %s)" % s.status)
        return
    s.pump(WS_WAIT, want=">")

    began = time.time()
    s.keys("Echo HELLO\n")
    got = s.pump(WS_WAIT, want="HELLO")
    took = time.time() - began

    check(b"HELLO" in got, "the command answers (got %r)" % got[-80:])
    check(s.frames <= 6,
          "and costs a handful of frames, not one per byte (took %d)"
          % s.frames)
    check(took < 5.0,
          "and comes back promptly (%.2fs)" % took)
    print("  (%d frames, %d bytes, %.2fs)" % (s.frames, len(got), took))

    s.close()
    check(free() is not None, "and the Shell is free again")


def test_vim():
    """vim: the most demanding thing this console runs.

    Not because anyone will edit over it -- the workload is short commands --
    but because a full-screen editor exercises raw mode, cursor addressing,
    the window size and a redraw all at once.  What is asserted is that a
    redraw COMPLETES and stays small; the wall clock is reported rather than
    asserted tightly, because the cost is vim thinking on a 68020 and that is
    a floor, not a defect."""
    print("vim")

    if not WANT_VIM:
        print("  SKIPPED: no vim staged (AMINETXDUO_WSCONSOLE_VIM)")
        return

    s = Session()
    if s.status != 101:
        check(False, "cannot upgrade (got %s)" % s.status)
        return
    s.pump(WS_WAIT, want=">")
    s.word("size 80 25")

    # -u NONE, which is not a way of avoiding a hard case.  The vim package
    # in the asset store has no runtime files, so vim stops on
    # "E1187: Failed to source defaults.vim -- Press ENTER" before it paints
    # anything, and an arm that waited for the file would be measuring a
    # missing tarball.  Skipping the startup scripts gets to the screen this
    # is about, and is what a person hitting that prompt would do next.
    s.keys("vim -u NONE DH0:Public/pagefile.txt\n")
    got = s.pump(120.0, want="quick brown fox")
    check(b"quick brown fox" in got,
          "vim opens the file and paints (got %r)" % got[-160:])
    if b"quick brown fox" not in got:
        s.keys(":q!\r")
        s.close()
        free()
        return

    check(s.said("mode raw"), "and puts the console in raw mode (heard %r)"
          % [w for _, w in s.words])

    # A redraw: jump to the end of the file, which repaints the window.
    before = s.frames
    began = time.time()
    s.keys("G")
    got = s.pump(30.0, quiet=1.5)
    took = time.time() - began
    frames = s.frames - before

    check(len(got) > 0, "a keystroke redraws (got %d bytes)" % len(got))
    check(took < 20.0, "and the redraw completes (%.2fs)" % took)
    check(len(got) < 8192,
          "and a redraw is under 8 KB, so nothing is repainting the world "
          "(was %d bytes in %d frames)" % (len(got), frames))
    print("  (redraw: %d frames, %d bytes, %.2fs)" % (frames, len(got), took))

    s.keys(":q!\r")
    s.pump(30.0, want=">")
    check(s.said("mode cooked"), "and vim gives the console back cooked")

    s.close()
    free()


def main():
    print("wsterm-console against ws://%s:%d/shell\n" % (d.ADDR, d.PORT))

    test_mode_is_announced()
    test_short_command_is_cheap()
    test_size_is_taken()
    test_takeover()
    test_ssh_password()
    test_ssh_interactive()
    test_ed()
    test_more()
    test_vim()
    # Last, because it is the slow one and because it deliberately leaves the
    # terminal quiet for over a minute.
    test_dead_client_is_reclaimed()

    print("\n%d checks, %d failure(s)" % (d.checks, len(d.failures)))
    for f in d.failures:
        print("  %s" % f)

    return 0 if not d.failures else 1


if __name__ == "__main__":
    sys.exit(main())
