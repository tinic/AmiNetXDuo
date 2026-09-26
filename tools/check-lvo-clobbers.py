#!/usr/bin/env python3
#
# Every inline-asm library call names all four scratch registers.
#
#   tools/check-lvo-clobbers.py [--root DIR] [FILE...]
#
# The AmigaOS library ABI lets any LVO destroy d0, d1, a0 and a1.  An extended
# asm statement that calls one (`jsr a6@(-N)`) has to list each of the four as
# an output or a clobber; one listed only as an input is a register GCC
# believes still holds its value after the jsr.  That was #68: bsd_recvfrom
# left a1 off, the next inlined call passed a stale `from`, and it answered
# EFAULT.  The rest of the class is #70.
#
# Declarations are read through object-like macros (BSD_SCRATCH,
# NETDEV_REG_A1) from the file and the headers it includes by "name".
# Top-level asm has no operand lists and is not an inline call; it is skipped.
# An operand-less asm inside a function body that makes the call is flagged
# with all four missing: GCC assumes basic asm clobbers nothing.
#
# Output: one `lvo_clobbers=fail` line per site, then a summary line.
# Exit 0 when nothing is flagged, 1 when something is, 2 on a usage error.
#
# SPDX-License-Identifier: MIT

import os
import re
import sys

SCRATCH = ("a0", "a1", "d0", "d1")

# jsr a6@(-30), jsr -30(a6), jsr %c1(a6), jsr (-30,a6), with any %% escaping.
LVO_RE = re.compile(r'\bjsr\s+(?:%*a6@'
                    r'|-?%*\w+\(\s*%*a6\s*\)'
                    r'|\(\s*-?%*\w+\s*,\s*%*a6\s*\))')
ASM_RE = re.compile(r'\b(?:__asm__|__asm|asm)\s*(?:(?:__volatile__|__volatile|volatile)\s*)?\(')
DECL_RE = re.compile(r'\bregister\b[^;{}()]*?\b([A-Za-z_]\w*)\s*__asm(?:__)?\s*\(\s*"%*([ad][0-7])"\s*\)')
DEFINE_RE = re.compile(r'^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)(?![\w(])[ \t]*(.*)$', re.M)
INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"', re.M)
OPERAND_RE = re.compile(r'"([^"]*)"\s*\(\s*([A-Za-z_]\w*)\s*\)')
REG_RE = re.compile(r'"%*([ad][0-7])"')


def strip_comments(text):
    """Blank comments; keep strings and every newline."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
        elif c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def balanced(text, i):
    """text[i] is '('; return the index of its matching ')'."""
    depth = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            i = j
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def brace_depth(text, end):
    """Brace depth at text[end], ignoring strings and character constants."""
    depth, i = 0, 0
    while i < end:
        c = text[i]
        if c in '"\'':
            j = i + 1
            while j < end and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            i = j
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
        i += 1
    return depth


def split_colons(body):
    """Split an asm body at its top-level colons."""
    parts, depth, start, i, n = [], 0, 0, 0, len(body)
    while i < n:
        c = body[i]
        if c in '"\'':
            j = i + 1
            while j < n and body[j] != c:
                j += 2 if body[j] == '\\' else 1
            i = j
        elif c in '([':
            depth += 1
        elif c in ')]':
            depth -= 1
        elif c == ':' and depth == 0:
            parts.append(body[start:i])
            start = i + 1
        i += 1
    parts.append(body[start:])
    return parts


class Scanner:
    def __init__(self, root):
        self.root = root
        self.cache = {}
        self.joined = {}
        self.by_base = {}
        for d in ("include", "src", "tests"):
            for dp, _, fs in os.walk(os.path.join(root, d)):
                for f in fs:
                    if f.endswith(".h"):
                        self.by_base.setdefault(f, []).append(os.path.join(dp, f))

    def text(self, path):
        if path not in self.cache:
            with open(path, encoding="latin-1") as fh:
                raw = strip_comments(fh.read())
            # A continuation keeps its line for scanning, and is joined for
            # reading a #define.
            self.cache[path] = raw.replace("\\\n", " \n")
            self.joined[path] = raw.replace("\\\n", " ")
        return self.cache[path]

    def resolve(self, name, here):
        for cand in (os.path.join(os.path.dirname(here), name),
                     os.path.join(self.root, "include", name),
                     os.path.join(self.root, "src", name)):
            if os.path.isfile(cand):
                return os.path.normpath(cand)
        hits = self.by_base.get(os.path.basename(name), [])
        return hits[0] if len(hits) == 1 else None

    def macros(self, path, seen=None):
        seen = set() if seen is None else seen
        if path in seen:
            return {}
        seen.add(path)
        self.text(path)
        text = self.joined[path]
        out = {}
        for inc in INCLUDE_RE.findall(text):
            p = self.resolve(inc, path)
            if p:
                out.update(self.macros(p, seen))
        for m in DEFINE_RE.finditer(text):
            out[m.group(1)] = m.group(2).strip()
        return out

    @staticmethod
    def expand(s, macros):
        if not macros:
            return s
        pat = re.compile(r'\b(' + '|'.join(map(re.escape, macros)) + r')\b')
        for _ in range(6):
            t = pat.sub(lambda m: macros[m.group(1)], s)
            if t == s:
                break
            s = t
        return s

    def scan(self, path):
        text = self.text(path)
        macros = None
        rel = os.path.relpath(path, self.root)
        stmts, flagged = 0, []
        for m in ASM_RE.finditer(text):
            open_at = m.end() - 1
            close_at = balanced(text, open_at)
            if close_at < 0:
                continue
            body = text[open_at + 1:close_at]
            if macros is None:
                macros = self.macros(path)
            parts = split_colons(self.expand(body, macros))
            if not LVO_RE.search(parts[0]):
                continue
            if len(parts) < 2:
                if brace_depth(text, m.start()) > 0:
                    stmts += 1
                    line = text.count("\n", 0, m.start()) + 1
                    flagged.append((rel, line, list(SCRATCH)))
                continue
            stmts += 1
            # Declarations visible here: from the last column-0 brace.
            start = text.rfind("\n{", 0, m.start())
            start = 0 if start < 0 else start
            regs = {}
            for d in DECL_RE.finditer(self.expand(text[start:m.start()], macros)):
                regs[d.group(1)] = d.group(2)
            covered = set()
            for c, var in OPERAND_RE.findall(parts[1]):
                if var in regs and ("=" in c or "+" in c):
                    covered.add(regs[var])
            if len(parts) > 3:
                covered.update(REG_RE.findall(parts[3]))
            missing = [r for r in SCRATCH if r not in covered]
            if missing:
                line = text.count("\n", 0, m.start()) + 1
                flagged.append((rel, line, missing))
        return stmts, flagged


def main(argv):
    root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    files = []
    args = list(argv)
    while args:
        a = args.pop(0)
        if a == "--root" and args:
            root = os.path.abspath(args.pop(0))
        elif a.startswith("-"):
            print("usage: check-lvo-clobbers.py [--root DIR] [FILE...]", file=sys.stderr)
            return 2
        else:
            files.append(os.path.abspath(a))
    if not files:
        for d in ("include", "src", "tests"):
            for dp, dns, fs in os.walk(os.path.join(root, d)):
                dns[:] = sorted(x for x in dns if x != "third_party")
                files += [os.path.join(dp, f) for f in fs if f.endswith((".c", ".h"))]

    sc = Scanner(root)
    total, flagged, nfiles = 0, [], 0
    for f in sorted(files):
        s, fl = sc.scan(f)
        total += s
        nfiles += 1 if s else 0
        flagged += fl
    for rel, line, missing in flagged:
        print("lvo_clobbers=fail file=%s line=%d missing=%s" % (rel, line, ",".join(missing)))
    if total == 0:
        print("lvo_clobbers=fail reason=no_statements_found")
        return 1
    state = "fail" if flagged else "ok"
    print("lvo_clobbers=%s statements=%d files=%d flagged=%d" % (state, total, nfiles, len(flagged)))
    return 1 if flagged else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
