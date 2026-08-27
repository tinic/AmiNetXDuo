#!/usr/bin/env python3
"""Run AmigaDOS commands on a LIVE machine, through httpd's /shell.

    tests/tools/hwshell.py ADDRESS PORT [-t SECONDS] < commands

WHAT IT IS FOR

  tools/amiberry-run.sh drives a guest by building a drive and booting it.
  Nothing here can do that to a machine that is already switched on and
  standing on somebody's desk, and the real A1200 in the lab is the only
  3c589 this project has.  Its control surface is the one it already serves:
  WebDAV for the files and /shell for the Shell.

  The frame reader and the connection are tests/tools/httpd-drill.py's,
  imported rather than copied, for the reason wsterm-console.py gives -- they
  are the part that has to be right at the level of bytes.

HOW A COMMAND IS KNOWN TO HAVE FINISHED

  By the prompt, and by a prompt that cannot be confused with anything a
  command prints: `Prompt` is set to a token generated for this run.  A Shell
  banner, a command that prints nothing and a command that prints the word
  "prompt" are all then unambiguous.  Nothing here counts lines or sleeps.

OUTPUT

  One `----- <command>` line per command, then what it printed.  The exit
  status is 0 when every command was reached, 3 when the machine did not
  answer at all -- which is not a failed run, it is no run.

SPDX-License-Identifier: MIT
"""

import argparse
import importlib.util
import os
import socket
import sys
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))


def load_drill(addr, port):
    """httpd-drill.py reads its target out of sys.argv at import time."""
    saved = sys.argv
    sys.argv = [os.path.join(HERE, "httpd-drill.py"), addr, str(port),
                "--ws-only"]
    try:
        spec = importlib.util.spec_from_file_location(
            "httpd_drill", os.path.join(HERE, "httpd-drill.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
    finally:
        sys.argv = saved
    return mod


def main():
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("address")
    ap.add_argument("port", nargs="?", type=int, default=80)
    ap.add_argument("-t", "--timeout", type=float, default=60.0,
                    help="seconds allowed for any one command")
    ap.add_argument("-c", "--command", action="append", default=[],
                    help="a command to run; repeatable, else read stdin")
    ap.add_argument("--take", action="store_true",
                    help="take the terminal off whoever holds it")
    ap.add_argument("-h", "--help", action="help")
    a = ap.parse_args()

    commands = list(a.command)
    if not commands:
        commands = [ln.strip() for ln in sys.stdin
                    if ln.strip() and not ln.startswith("#")]
    if not commands:
        print("hwshell: no commands", file=sys.stderr)
        return 2

    d = load_drill(a.address, a.port)
    os.environ.setdefault("AMINETXDUO_WS_WAIT", str(a.timeout))
    d.WS_WAIT = a.timeout

    try:
        ws = d.WsConn(timeout=a.timeout)
    except (OSError, socket.error) as e:
        print("shell_state=unreachable")
        print("hwshell: %s:%d did not accept a connection: %s"
              % (a.address, a.port, e), file=sys.stderr)
        return 3

    if ws.status == 503 and a.take:
        # ?take=1 is httpd own reclaim, src/tools/httpd.c:3619.  The first
        # request only lets the holder go -- it answers 503 as well, saying
        # so -- and the second is the one that gets the terminal.
        d.WsConn(timeout=a.timeout, path=d.TERM + "?take=1")
        ws = d.WsConn(timeout=a.timeout)

    if ws.status != 101:
        if ws.status == 503:
            # 503 IS NOT "NO TERMINAL".  httpd serves one Shell at a time and
            # answers 503 for as long as somebody holds it, which is exactly
            # what the timeout branch below leaves behind.  Reporting that as
            # "httpd serves /shell only with -T" sent the next reader looking
            # for a switch that was there all along, and cost a run on
            # amiga-1200 doing it.  httpd says which of the two it is in the
            # body; that is what goes out here.
            print("shell_state=busy")
            print("hwshell: %s:%d answered 503 to the /shell upgrade: %s  "
                  "Pass --take to take it off a session that has stopped "
                  "reading."
                  % (a.address, a.port,
                     ws.buf.decode("latin-1", "replace").strip()),
                  file=sys.stderr)
        else:
            print("shell_state=no_terminal")
            print("hwshell: %s:%d answered %s to the /shell upgrade.  httpd "
                  "serves /shell only with -T."
                  % (a.address, a.port, ws.status), file=sys.stderr)
        return 3

    # KEYSTROKES ARE BINARY FRAMES.  src/tools/httpterm.c:1721 puts opcode 2
    # into the Shell's input and reads opcode 1 as a CONTROL WORD -- `size`
    # and the rest -- so a text frame full of AmigaDOS is not a command that
    # failed, it is a word the terminal did not know.  Sent as text, every
    # line here was silently dropped and the machine answered pings.
    def send(text):
        ws.send(d.ws_frame(2, text.encode("latin-1")))

    def until(token, seconds):
        """Everything the Shell said up to `token`, or None when it never
        came.  Opcode 1 is the server's own control channel and is not the
        Shell talking, so it is kept out of what a command is said to have
        printed."""
        got = ""
        deadline = time.time() + seconds
        while time.time() < deadline:
            f = ws.frame(deadline=deadline)
            if f is None:
                break
            fin, op, payload, masked = f
            if op == 2:
                got += payload.decode("latin-1", "replace")
                if token in got:
                    return got[:got.index(token)]
            elif op == 9:
                ws.send(d.ws_frame(10, payload))
            elif op == 8:
                break
        return None

    token = "HWSH-%s" % uuid.uuid4().hex[:12]

    # The banner, and whatever prompt the machine was already set to.  Two
    # seconds is not a guess about the machine's speed: the sync below is
    # what waits, and this only keeps the banner out of the first command's
    # output.
    until("\x00-never-\x00", 2.0)

    # The Shell's line terminator here is newline, which is what
    # tests/tools/wsterm-console.py types.  A carriage return is taken as a
    # character and the line is never entered: every command below then
    # times out against a machine that is working.
    send('Prompt "%s*N"\n' % token)
    if until(token, a.timeout) is None:
        print("shell_state=no_prompt")
        print("hwshell: the Shell on %s:%d never echoed the run's prompt.  "
              "Either `Prompt` is not in its C: or nothing is reading the "
              "socket." % (a.address, a.port), file=sys.stderr)
        return 3

    print("shell_state=ready")
    print("shell_prompt=%s" % token)

    rc = 0
    for cmd in commands:
        print("----- %s" % cmd)
        sys.stdout.flush()
        send(cmd + "\n")
        out = until(token, a.timeout)
        if out is None:
            # BREAK IT BEFORE LETTING GO.  A Shell still running a command
            # holds the one terminal session httpd serves, and every later
            # connection is answered 503 -- so a run that timed out took the
            # machine with it and the next thing to ask a question got
            # "httpd serves /shell only with -T" about a machine serving it.
            # Ctrl-C is what a person would send.
            send("\x03")
            until(token, 10.0)
            print("shell_state=timeout")
            print("hwshell: %r did not return to a prompt in %.0f s"
                  % (cmd, a.timeout), file=sys.stderr)
            rc = 4
            break
        # The Shell echoes nothing; the page does.  What comes back is the
        # command's own output and no more.
        sys.stdout.write(out.replace("\r\n", "\n").replace("\r", "\n"))
        if not out.endswith("\n"):
            sys.stdout.write("\n")

    try:
        ws.close()
    except OSError:
        pass

    print("shell_commands=%d" % len(commands))
    return rc


if __name__ == "__main__":
    sys.exit(main())
