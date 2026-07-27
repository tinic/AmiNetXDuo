#!/usr/bin/env python3
"""Static checks for a Commodore Installer script.

Not a compiler.  It tokenises the script the way lparse.c does -- ';' starts a
comment that runs to end of line, '"' and '\\'' quote strings which may not
span a line, '\\' escapes inside them -- and then checks the things that are
easy to get wrong and expensive to find out about on an Amiga:

  * unbalanced parentheses, and where the imbalance starts;
  * string literals over MAX_STRING (512) bytes, which the lexer rejects;
  * unterminated strings;
  * every head symbol of a statement being a name the Installer 2.17 symbol
    tables in compile.c actually define, or a procedure the script defines,
    or a string (the string-substitution form);
  * parameters used in statements that compile.c's param_syms mask does not
    allow them in;
  * format strings spelled as several adjacent literals.  ("a" "b" x) does
    NOT concatenate: the first literal is the format and everything after it
    is an argument, so "b" is formatted through the first %s and x is
    dropped.  Counting % specifiers against arguments catches it;
  * (choices) lists that will not fit on an Installer page.  This one is
    learned the hard way: layout_box_gads() lays a radio page out into
    whatever room is left, and when the choices do not fit it silently
    creates fewer gadgets than there are choices.  default_radio() then
    marks a gadget that does not exist, final_radio() finds nothing
    selected, and the installation dies on "askchoice: No choices
    selected" -- with no hint that the labels were the problem.

Usage: checkscript.py <script> [...]
Exit status 0 if every file passes.

SPDX-License-Identifier: MIT
"""

import sys

MAX_STRING = 512

# The Installer's window is 56 characters wide and sixteen text rows tall
# (window.c calc_window_size), of which a radio page gets what is left after
# the prompt and the buttons.  Two columns of four short labels fit; eight
# long ones do not.  These limits are empirical, from watching Installer 2.17
# fail on the real thing.
#
# NINE fits once the labels are short.  The card page carries nine at ten
# characters or less ("Ariadne II", "X-Surf 100") and lays out; it did not
# when the same page used names like "Commodore A2065" and "Not in this
# list".  Both numbers are about ROOM, and the count alone was never the
# whole story -- which is why the length limit below matters more than this
# one.  Raised only after install/test/run-installer-fsuae.sh -l AVERAGE
# drove the page under Installer 2.17 and completed; if a future page dies on
# "askchoice: No choices selected", it has too many or they are too wide, and
# the answer is to merge entries rather than to raise this again.
MAX_CHOICES = 9
MAX_CHOICE_LEN = 22

# ---------------------------------------------------------------- symbols --
#
# Transcribed from Installer 2.17 compile.c: statement_syms, control_syms,
# func_syms, param_syms, string_syms, help_syms.

STATEMENTS = {
    "makedir": "NEWDIR", "copyfiles": "FILES", "copylib": "LIBS",
    "startup": "STARTEDIT", "tooltype": "TOOLTYPE", "textfile": "TEXTFILE",
    "execute": "OTHER", "run": "OTHER", "rexx": "OTHER", "rename": "RENAME",
    "delete": "DELETE", "abort": "NONE", "exit": "OTHER",
    "complete": "NONE", "message": "OTHER", "working": "OTHER",
    "welcome": "NONE",
}

CONTROL = {"set", "if", "while", "until", "trap", "onerror", "foreach",
           "select", "procedure"}

FUNCS = {
    "cat": None, "tackon": None, "fileonly": None, "pathonly": None,
    "askfile": "ASKFILE", "askdir": "ASKDIR", "askstring": "ASKSTRING",
    "asknumber": "ASKNUM", "askchoice": "ASKCHOICE", "askoptions": "ASKCHOICE",
    "askbool": "ASKBOOL", "askdisk": "ASKDISK", "exists": "OTHER",
    "earlier": None, "getsize": None, "getdiskspace": None, "getsum": None,
    "getversion": "OTHER", "getassign": None, "makeassign": "OTHER",
    "getenv": None, "select": None,
    "=": None, ">": None, ">=": None, "<": None, "<=": None, "<>": None,
    "+": None, "-": None, "*": None, "/": None,
    "and": None, "or": None, "not": None, "xor": None, "in": None,
    "debug": None, "transcript": None, "protect": None, "user": None,
    "shiftleft": None, "shiftright": None, "substr": None, "expandpath": None,
    "database": None, "delopts": None, "strlen": None, "patmatch": None,
    "getdevice": None,
    "bitand": None, "bitor": None, "bitnot": None, "bitxor": None,
}

ASK = {"ASKFILE", "ASKDIR", "ASKSTRING", "ASKNUM", "ASKCHOICE", "ASKBOOL",
       "LIBS", "ASKDISK"}
ALL = ({"NEWDIR", "FILES", "STARTEDIT", "TOOLTYPE", "TEXTFILE", "OTHER",
        "RENAME", "DELETE"} | ASK)

# param name -> set of actions it is legal in (compile.c param_syms)
PARAMS = {
    "help": ALL,
    "prompt": ALL,
    "choices": {"FILES", "FUNC", "ASKCHOICE", "ASKBOOL"},
    "pattern": {"FILES"},
    "all": {"FILES"},
    "source": {"FILES", "RENAME", "LIBS"},
    "dest": {"FILES", "TOOLTYPE", "TEXTFILE", "RENAME", "DELETE", "NEWDIR",
             "LIBS", "ASKDISK"},
    "newname": {"FILES", "LIBS", "ASKDISK"},
    "confirm": {"NEWDIR", "FILES", "TEXTFILE", "TOOLTYPE", "RENAME", "DELETE",
                "LIBS", "OTHER"},
    "safe": {"FILES", "NEWDIR", "TEXTFILE", "TOOLTYPE", "RENAME", "DELETE",
             "LIBS", "OTHER"},
    "infos": {"NEWDIR", "FILES", "LIBS", "DELETE"},
    "settooltype": {"TOOLTYPE"},
    "setdefaulttool": {"TOOLTYPE"},
    "setstack": {"TOOLTYPE"},
    "noposition": {"TOOLTYPE", "FILES", "LIBS"},
    "default": {"ASKSTRING", "ASKCHOICE", "ASKNUM", "ASKDIR", "ASKFILE",
                "ASKBOOL"},
    "range": {"ASKNUM"},
    "command": {"STARTEDIT"},
    "append": {"TEXTFILE"},
    "files": {"FILES"},
    "include": {"TEXTFILE"},
    "optional": {"FILES", "LIBS", "DELETE", "ASKDIR", "TOOLTYPE"},
    "fonts": {"FILES"},
    "disk": {"RENAME", "ASKDIR"},
    "resident": {"LIBS", "OTHER"},
    "swapcolors": {"TOOLTYPE"},
    "newpath": {"ASKDIR"},
    "noreq": {"OTHER"},
    "assigns": {"ASKDISK"},
    "nogauge": {"FILES", "LIBS"},
    "quiet": {"OTHER"},
}

PREDEF = {
    "@icon", "@default-dest", "@pretend", "@user-level", "@error-msg",
    "@special-msg", "@each-type", "@each-name", "@ioerr", "@execute-dir",
    "false", "true", "@language", "@app-name", "@abort-button",
    "@askoptions-help", "@askchoice-help", "@asknumber-help",
    "@askstring-help", "@askdisk-help", "@copylib-help", "@copyfiles-help",
    "@askfile-help", "@askdir-help", "@makedir-help", "@startup-help",
}

# ------------------------------------------------------------- tokenising --

LP, RP, STR, INT, SYM = "( ) str int sym".split()


def tokenise(text, path, errors):
    toks = []
    line = 1
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif c in " \t\r":
            i += 1
        elif c == ";":
            while i < n and text[i] != "\n":
                i += 1
        elif c in "\"'":
            quote = c
            start = line
            i += 1
            buf = []
            closed = False
            while i < n:
                if text[i] == quote:
                    closed = True
                    i += 1
                    break
                if text[i] == "\\" and i + 1 < n:
                    i += 1
                    buf.append(text[i])
                    if text[i] == "\n":
                        line += 1
                elif text[i] == "\n":
                    errors.append(f"{path}:{start}: string runs past end of "
                                  f"line (the lexer forbids that)")
                    break
                else:
                    buf.append(text[i])
                i += 1
            if not closed:
                errors.append(f"{path}:{start}: unterminated string")
            s = "".join(buf)
            if len(s) >= MAX_STRING:
                errors.append(f"{path}:{start}: string literal is {len(s)} "
                              f"bytes, the limit is {MAX_STRING}")
            toks.append((STR, s, start))
        elif c == "(":
            toks.append((LP, "(", line))
            i += 1
        elif c == ")":
            toks.append((RP, ")", line))
            i += 1
        else:
            j = i
            while j < n and text[j] not in " \t\r\n();\"'":
                j += 1
            word = text[i:j]
            start = line
            i = j
            if word.lstrip("-").isdigit() or (
                    word[:1] in "$%" and len(word) > 1):
                toks.append((INT, word, start))
            else:
                toks.append((SYM, word, start))
    return toks


# --------------------------------------------------------------- checking --

def check(path):
    errors = []
    warnings = []
    text = open(path, "r", encoding="latin-1").read()
    toks = tokenise(text, path, errors)

    # parentheses
    depth = 0
    opens = []
    for kind, val, line in toks:
        if kind == LP:
            depth += 1
            opens.append(line)
        elif kind == RP:
            depth -= 1
            if depth < 0:
                errors.append(f"{path}:{line}: unmatched ')'")
                depth = 0
            elif opens:
                opens.pop()
    if depth:
        errors.append(f"{path}: {depth} unclosed '(' -- outermost opened at "
                      f"line {opens[0] if opens else '?'}")

    # walk the tree, tracking which statement each parameter sits in
    procs = set()
    defined = set()
    for idx in range(len(toks) - 2):
        if toks[idx][0] == LP and toks[idx + 1][1].lower() == "procedure":
            procs.add(toks[idx + 2][1].lower())
        if toks[idx][0] == LP and toks[idx + 1][1].lower() == "set":
            j = idx + 2
            while j + 1 < len(toks) and toks[j][0] == SYM:
                defined.add(toks[j][1].lower())
                j += 2

    stack = []          # action context of each enclosing '('
    i = 0
    while i < len(toks):
        kind, val, line = toks[i]
        if kind == LP:
            nxt = toks[i + 1] if i + 1 < len(toks) else (None, "", line)
            head = nxt[1].lower()
            action = None
            if nxt[0] == SYM:
                if head in STATEMENTS:
                    action = STATEMENTS[head]
                elif head in FUNCS:
                    action = FUNCS[head]
                elif head in CONTROL:
                    action = None
                elif head in PARAMS:
                    ctx = next((a for a in reversed(stack) if a), None)
                    if ctx is None:
                        warnings.append(
                            f"{path}:{line}: parameter ({head}) outside any "
                            f"statement")
                    elif ctx not in PARAMS[head]:
                        errors.append(
                            f"{path}:{line}: ({head}) is not legal in a "
                            f"statement whose action is {ctx}")
                elif head in procs:
                    pass
                elif head in PREDEF or head in defined:
                    pass
                else:
                    errors.append(f"{path}:{line}: unknown head symbol "
                                  f"'{nxt[1]}'")
            stack.append(action)
        elif kind == RP:
            if stack:
                stack.pop()
        i += 1

    # ("fmt" args...) with the wrong number of arguments
    i = 0
    while i < len(toks) - 1:
        if toks[i][0] == LP and toks[i + 1][0] == STR:
            fmt = toks[i + 1][1]
            line = toks[i][2]
            # count the arguments: everything up to the matching ')' at
            # depth 0, treating a nested group as one argument
            j = i + 2
            depth = 0
            nargs = 0
            while j < len(toks):
                kind = toks[j][0]
                if kind == LP:
                    if depth == 0:
                        nargs += 1
                    depth += 1
                elif kind == RP:
                    if depth == 0:
                        break
                    depth -= 1
                elif depth == 0:
                    nargs += 1
                j += 1

            specs = 0
            k = 0
            while k < len(fmt) - 1:
                if fmt[k] == "%":
                    if fmt[k + 1] == "%":
                        k += 1
                    else:
                        specs += 1
                k += 1

            if nargs and specs != nargs:
                errors.append(
                    f"{path}:{line}: format {fmt[:40]!r} has {specs} "
                    f"specifier(s) but {nargs} argument(s) -- adjacent "
                    f"string literals are not concatenated here, so build "
                    f"the format with (cat ...) and pass it as a variable")
        i += 1

    # (choices ...) that will not fit on a page
    i = 0
    while i < len(toks) - 1:
        if toks[i][0] == LP and toks[i + 1][1].lower() == "choices":
            j = i + 2
            labels = []
            while j < len(toks) and toks[j][0] == STR:
                labels.append(toks[j][1])
                j += 1
            line = toks[i][2]
            if len(labels) > MAX_CHOICES:
                errors.append(f"{path}:{line}: {len(labels)} choices; more "
                              f"than {MAX_CHOICES} will not fit on a page")
            for lab in labels:
                if len(lab) > MAX_CHOICE_LEN:
                    errors.append(
                        f"{path}:{line}: choice {lab!r} is {len(lab)} "
                        f"characters; over {MAX_CHOICE_LEN} and the page "
                        f"silently drops gadgets")
        i += 1

    # unknown bare symbols (typos in variable names)
    known = set(STATEMENTS) | CONTROL | set(FUNCS) | set(PARAMS) | PREDEF
    for idx, (kind, val, line) in enumerate(toks):
        if kind != SYM:
            continue
        low = val.lower()
        if low in known or low in procs or low in defined:
            continue
        if idx and toks[idx - 1][0] == LP:
            continue                      # already reported above
        errors.append(f"{path}:{line}: '{val}' is never set and is not a "
                      f"built-in name")

    for w in warnings:
        print("warning:", w)
    for e in errors:
        print("error:", e)
    if not errors:
        print(f"{path}: ok ({len(toks)} tokens)")
    return not errors


def main(argv):
    ok = True
    for path in argv[1:]:
        ok = check(path) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
