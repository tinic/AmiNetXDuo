#!/usr/bin/env python3
"""The key tables of src/tools/updkeys.c, for the release scripts.

  updkeys.py list  <updkeys.c>                 key=<id> pk=<hex> build=<b>
  updkeys.py check <updkeys.c> <container>...  the release check

updkeys.c holds two tables: the UPD_TEST_KEY one (the scenario build) and
the release one (#else).  Each entry is the id string, then 32 0x.. byte
literals; upd_key_count follows each table.

check refuses, per stored (-lh0-) update container, an UpdateAmiNetXDuo that
does not carry every release key's 32 bytes and id, or that carries a test
key's bytes.  A release table with no key is refused too: such a tool
accepts no manifest.  The container stores its members unpacked, so the
tool's bytes are in it verbatim.

Output is key=value plus an exit code: 0 good, 1 refused, 2 cannot run.

SPDX-License-Identifier: MIT
"""

import re
import sys

ENTRY = re.compile(r'\{\s*"([a-z0-9-]+)"\s*,\s*\{((?:\s*0x[0-9a-fA-F]{2}\s*,?){32})\s*\}\s*\}')
COUNT = re.compile(r'upd_key_count\s*=\s*([0-9]+)\s*;')


def tables(path):
    text = open(path, encoding="ascii").read()
    m = re.search(r'#ifdef\s+UPD_TEST_KEY(.*?)#else(.*?)#endif', text, re.S)
    if not m:
        raise ValueError("no #ifdef UPD_TEST_KEY / #else / #endif")
    out = {}
    for name, body in (("test", m.group(1)), ("release", m.group(2))):
        keys = []
        for e in ENTRY.finditer(body):
            pk = bytes(int(b, 16) for b in re.findall(r'0x([0-9a-fA-F]{2})', e.group(2)))
            keys.append((e.group(1), pk))
        c = COUNT.search(body)
        if not c:
            raise ValueError("table=%s no upd_key_count" % name)
        n = int(c.group(1))
        if n > len(keys):
            raise ValueError("table=%s upd_key_count=%d entries=%d" % (name, n, len(keys)))
        out[name] = keys[:n]
    return out


def main(argv):
    if len(argv) < 3 or argv[1] not in ("list", "check"):
        print("update_keys=FAIL usage=list|check_updkeys.c_[containers]")
        return 2
    try:
        t = tables(argv[2])
    except (OSError, ValueError) as e:
        print("update_keys=FAIL source=%s reason=%s" % (argv[2], str(e).replace(" ", "_")))
        return 2

    if argv[1] == "list":
        for build in ("release", "test"):
            for kid, pk in t[build]:
                print("key=%s pk=%s build=%s" % (kid, pk.hex(), build))
        return 0

    if not t["release"]:
        print("update_keys=FAIL release_keys=0")
        return 1
    if len(argv) < 4:
        print("update_keys=FAIL containers=0")
        return 2
    for path in argv[3:]:
        try:
            data = open(path, "rb").read()
        except OSError:
            print("update_keys=FAIL unreadable=%s" % path)
            return 2
        name = path.rsplit("/", 1)[-1]
        for kid, pk in t["test"]:
            if pk in data:
                print("update_keys=FAIL container=%s test_key=%s" % (name, kid))
                return 1
        for kid, pk in t["release"]:
            if pk not in data or kid.encode("ascii") + b"\0" not in data:
                print("update_keys=FAIL container=%s missing_release_key=%s" % (name, kid))
                return 1
    print("update_keys=PASS release=%s containers=%d" %
          (",".join(k for k, _ in t["release"]), len(argv) - 3))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
