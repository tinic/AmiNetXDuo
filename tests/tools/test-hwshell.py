#!/usr/bin/env python3
"""tests/tools/hwshell.py against a stand-in for httpd's /shell, on the host.

    python3 tests/tools/test-hwshell.py [path/to/hwshell.py]

The stand-in keeps to the wire src/tools/httpterm.c serves: binary frames are
keystrokes into a cooked line buffer, a text frame is a control word, and only
the word `break` is Ctrl-C.  Behind it is one Shell that outlives its socket,
as httpd's does, and that reads a line the way dos.library's ReadItem() does:
a control character is part of the command name, so "\\x03Prompt" is an
unknown command.

Runs against the same Shell:

  1. a command that never returns, then another in the SAME run.  hwshell
     must break it with the `break` word, leave no byte behind in the
     Shell's input, report it stopped only on the prompt, and run the next
     command cleanly with its own prompt.
  2. a failing command and `Echo "rc=$RC"`.  The Prompt has to take, which a
     stray 0x03 from run 1 prevents, and each command's RC has to come back.
  3. a timeout, then a command in a NEW connection, which must run cleanly.
  4. a command that ignores the break.  hwshell must say shell_break=
     no_prompt and shell_command=unknown, exit 5, and type nothing more into
     a Shell it cannot prove is idle.

Output is key=value; the exit status is the number of failed checks.

SPDX-License-Identifier: MIT
"""

import os
import re
import socket
import struct
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))


class Shell:
    """The AmigaDOS side: the parts of shell.c and ReadItem() hwshell meets."""

    def __init__(self):
        self.pending = b""          # typed, not yet a whole line
        self.prompt = b"%N.SYS:> "
        self.rc = 0
        self.vars = {}
        self.busy = False           # a command that only Ctrl-C ends
        self.deaf = False           # ...and one that ignores Ctrl-C too
        self.keys = b""             # every keystroke byte, for the checks
        self.words = []             # every control word, for the checks

    def show_prompt(self):
        text = self.prompt.replace(b"%R", str(self.rc).encode())
        text = text.replace(b"%N", b"1")
        return b"\x0f" + text

    def line(self, raw):
        """One command line; what the Shell prints for it."""
        text = raw.decode("latin-1")
        text = re.sub(r"\$(\w+)",
                      lambda m: self.vars.get(m.group(1).upper(),
                                              str(self.rc) if
                                              m.group(1).upper() == "RC"
                                              else ""),
                      text)
        name, _, rest = text.strip(" \t").partition(" ")
        if name == "":
            return b""
        if name.lower() == "prompt":
            arg = rest.strip()
            if arg.startswith('"') and arg.endswith('"'):
                arg = arg[1:-1]
            self.prompt = arg.replace("*N", "\n").encode("latin-1")
            self.rc = 0
            return b""
        if name.lower() == "echo":
            arg = rest.strip()
            if arg.startswith('"') and arg.endswith('"'):
                arg = arg[1:-1]
            self.rc = 0
            return arg.encode("latin-1") + b"\n"
        if name == "Hang":
            self.busy = True
            return b""
        if name == "Deaf":
            self.busy = True
            self.deaf = True
            return b""
        if name == "Fail":
            self.rc = 5
            return b"Fail: no\n"
        self.rc = 10
        return ("%s: Unknown command\n" % name).encode("latin-1")

    def keystrokes(self, data):
        self.keys += data
        self.pending += data
        out = b""
        while not self.busy and b"\n" in self.pending:
            raw, self.pending = self.pending.split(b"\n", 1)
            out += self.line(raw)
            if not self.busy:
                out += self.show_prompt()
        return out

    def word(self, w):
        self.words.append(w)
        if w == "break" and self.busy and not self.deaf:
            self.busy = False
            self.rc = 20
            return b"***Break\n" + self.show_prompt() + \
                self.keystrokes(b"")
        return b""


def frame(op, payload):
    n = len(payload)
    if n < 126:
        return struct.pack("!BB", 0x80 | op, n) + payload
    return struct.pack("!BBH", 0x80 | op, 126, n) + payload


def serve(lsock, shell, stop):
    while not stop.is_set():
        try:
            c, _ = lsock.accept()
        except OSError:
            return
        c.settimeout(0.2)
        buf = b""
        try:
            while b"\r\n\r\n" not in buf:
                buf += c.recv(4096)
            buf = buf.split(b"\r\n\r\n", 1)[1]
            c.sendall(b"HTTP/1.1 101 Switching Protocols\r\n"
                      b"Upgrade: websocket\r\nConnection: Upgrade\r\n"
                      b"Sec-WebSocket-Accept: x\r\n\r\n")
            c.sendall(frame(2, b"New Shell process 1\n" + shell.show_prompt()))
            while not stop.is_set():
                while len(buf) >= 6:
                    b0, b1 = buf[0], buf[1]
                    n, at = b1 & 0x7f, 2
                    if n == 126:
                        n, at = struct.unpack("!H", buf[2:4])[0], 4
                    if len(buf) < at + 4 + n:
                        break
                    mask = buf[at:at + 4]
                    data = bytes(buf[at + 4 + i] ^ mask[i % 4]
                                 for i in range(n))
                    buf = buf[at + 4 + n:]
                    op = b0 & 0x0f
                    if op == 2:
                        out = shell.keystrokes(data)
                    elif op == 1:
                        out = shell.word(data.decode("latin-1"))
                    elif op == 8:
                        raise ConnectionError
                    else:
                        out = b""
                    if out:
                        c.sendall(frame(2, out))
                try:
                    more = c.recv(4096)
                except socket.timeout:
                    continue
                if not more:
                    break
                buf += more
        except (OSError, ConnectionError):
            pass
        finally:
            c.close()


def main():
    hwshell = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(HERE, "hwshell.py")

    shell = Shell()
    stop = threading.Event()
    lsock = socket.socket()
    lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    lsock.bind(("127.0.0.1", 0))
    lsock.listen(4)
    port = lsock.getsockname()[1]
    t = threading.Thread(target=serve, args=(lsock, shell, stop), daemon=True)
    t.start()

    failed = 0

    def check(ok, what):
        nonlocal failed
        print("%s=%s" % (what, "ok" if ok else "FAIL"))
        if not ok:
            failed += 1

    def run(*cmds):
        args = [sys.executable, hwshell, "127.0.0.1", str(port), "-t", "2"]
        for cmd in cmds:
            args += ["-c", cmd]
        p = subprocess.run(args, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=60)
        return p.returncode, p.stdout.decode("latin-1")

    rc, out = run("Hang", 'Echo "after"')
    check(rc == 4, "hang_exit_4")
    check("break" in shell.words, "hang_sent_break_word")
    check(b"\x03" not in shell.keys, "hang_sent_no_ctrl_c_byte")
    check(not shell.busy and shell.pending == b"", "hang_broken_input_clean")
    check("shell_state=timeout\nshell_break=prompt\nshell_command=stopped\n"
          "shell_rc=20\n" in out, "hang_stopped_only_on_prompt")
    check('----- Echo "after"\nafter\nshell_rc=0\n' in out,
          "same_run_next_command_clean")

    rc, out = run("Fail", 'Echo "rc=$RC"')
    check(rc == 0 and "shell_state=ready" in out, "next_run_prompt_takes")
    check("shell_rc=5" in out, "failing_command_rc_reported")
    check("rc=5\n" in out, "rc_carries_to_next_command")
    # The reused Shell must come out of a clean run holding the standard
    # prompt, not the run's one-off HWSH-<uuid> token.
    check(shell.prompt == b"%N.%S> ",
          "clean_run_restores_standard_prompt")

    rc, out = run("Hang")
    check(rc == 4, "lone_hang_exit_4")
    rc, out = run('Echo "fresh"')
    check(rc == 0 and '----- Echo "fresh"\nfresh\nshell_rc=0\n' in out,
          "new_connection_after_timeout_clean")

    before = len(shell.keys)
    rc, out = run("Deaf", 'Echo "never"')
    check(rc == 5, "deaf_exit_5")
    check("shell_break=no_prompt\nshell_command=unknown\n" in out,
          "deaf_reports_no_prompt_unknown")
    check("shell_command=stopped" not in out, "deaf_not_reported_stopped")
    check(b"never" not in shell.keys[before:], "deaf_nothing_more_sent")

    stop.set()
    lsock.close()
    if failed:
        print("--- last run\n" + out, end="")
    print("hwshell_checks_failed=%d" % failed)
    return failed


if __name__ == "__main__":
    sys.exit(main())
