#!/usr/bin/env python3
"""Verify the pinned newlib allocator in every installed toolchain multilib.

The 16.2.2 packages shipped a bootstrap libc despite pinning fixed newlib
source. For each immutable asset series, compare installed malloc.o bytes
against the known-good newlib build, in both libc.a and libg.a. A future
source, compiler, or flag change must deliberately update this map and its
runtime Enforcer proof; unknown bytes fail closed.

Every series is ALSO proven structurally, from the disassembly of
MemMap::alloc: both fresh allocations, the big node (malloc.cpp:578) and the
small page (malloc.cpp:593), must store zero to Node::prev. --object FILE
checks one malloc.o that way and prints which basis each path used.

THE TWO BASES. Node is {next, prev, leaf, size}. AllocMem returns the raw
block; __sys_alloc stores the size in the raw block's first longword and
returns raw+4, the Node. So prev is 8(raw) = 4(node), and 4(raw) is NEXT.
  inline   -O2 (16.2.1-16.2.3) inlines AllocMem into alloc in the non-baserel
           multilibs and never calls __sys_alloc there. Site: `jsr -198(a6)` then `movea.l d0,aX`; aX is
           raw, the size store to 0(raw) is required, and prev is 8(aX).
  call     -Os (16.2.4), and -O2 baserel (libb, libb32), call __sys_alloc out
           of line. Its fall-through path must return AllocMem's result + 4
           (sys_alloc_returns_node traces d0); the caller's `movea.l d0,aX`
           is then the node and prev is 4(aX).
The store must follow the site before any call or return, with aX not
rewritten and no branch jumping over it except the AllocMem-failed exit.
Zero means `clr.l`, or a stack slot proven to hold find()'s null (see
_spilled_null). Exactly two sites; anything else is unproven and fails.
"""

import hashlib
import pathlib
import re
import subprocess
import sys
import tempfile

# Series proven structurally as well as by hash: all of them.
STRUCTURAL_SERIES = ("16.2.3", "16.2.4")


# Paths are relative to <prefix>/m68k-amigaos/lib. The paired libc/libg
# archive members must match for each variant. One map per asset series; every
# archive in a prefix must match the same series.
EXPECTED_MALLOC_SHA256 = {
    # newlib at -O2 -fomit-frame-pointer. Eight distinct hashes, eleven multilibs.
    "16.2.3": {
        "": "0f2522d271d7094be4c9dd974e24634d13b6af150fd48fc8de89caf1ab4f2c22",
        "libm020": "1531c3f1489f4d1a19b53d69e3fcb3a17abbce21c49216151c323dbdb7e00144",
        "libm020/libm881": "1531c3f1489f4d1a19b53d69e3fcb3a17abbce21c49216151c323dbdb7e00144",
        "libm060": "a60a7c7e936c87f661d7b23430d7f908d5b902c1131dbf5eae792a54a94c4e91",
        "libb": "0085a784fa1bbe184f322df4ae1b48413a0c421ca1daf570d491c4fcc8798316",
        "libb/libm020": "af379f9fbe7da3bab6a5476c094147415e55ea7e9c491d4d94676f17b145e1b1",
        "libb/libm020/libm881": "af379f9fbe7da3bab6a5476c094147415e55ea7e9c491d4d94676f17b145e1b1",
        "libb/libm060": "c486aa7719ae6bedd46d82a37609951019df6d29ad135558c10a5c2285347a3b",
        "libb32/libm020": "07c5767e7ec8e7dd31c3649cab559b9cb5a498743cb3976ff9b5adda352352a2",
        "libb32/libm020/libm881": "07c5767e7ec8e7dd31c3649cab559b9cb5a498743cb3976ff9b5adda352352a2",
        "libb32/libm060": "f7f63dad7cb7ff955678a50a2f778ce46a8988ee2c9ed683e17cf6dd98b5f188",
    },
    # newlib at -Os -fomit-frame-pointer. Eight distinct hashes, eleven multilibs.
    # The fresh-page prev store (malloc.cpp:593) is `move.l 24(sp),4(a0)`:
    # find()'s null result, spilled before __sys_alloc, not a clr.l.
    "16.2.4": {
        "": "8ea1e8bb23083c50a8f391e56264b731e1f9787df4a45628445ea59ee605dbea",
        "libm020": "e7b1be5336bac70a25008e1f14bee229b6788f06934b1667f336a007dce5c21b",
        "libm020/libm881": "e7b1be5336bac70a25008e1f14bee229b6788f06934b1667f336a007dce5c21b",
        "libm060": "966703699ca1463a8c0abfdd772f8d86af788098949da0565541d0cc8b7f342b",
        "libb": "8185202bb931ddc973bc9804810f49fb402b2d99349146b0033d2d15aeefcbe8",
        "libb/libm020": "cd9c271067a7f4f67691923f776b1a7c976db0e37778c3f39cbdf95f0d57fdf2",
        "libb/libm020/libm881": "cd9c271067a7f4f67691923f776b1a7c976db0e37778c3f39cbdf95f0d57fdf2",
        "libb/libm060": "64cc78483d1cd8be8f07198e2d2392f4dbe7f70b50aea1d5ca278972072ef109",
        "libb32/libm020": "07087e90772a0193dfd50298d6aa36928bd6e2e6662eb29595e963517ecc9510",
        "libb32/libm020/libm881": "07087e90772a0193dfd50298d6aa36928bd6e2e6662eb29595e963517ecc9510",
        "libb32/libm060": "8147a07f4f8f3420414277802ff11823658d3eb48b92a89487aa19e728264e41",
    },
}


_FUNC = re.compile(r"^[0-9a-f]+ [0-9a-f]+ (\S+):$")
_INS = re.compile(r"^\s*([0-9a-f]+):\t[0-9a-f ]+\t(.*)$")


def _function(objdump, obj, name):
    """[(address, instruction)] of one function in obj."""
    out = subprocess.run([objdump, "-d", str(obj)], check=True,
                         capture_output=True, text=True).stdout
    body, inside = [], False
    for line in out.splitlines():
        f = _FUNC.match(line)
        if f:
            inside = f.group(1) == name
            continue
        m = _INS.match(line)
        if inside and m:
            body.append((int(m.group(1), 16), m.group(2).strip()))
    return body


def _is_call(ins):
    return ins.startswith(("jsr", "bsr", "jbsr"))


def _calls(body, i, name):
    """body[i] calls name, directly or through `lea name(pc),aN; jsr (aN)`."""
    ins = body[i][1]
    if not _is_call(ins):
        return False
    if name in ins:
        return True
    m = re.fullmatch(r"jsr \((a\d)\)", ins)
    return bool(m) and any(name in b and b.endswith("," + m.group(1))
                           for _, b in body[max(0, i - 3):i])


def _branch_target(ins):
    m = re.match(r"(b(?!sr)[a-z]+|db[a-z]+ d\d,|jmp)\.?[a-z]?\s+([0-9a-f]+)\s", ins + " ")
    if not m or ins.startswith(("btst", "bchg", "bclr", "bset")):
        return None
    return int(m.group(2), 16)


def _writes(ins, reg):
    """Instruction changes reg (destination, or post-inc/pre-dec addressing)."""
    if ins.startswith(("cmp", "tst")):
        return False
    return ins.endswith("," + reg) or f"({reg})+" in ins or f"-({reg})" in ins


def _moves_sp(ins):
    return (_writes(ins, "sp") or "(sp)+" in ins or "-(sp)" in ins) and not _is_call(ins)


def sys_alloc_returns_node(objdump, obj):
    """True if __sys_alloc's success path returns AllocMem's block + 4.

    Tracks d0 and the address registers as offsets from the AllocMem result
    along the fall-through path (the beq after it is the null return).
    Seen: `movea.l d0,a0; move.l X,(a0)+; move.l a0,d0` and
    `movea.l d0,a0; addq.l #4,d0; move.l X,(a0)`.
    """
    body = [i for _, i in _function(objdump, obj, "__Z11__sys_allocj")]
    if body.count("jsr -198(a6)") != 1:
        return False
    regs = {"d0": 0}
    for ins in body[body.index("jsr -198(a6)") + 1:]:
        if ins == "rts":
            return regs.get("d0") == 4
        m = re.fullmatch(r"move(?:a)?\.l (d0|a\d),(d0|a\d)", ins)
        if m:
            regs[m.group(2)] = regs.get(m.group(1))
            continue
        m = re.fullmatch(r"addq\.l #(\d),(d0|a\d)", ins)
        if m:
            if regs.get(m.group(2)) is not None:
                regs[m.group(2)] += int(m.group(1))
            continue
        for r in list(regs):
            if f"({r})+" in ins and regs[r] is not None:
                regs[r] += 4
            elif f"-({r})" in ins and regs[r] is not None:
                regs[r] -= 4
            elif ins.endswith("," + r) and not ins.startswith(("cmp", "tst")):
                regs[r] = None
        if _is_call(ins):
            return False
    return False


def _spilled_null(body, load, slot):
    """Slot N(sp) read at body[load] provably holds find()'s NULL.

    Walking back from the load, the slot's last writer must be `move.l aY,N(sp)`
    with sp unmoved in between. aY's last writer before that must be
    `movea.l d0,aY` straight after the call to MemMap::find, then `tst.l d0`
    and a branch that puts the null case into a window ending at the load:
      bne T  (T past the load)  the window starts after the bne;
      beq T  (T before the spill) the window starts at T, and the
             instruction before T must not fall through (rts/bra/jmp).
    No branch from outside the window may land inside it.
    """
    k = load - 1
    while k >= 0:
        ins = body[k][1]
        if _moves_sp(ins):
            return False
        if ins.endswith(f",{slot}(sp)"):
            break
        k -= 1
    else:
        return False
    m = re.fullmatch(r"move\.l (a\d),%s\(sp\)" % slot, body[k][1])
    if not m:
        return False
    reg = m.group(1)
    addr = {a: n for n, (a, _) in enumerate(body)}
    load_at = body[load][0]
    for j in range(1, len(body) - 2):
        if not (body[j][1] == f"movea.l d0,{reg}" and _calls(body, j - 1, "__ZN6MemMap4findEi")
                and body[j + 1][1] == "tst.l d0"):
            continue
        br, target = body[j + 2][1], _branch_target(body[j + 2][1])
        if target is None:
            continue
        if br.startswith("bne") and target > load_at and j + 3 <= k:
            start = j + 3
        elif br.startswith("beq") and target in addr and j + 2 < addr[target] <= k:
            start = addr[target]
            prev = body[start - 1][1]
            if not (prev == "rts" or prev.startswith(("bra", "jmp"))):
                continue
        else:
            continue
        # aY unchanged, and no call, from the window start to the spill.
        if any(_writes(x, reg) or _is_call(x) for _, x in body[start:k]):
            continue
        lo, hi = body[start][0], load_at
        entered = False
        for n, (at, ins) in enumerate(body):
            t = _branch_target(ins)
            if t is None or n == j + 2 or not (lo <= t <= hi):
                continue
            if not (lo <= at < hi):
                entered = True
        if not entered:
            return True
    return False


def prev_stores(objdump, obj):
    """One (basis, verdict) per fresh allocation site in MemMap::alloc."""
    body = _function(objdump, obj, "__ZN6MemMap5allocEj")
    node_call = None
    sites = []
    for i, (_, ins) in enumerate(body):
        if ins == "jsr -198(a6)":
            sites.append((i, "inline"))
        elif _calls(body, i, "__Z11__sys_allocj"):
            if node_call is None:
                node_call = sys_alloc_returns_node(objdump, obj)
            sites.append((i, "call" if node_call else "unproven"))
    out = []
    for i, basis in sites:
        if basis == "unproven":
            out.append((basis, "__sys_alloc does not return AllocMem+4"))
            continue
        m = re.fullmatch(r"movea\.l d0,(a\d)", body[i + 1][1] if i + 1 < len(body) else "")
        if not m:
            out.append((basis, "result not kept in an address register"))
            continue
        reg = m.group(1)
        off = 8 if basis == "inline" else 4
        size_stored = basis == "call"
        copies = set()
        verdict = None
        for k in range(i + 2, len(body)):
            at, ins = body[k]
            if _is_call(ins) or ins == "rts":
                break
            c = re.fullmatch(r"movea\.l d0,(a\d)", ins)
            if c and c.group(1) != reg:
                copies.add(c.group(1))
            if re.fullmatch(r"move\.l [^,]+,\((%s)\)\+?" % "|".join([reg] + sorted(copies)), ins):
                size_stored = True
            zero = ins == f"clr.l {off}({reg})"
            st = re.fullmatch(r"move\.l (\d+)\(sp\),%d\(%s\)" % (off, reg), ins)
            if zero or (st and _spilled_null(body, k, st.group(1))):
                # Only the AllocMem-failed exit (tst.l d0; beq, right after
                # the site) may jump past the store.
                skipped = [
                    a for n, (a, x) in enumerate(body[i + 1:k], i + 1)
                    if (_branch_target(x) or 0) > at
                    and not (x.startswith("beq") and n <= i + 4 and body[n - 1][1] == "tst.l d0")
                ]
                if skipped:
                    verdict = f"store at {at:x} can be branched over"
                elif not size_stored:
                    verdict = "no size store to 0(raw); basis unproven"
                else:
                    verdict = "clr" if zero else "spilled_null"
                break
            if _writes(ins, reg):
                break
        out.append((basis, verdict or f"no zero store to {off}({reg})"))
    return out


def structural(objdump, obj):
    verdicts = prev_stores(objdump, obj)
    if len(verdicts) != 2:
        return f"{len(verdicts)} allocation site(s) in MemMap::alloc, want 2"
    bad = [v for _, v in verdicts if v not in ("clr", "spilled_null")]
    return "; ".join(bad) if bad else None


def main() -> int:
    args = sys.argv[1:]
    if len(args) == 3 and args[0] == "--object":
        objdump = pathlib.Path(args[2]).resolve() / "bin/m68k-amigaos-objdump"
        why = structural(str(objdump), args[1])
        verdicts = prev_stores(str(objdump), args[1])
        print(f"malloc_prev_stores={','.join(b + ':' + v.replace(' ', '_') for b, v in verdicts) or 'none'}"
              f" result={'fail' if why else 'pass'} object={args[1]}")
        return 1 if why else 0
    series = None
    if len(args) == 3 and args[0] == "--series" and args[1] in EXPECTED_MALLOC_SHA256:
        series = args[1]
        args = args[2:]
    if len(args) != 1:
        print(
            f"usage: {sys.argv[0]} [--series {'|'.join(EXPECTED_MALLOC_SHA256)}] <toolchain-prefix>",
            file=sys.stderr,
        )
        return 2
    prefix = pathlib.Path(args[0]).resolve()
    ar = prefix / "bin/m68k-amigaos-ar"
    gcc = prefix / "bin/m68k-amigaos-gcc"
    libdir = prefix / "m68k-amigaos/lib"
    if not ar.is_file() or not gcc.is_file() or not libdir.is_dir():
        print(f"missing cross-gcc, cross-ar or library directory under {prefix}", file=sys.stderr)
        return 1

    failures = []
    multilibs = subprocess.run(
        [str(gcc), "-print-multi-lib"], check=False, capture_output=True, text=True
    )
    if multilibs.returncode:
        print("cross-gcc could not enumerate multilibs", file=sys.stderr)
        return 1
    compiler_variants = set()
    for line in multilibs.stdout.splitlines():
        if line.strip():
            variant = line.split(";", 1)[0]
            compiler_variants.add("" if variant == "." else variant)
    variants = set(EXPECTED_MALLOC_SHA256["16.2.3"])
    if compiler_variants != variants:
        failures.append(
            "compiler multilib paths differ from pinned allocator map: "
            f"{sorted(compiler_variants)}"
        )

    for archive_name in ("libc.a", "libg.a"):
        expected = {
            str(pathlib.PurePosixPath(variant) / archive_name)
            for variant in variants
        }
        actual = {
            archive.relative_to(libdir).as_posix()
            for archive in libdir.rglob(archive_name)
        }
        for extra in sorted(actual - expected):
            failures.append(f"m68k-amigaos/lib/{extra}: unexpected unverified multilib")
    got = {}
    objects = {}
    for variant in sorted(variants):
        for companion in ("crt0.o", "libm.a"):
            path = libdir / variant / companion
            if not path.is_file():
                failures.append(f"{path.relative_to(prefix)}: missing multilib companion")
        for archive_name in ("libc.a", "libg.a"):
            archive = libdir / variant / archive_name
            if not archive.is_file():
                failures.append(f"{archive.relative_to(prefix)}: missing archive")
                continue
            result = subprocess.run(
                [str(ar), "p", str(archive), "malloc.o"],
                check=False,
                capture_output=True,
            )
            if result.returncode or not result.stdout:
                failures.append(f"{archive.relative_to(prefix)}: missing malloc.o")
                continue
            got[archive] = (variant, hashlib.sha256(result.stdout).hexdigest())
            objects[archive] = result.stdout

    # Every archive must match one series; a prefix mixing two fails.
    candidates = [series] if series else list(EXPECTED_MALLOC_SHA256)
    matched = [
        name for name in candidates
        if all(h == EXPECTED_MALLOC_SHA256[name].get(v) for v, h in got.values())
    ]
    if not matched:
        # Report against the nearest series: a mixed prefix names its strays.
        near = max(
            candidates,
            key=lambda name: sum(
                h == EXPECTED_MALLOC_SHA256[name].get(v) for v, h in got.values()
            ),
        )
        want = EXPECTED_MALLOC_SHA256[near]
        for archive, (variant, h) in got.items():
            if h != want.get(variant):
                failures.append(
                    f"{archive.relative_to(prefix)}: malloc.o {h}, want {want.get(variant)}"
                    f" ({near})"
                )

    if matched and matched[0] in STRUCTURAL_SERIES:
        objdump = str(prefix / "bin/m68k-amigaos-objdump")
        with tempfile.TemporaryDirectory() as tmp:
            for archive, data in objects.items():
                obj = pathlib.Path(tmp) / "malloc.o"
                obj.write_bytes(data)
                why = structural(objdump, obj)
                if why:
                    failures.append(f"{archive.relative_to(prefix)}: malloc.o prev stores: {why}")

    if failures:
        print("toolchain allocator verification FAILED:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print(
        "toolchain allocator verification passed: 11 libc and 11 libg multilibs,"
        f" series {matched[0]}"
        + (", both prev stores proven" if matched[0] in STRUCTURAL_SERIES else "")
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
