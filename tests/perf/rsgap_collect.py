#!/usr/bin/env python3
"""Turn one run-rsgap.sh session's files into per-boot key=value lines.

  rsgap_collect.py --results DIR --images FILE --peer-log FILE
                   --schedule R,A,A,R --transfers 4 --bytes N --wd-secs S
                   [--fault-boot M] [--door FILE]

DIR holds what the guest wrote to SYS:rsgap: boot-<n>.kv/.wd per finished boot
and cur.kv/.wd for the last one (the door boot).  FILE lists the host-side md5
of every staged image as `rsgap_image arm=R mode=M guest=<guest path> md5=<hex>`,
arm R, A or *, mode measure, door or *.  A boot's required set is every entry
matching its arm and mode; its RECORD must hash each of them exactly once with
that md5 and nothing else, or hash_match=0.

A measure boot is valid only with the selector consumed, neither the stack nor
the driver loaded before SELECT, one stack of the selected arm, every hash
matching, every transfer complete and seen by the peer, and no watchdog.  A
door boot is valid with the selector absent, nothing preloaded, AmiNetXDuo
alone and every hash matching.

Measurement boots are paired by schedule position, (1,2), (3,4), ...; pair=K
names the pair, pair=- a boot outside any complete pair (the fault boot, its
partner, an odd last boot).  Validity never moves a boot to another pair.

Emits, in boot order:

  rsgap_boot boot=N arm=R mode=measure selector_consumed=1 stack=R
             one_stack=1 hash_match=1 hashes_expected=8 ... pos=1 pair=1
             xfers=4 ok=4 peer_match=1 wd_fired=0 next_arm=A kbps=...
             mean_kbps=... valid=1
  rsgap_recovery boot=N wd_fired=1 elapsed_s=E bound_s=S within_bound=1
             next_boot=N+1 next_arm=A next_selector=absent door=up
  rsgap_door boot=N arm=A ... door=up cancel_confirmed=1 valid=1
  rsgap_run boots=N measure=M door=D scheduled=S pairs_scheduled=P
            missing_pos=- expected_ok=1

Exit 0 when the session did exactly what the schedule asked for (every
unfaulted measurement boot valid, the fault boot recovered by the watchdog,
a door boot at the end with the door up and the watchdog cancelled over it),
1 otherwise.

SPDX-License-Identifier: MIT
"""

import argparse
import glob
import os
import re
import statistics
import sys

# Task names that identify a running stack.  "AmiNetXDuo stack" is
# src/bsdsocket/library.c's NP_Name; Roadshow's are the names it gives its
# own processes.
A_TASK = re.compile(r"AmiNetXDuo")
R_TASK = re.compile(r"(?i)roadshow|^bsdsocket|^TCP/IP|^netio|^ipc")


def kv_of(line):
    out = {}
    for tok in line.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


def read_boot(path_kv, path_wd):
    b = {"xfers": [], "md5": [], "boards": [], "mem": [], "wd": []}
    for path, key in ((path_kv, None), (path_wd, "wd")):
        if not path or not os.path.exists(path):
            continue
        with open(path, errors="replace") as fh:
            for line in fh:
                kv = kv_of(line.strip())
                rec = kv.get("rec")
                if rec is None:
                    continue
                if rec == "xfer":
                    b["xfers"].append(kv)
                elif rec == "md5":
                    b["md5"].append(kv)
                elif rec == "board":
                    b["boards"].append(kv)
                elif rec == "mem":
                    b["mem"].append(kv)
                elif rec == "wd":
                    b["wd"].append(kv)
                else:
                    b[rec] = kv
    return b


def load_images(path):
    """[(arm, mode, guest, md5)] in file order."""
    imgs = []
    with open(path) as fh:
        for line in fh:
            if line.startswith("rsgap_image "):
                kv = kv_of(line)
                if not all(k in kv for k in ("arm", "mode", "guest", "md5")):
                    sys.exit("rsgap_collect: bad image line: " + line.strip())
                imgs.append((kv["arm"], kv["mode"], kv["guest"], kv["md5"]))
    return imgs


def required_hashes(imgs, arm, mode):
    """guest path -> md5 for one boot's arm and mode."""
    req = {}
    for iarm, imode, guest, md5 in imgs:
        if iarm in (arm, "*") and imode in (mode, "*"):
            req[guest] = md5
    return req


def load_peer(path):
    """Peer connections in log order.  Keyed by nothing: a stack reuses the
    same ephemeral ports after every reboot (Roadshow starts at 1024), so a
    client address names a connection only within one boot."""
    conns = []
    if path and os.path.exists(path):
        with open(path) as fh:
            for line in fh:
                if "event=conn" in line:
                    kv = kv_of(line)
                    kv["used"] = False
                    conns.append(kv)
    return conns


def take_peer(conns, client, kind):
    """The first unused peer connection of this kind from this client."""
    for c in conns:
        if not c["used"] and c.get("kind") == kind and c.get("client") == client:
            c["used"] = True
            return c
    return None


def stack_seen(b):
    names = b.get("tasks", {}).get("names", "").split(",")
    names += b.get("ports", {}).get("names", "").split(",")
    a = any(A_TASK.search(n) for n in names)
    r = (not a and int(b.get("stacklib", {}).get("count", "0")) > 0) or \
        any(R_TASK.search(n) for n in names if not A_TASK.search(n))
    if a and r:
        return "both"
    return "A" if a else "R" if r else "none"


def hash_match(b, arm, mode, imgs):
    """Every required hash exactly once and right, nothing else.
    Returns (match, counts) with counts in output order."""
    req = required_hashes(imgs, arm, mode)
    count = {}
    bad = unexpected = 0
    for m in b["md5"]:
        guest = m.get("file", "")
        if guest not in req:
            unexpected += 1
            continue
        count[guest] = count.get(guest, 0) + 1
        if m.get("md5") != req[guest]:
            bad += 1
    missing = sum(1 for g in req if g not in count)
    dup = sum(c - 1 for c in count.values())
    ok = int(bool(req) and not (missing or dup or unexpected or bad))
    return ok, (("hashes_expected", len(req)), ("hashes_seen", len(b["md5"])),
                ("hash_missing", missing), ("hash_dup", dup),
                ("hash_unexpected", unexpected), ("hash_bad", bad))


def pair_of(pos, scheduled, fault_boot):
    """Pair id of schedule position pos (1-based), or None when its pair is
    incomplete by design."""
    mate = pos + 1 if pos % 2 else pos - 1
    if mate > scheduled or fault_boot in (pos, mate):
        return None
    return (pos - 1) // 2 + 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", required=True)
    ap.add_argument("--images", required=True)
    ap.add_argument("--peer-log")
    ap.add_argument("--door")
    ap.add_argument("--schedule", required=True)
    ap.add_argument("--transfers", type=int, default=4)
    ap.add_argument("--bytes", type=int, required=True)
    ap.add_argument("--wd-secs", type=int, required=True)
    ap.add_argument("--fault-boot", type=int, default=0)
    a = ap.parse_args()

    sched = [s for s in a.schedule.split(",") if s]
    imgs = load_images(a.images)
    peer = load_peer(a.peer_log)
    door = {}
    if a.door and os.path.exists(a.door):
        with open(a.door) as fh:
            for line in fh:
                door.update(kv_of(line))

    files = {}
    for p in glob.glob(os.path.join(a.results, "boot-*.kv")) + \
            glob.glob(os.path.join(a.results, "boot-*.wd")):
        n = int(re.search(r"boot-(\d+)\.", p).group(1))
        files.setdefault(n, {})[p.rsplit(".", 1)[1]] = p
    cur = read_boot(os.path.join(a.results, "cur.kv"),
                    os.path.join(a.results, "cur.wd"))
    boots = []
    for n in sorted(files):
        boots.append(read_boot(files[n].get("kv"), files[n].get("wd")))
    if "select" in cur:
        boots.append(cur)

    ok_all = True
    measure_seen = 0
    door_seen = 0
    expect = list(sched)
    if a.fault_boot:
        expect = sched[:a.fault_boot]

    for i, b in enumerate(boots):
        sel = b.get("select", {})
        n = sel.get("boot", "?")
        arm = sel.get("arm", "?")
        mode = sel.get("mode", "?")
        fired = [w for w in b["wd"] if w.get("state") == "fired"]
        cancelled = [w for w in b["wd"] if w.get("state") == "cancelled"]
        seen = stack_seen(b)
        one = 1 if seen == arm else 0
        hm, hcounts = hash_match(b, arm, mode, imgs)
        pre_ok = (sel.get("bsdsocket_preloaded") == "0" and
                  sel.get("driver_preloaded") == "0")
        xs = b["xfers"]
        data = [x for x in xs if x.get("tag") != "door"]
        good = [x for x in data if x.get("result") == "ok"]
        pm = 1
        for x in data:
            pc = take_peer(peer, x.get("local"), "data")
            if pc is None or pc.get("bytes") != x.get("rx_bytes"):
                pm = 0
        kbps = [float(x["rx_kbps"]) for x in good]
        mean = statistics.fmean(kbps) if kbps else 0.0
        nxt = b.get("next", {}).get("next_arm", "-")
        consumed = sel.get("consumed", "0")
        common = ("boot=%s arm=%s mode=%s selector=%s selector_consumed=%s "
                  "bsdsocket_preloaded=%s driver_preloaded=%s stack=%s "
                  "one_stack=%d stacklib_id=%s driver_version=%s cpu=%s "
                  "attnflags=%s cachecontrol=%s z3_boards=%d hash_match=%d "
                  "%s"
                  % (n, arm, mode, sel.get("selector", "?"), consumed,
                     sel.get("bsdsocket_preloaded", "?"),
                     sel.get("driver_preloaded", "?"), seen, one,
                     b.get("stacklib", {}).get("id", "-"),
                     b.get("driver", {}).get("version", "-"),
                     b.get("sys", {}).get("cpu", "-"),
                     b.get("sys", {}).get("attnflags", "-"),
                     b.get("sys", {}).get("cachecontrol", "-"),
                     sum(1 for x in b["boards"] if x.get("zorro") == "3"),
                     hm, " ".join("%s=%d" % kv for kv in hcounts)))
        if mode == "measure":
            measure_seen += 1
            faulted = a.fault_boot and measure_seen == a.fault_boot
            valid = int(len(good) == a.transfers and one and hm and pm and
                        consumed == "1" and pre_ok and
                        not fired and all(int(x["rx_bytes"]) == a.bytes
                                          for x in good))
            pair = pair_of(measure_seen, len(expect), a.fault_boot)
            print("rsgap_boot %s pos=%d pair=%s xfers=%d ok=%d peer_match=%d "
                  "wd_fired=%d next_arm=%s kbps=%s mean_kbps=%.2f valid=%d"
                  % (common, measure_seen, pair or "-", len(data), len(good),
                     pm, 1 if fired else 0, nxt,
                     ",".join("%.2f" % k for k in kbps) or "-", mean, valid))
            want_arm = expect[measure_seen - 1] if measure_seen <= len(expect) \
                else None
            if arm != want_arm:
                ok_all = False
            if faulted:
                nb = boots[i + 1] if i + 1 < len(boots) else {}
                nsel = nb.get("select", {})
                el = int(fired[0]["elapsed_s"]) if fired else -1
                within = int(bool(fired) and el <= a.wd_secs + 5)
                rec_ok = (fired and within and nsel.get("arm") == "A" and
                          nsel.get("selector") == "absent" and
                          nsel.get("mode") == "door" and
                          door.get("door") == "up")
                print("rsgap_recovery boot=%s arm=%s wd_fired=%d elapsed_s=%d "
                      "bound_s=%d within_bound=%d next_boot=%s next_arm=%s "
                      "next_selector=%s next_mode=%s next_stack=%s door=%s"
                      % (n, arm, 1 if fired else 0, el, a.wd_secs, within,
                         nsel.get("boot", "-"), nsel.get("arm", "-"),
                         nsel.get("selector", "-"), nsel.get("mode", "-"),
                         stack_seen(nb) if nb else "-",
                         door.get("door", "down")))
                if not rec_ok:
                    ok_all = False
            elif not valid:
                ok_all = False
        else:
            door_seen += 1
            dx = [x for x in xs if x.get("tag") == "door"]
            dpc = take_peer(peer, dx[0].get("local"), "door") if dx else None
            dvalid = int(sel.get("selector") == "absent" and consumed == "0" and
                         pre_ok and one == 1 and hm == 1)
            print("rsgap_door %s door_rx=%s door_peer_match=%d wd_fired=%d "
                  "wd_cancelled=%d wd_cancel_via=%s wd_cancel_elapsed_s=%s "
                  "door=%s door_http=%s door_fetch=%s cancel_put=%s "
                  "cancel_confirmed=%s valid=%d"
                  % (common, dx[0].get("result", "-") if dx else "none",
                     1 if dpc and dpc.get("bytes") == dx[0].get("rx_bytes")
                     else 0,
                     1 if fired else 0, 1 if cancelled else 0,
                     cancelled[0].get("via", "-") if cancelled else "-",
                     cancelled[0].get("elapsed_s", "-") if cancelled else "-",
                     door.get("door", "down"), door.get("door_http", "-"),
                     door.get("door_fetch", "-"), door.get("cancel_put", "-"),
                     door.get("cancel_confirmed", "0"), dvalid))
            if i != len(boots) - 1 or door.get("door") != "up" or \
               door.get("cancel_confirmed") != "1" or not dvalid:
                ok_all = False

    if measure_seen != len(expect) or door_seen != 1:
        ok_all = False
    npairs = sum(1 for p in range(1, len(expect) + 1, 2)
                 if pair_of(p, len(expect), a.fault_boot))
    missing = list(range(measure_seen + 1, len(expect) + 1))
    print("rsgap_run boots=%d measure=%d door=%d scheduled=%d fault_boot=%d "
          "pairs_scheduled=%d missing_pos=%s expected_ok=%d"
          % (len(boots), measure_seen, door_seen, len(expect), a.fault_boot,
             npairs, ",".join(map(str, missing)) or "-", 1 if ok_all else 0))
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
