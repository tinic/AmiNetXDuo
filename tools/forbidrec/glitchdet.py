#!/usr/bin/env python3
#
# Glitches in a microphone recording of tools/forbidrec/tone.
#
#   glitchdet.py detect WAV [--freq HZ] [--from S] [--to S] [--events]
#   glitchdet.py validate WAV [--freq HZ] [--seed N]
#   glitchdet.py dump RAW [--freq HZ] [--rate HZ]
#
# detect: band-pass round the tone, demodulate it to a complex amplitude
# every 1 ms over 5 ms, and compare that with the steady sine the rest of
# the recording shows.  An event is
#   dropout  envelope under -12 dB of the reference
#   dip      envelope under -3 dB
#   phase    the phase steps by more than PHASE_STEP rad and stays there:
#            a block played twice or not at all
# each with its start (seconds into the WAV), its duration and its depth.
# The reference amplitude and the phase advance are running medians, so
# speaker and clock drift are not events.
#
# validate: inject 2, 5, 10 and 50 ms gaps and 5, 10 and 25 ms repeated
# blocks into a copy of WAV (a clean recording), and report how many detect
# finds, how far off its durations are, and what it finds in WAV itself.
#
# dump: check a `tone DUMP=` file (big-endian S16 stereo): level, SINAD,
# THD and the fit residual at buffer joins.
#
# Repeats only show as a phase step when the block is not a whole number of
# cycles: at 1170 Hz every block of 1, 2, 5, 10, 25, 50 or 250 ms is at
# least 0.15 cycle off one, so the tone for this is FREQ=1170.
#
# Output is key=value lines.  Needs numpy and scipy.
#
# SPDX-License-Identifier: MIT

import argparse
import sys

import numpy as np
from scipy.io import wavfile
from scipy.ndimage import median_filter
from scipy.signal import butter, hilbert, sosfiltfilt

WIN_MS = 5.0
HOP_MS = 1.0
DROPOUT_DB = -12.0
DIP_DB = -3.0
PHASE_STEP = 0.45
REF_S = 1.0
STEP_MS = 10
EDGE_S = 0.25


def load(path):
    rate, x = wavfile.read(path)
    x = x.astype(np.float64)
    if x.ndim > 1:
        x = x[:, 0]
    return rate, x


def estimate_freq(x, rate, guess):
    n = min(len(x), rate * 20)
    seg = x[:n] * np.blackman(n)
    spec = np.abs(np.fft.rfft(seg))
    f = np.fft.rfftfreq(n, 1.0 / rate)
    band = (f > guess * 0.9) & (f < guess * 1.1)
    k = np.flatnonzero(band)[np.argmax(spec[band])]
    a, b, c = np.log(spec[k - 1:k + 2] + 1e-12)
    return f[k] + 0.5 * (a - c) / (a - 2 * b + c) * (f[1] - f[0])


def analyse(x, rate, f0):
    """Per-hop complex amplitude of the tone, and the sample-rate envelope."""
    sos = butter(4, [f0 * 0.7, f0 * 1.3], btype="bandpass", fs=rate, output="sos")
    y = sosfiltfilt(sos, x)
    t = np.arange(len(y)) / rate
    z = y * np.exp(-2j * np.pi * f0 * t)
    win = int(rate * WIN_MS / 1000)
    hop = int(rate * HOP_MS / 1000)
    c = np.concatenate([[0], np.cumsum(z)])
    starts = np.arange(0, len(z) - win, hop)
    zz = 2 * (c[starts + win] - c[starts]) / win
    tc = (starts + win / 2) / rate
    env = np.abs(hilbert(y))
    k = max(1, int(rate * 0.001))
    env = np.convolve(env, np.ones(k) / k, mode="same")
    return tc, zz, env


def detect(x, rate, f0):
    tc, zz, env = analyse(x, rate, f0)
    amp = np.abs(zz)
    nref = int(REF_S * 1000 / HOP_MS) | 1
    aref = median_filter(amp, size=nref, mode="nearest")
    good = amp > aref * 10 ** (DIP_DB / 20)

    # Phase: the hop-to-hop advance, less its running median (the clock
    # offset between the Amiga and the recorder), summed.  A median of the
    # phase itself would follow a step and hide it.
    dphi = np.angle(zz[1:] * np.conj(zz[:-1]))
    dphi[~(good[1:] & good[:-1])] = 0.0
    dphi -= median_filter(dphi, size=nref, mode="nearest")
    res = np.concatenate([[0.0], np.cumsum(dphi)])
    events = []
    hop = HOP_MS / 1000

    # Amplitude events, at hop resolution first, then measured on the envelope.
    db = 20 * np.log10(np.maximum(amp, 1e-9) / np.maximum(aref, 1e-9))
    low = db < DIP_DB
    i = 0
    n = len(low)
    while i < n:
        if not low[i]:
            i += 1
            continue
        j = i
        while j + 1 < n and low[j + 1]:
            j += 1
        s0 = max(0, int((tc[i] - WIN_MS / 2000) * rate))
        s1 = min(len(env), int((tc[j] + WIN_MS / 2000) * rate))
        ref = float(np.median(aref[i:j + 1]))
        seg = env[s0:s1]
        under = seg < ref * 10 ** (DIP_DB / 20)
        if under.any():
            u = np.flatnonzero(under)
            start = (s0 + u[0]) / rate
            dur = (u[-1] - u[0] + 1) / rate
        else:
            start = tc[i]
            dur = (j - i + 1) * hop
        depth = float(db[i:j + 1].min())
        kind = "dropout" if depth < DROPOUT_DB else "dip"
        events.append(dict(t=start, dur=dur, kind=kind, depth=depth, step=0.0))
        i = j + 1

    # Phase steps: the median phase over STEP_MS before against after, using
    # only windows where the tone is at full level.
    m = int(STEP_MS / HOP_MS)
    step = np.zeros(len(res))
    for k in range(m, len(res) - m):
        a = res[k - m:k]
        b = res[k + 1:k + 1 + m]
        ga = good[k - m:k]
        gb = good[k + 1:k + 1 + m]
        if ga.sum() < m // 2 or gb.sum() < m // 2:
            continue
        d = np.median(b[gb]) - np.median(a[ga])
        step[k] = (d + np.pi) % (2 * np.pi) - np.pi
    big = np.abs(step) > PHASE_STEP
    k = 0
    while k < len(big):
        if not big[k]:
            k += 1
            continue
        j = k
        while j + 1 < len(big) and big[j + 1]:
            j += 1
        p = k + int(np.argmax(np.abs(step[k:j + 1])))
        t = tc[p]
        # One glitch, one event: a step inside an amplitude event is its.
        hit = [e for e in events if e["t"] - 0.02 <= t <= e["t"] + e["dur"] + 0.02]
        if hit:
            hit[0]["step"] = float(step[p])
        else:
            events.append(dict(t=t, dur=(j - k + 1) * hop, kind="phase",
                               depth=float(db[p]), step=float(step[p])))
        k = j + 1
    # The band-pass and the windows ring in and out at the ends.
    secs = len(x) / rate
    events = [e for e in events if EDGE_S <= e["t"] <= secs - EDGE_S]
    events.sort(key=lambda e: e["t"])
    return events, dict(aref=float(np.median(aref)), phase_noise=float(np.std(dphi[good[1:]])),
                        secs=len(x) / rate)


BUCKETS = [(0, 2, "lt_2ms"), (2, 5, "2_5ms"), (5, 10, "5_10ms"), (10, 50, "10_50ms"),
           (50, 1e9, "ge_50ms")]


def hist(events):
    h = {name: 0 for _, _, name in BUCKETS}
    for e in events:
        ms = e["dur"] * 1000
        for lo, hi, name in BUCKETS:
            if lo <= ms < hi:
                h[name] += 1
    return h


def summary(tag, events, info):
    mins = info["secs"] / 60
    kinds = {k: sum(e["kind"] == k for e in events) for k in ("dropout", "dip", "phase")}
    h = hist(events)
    print("%s secs=%.1f events=%d per_min=%.2f dropout=%d dip=%d phase=%d %s "
          "tone_amp=%.0f phase_noise_rad=%.3f" % (
              tag, info["secs"], len(events), len(events) / mins if mins else 0,
              kinds["dropout"], kinds["dip"], kinds["phase"],
              " ".join("%s=%d" % kv for kv in h.items()), info["aref"], info["phase_noise"]))


def cmd_detect(a):
    rate, x = load(a.wav)
    s0 = int(a.start * rate)
    s1 = int(a.end * rate) if a.end else len(x)
    x = x[s0:s1]
    f0 = estimate_freq(x, rate, a.freq)
    events, info = detect(x, rate, f0)
    print("tone freq_hz=%.3f rate=%d" % (f0, rate))
    if a.events:
        for e in events:
            print("event t_s=%.4f dur_ms=%.1f kind=%s depth_db=%.1f phase_step_rad=%.2f" % (
                e["t"] + a.start, e["dur"] * 1000, e["kind"], e["depth"], e["step"]))
    summary("result", events, info)


def cmd_validate(a):
    rate, x = load(a.wav)
    f0 = estimate_freq(x, rate, a.freq)
    clean, info = detect(x, rate, f0)
    summary("clean", clean, info)
    for e in clean:
        print("clean_event t_s=%.4f dur_ms=%.1f kind=%s depth_db=%.1f phase_step_rad=%.2f" % (
            e["t"], e["dur"] * 1000, e["kind"], e["depth"], e["step"]))
    rng = np.random.default_rng(a.seed)
    plan = [("gap", 2), ("gap", 5), ("gap", 10), ("gap", 50),
            ("repeat", 5), ("repeat", 10), ("repeat", 25)] * a.each
    rng.shuffle(plan)
    y = x.copy()
    spacing = (len(x) / rate - 2.0) / (len(plan) + 1)
    if spacing < 0.4:
        sys.exit("validate: recording too short for %d injections" % len(plan))
    inj = []
    for i, (kind, ms) in enumerate(plan):
        t = 1.0 + spacing * (i + 1) + rng.uniform(-0.1, 0.1)
        s = int(t * rate)
        n = int(ms * rate / 1000)
        if kind == "gap":
            y[s:s + n] = 0
        else:
            # The block just played, played again: the rest arrives late.
            y = np.concatenate([y[:s], y[s - n:s], y[s:len(y) - n]])
        inj.append((t, kind, ms))
    found, _ = detect(y, rate, f0)
    used = set()
    per = {}
    for t, kind, ms in inj:
        best = None
        for k, e in enumerate(found):
            if k not in used and abs(e["t"] - t) < 0.03:
                best = k
                break
        key = "%s_%dms" % (kind, ms)
        p = per.setdefault(key, dict(n=0, hit=0, durs=[]))
        p["n"] += 1
        if best is not None:
            used.add(best)
            p["hit"] += 1
            p["durs"].append(found[best]["dur"] * 1000)
            print("inject t_s=%.3f kind=%s ms=%d found=%s dur_ms=%.1f depth_db=%.1f "
                  "phase_step_rad=%.2f" % (t, kind, ms, found[best]["kind"],
                                           found[best]["dur"] * 1000, found[best]["depth"],
                                           found[best]["step"]))
        else:
            print("inject t_s=%.3f kind=%s ms=%d found=none" % (t, kind, ms))
    extra = [e for k, e in enumerate(found) if k not in used]
    for key, p in sorted(per.items()):
        print("validate case=%s injected=%d detected=%d median_dur_ms=%s" % (
            key, p["n"], p["hit"], "%.1f" % np.median(p["durs"]) if p["durs"] else "na"))
    tot = sum(p["n"] for p in per.values())
    hit = sum(p["hit"] for p in per.values())
    print("validate injected=%d detected=%d extra_events=%d clean_events=%d clean_secs=%.1f" % (
        tot, hit, len(extra), len(clean), info["secs"]))
    return 0 if hit == tot and not clean and not extra else 1


def cmd_dump(a):
    raw = np.frombuffer(open(a.raw, "rb").read(), dtype=">i2").reshape(-1, 2).astype(float)
    left, right = raw[:, 0], raw[:, 1]
    t = np.arange(len(left)) / a.rate
    basis = np.c_[np.sin(2 * np.pi * a.freq * t), np.cos(2 * np.pi * a.freq * t)]
    c, *_ = np.linalg.lstsq(basis, left, rcond=None)
    res = left - basis @ c
    amp = np.hypot(*c)
    spec = np.abs(np.fft.rfft(left * np.blackman(len(left)))) ** 2
    f = np.fft.rfftfreq(len(left), 1.0 / a.rate)

    def band(fc):
        i = np.argmin(np.abs(f - fc))
        return spec[i - 6:i + 7].sum()

    harm = sum(band(m * a.freq) for m in range(2, 11) if m * a.freq < a.rate / 2)
    joins = np.arange(a.buf, len(left), a.buf)
    jmax = np.abs(res[np.r_[joins - 1, joins]]).max() if len(joins) else 0.0
    sinad = 20 * np.log10(amp / np.sqrt(2) / max(res.std(), 1e-9))
    thd = 10 * np.log10(max(harm, 1e-30) / band(a.freq))
    ok = np.array_equal(left, right) and sinad > 60 and thd < -60 and jmax < 4
    print("dump frames=%d stereo_equal=%d peak=%d min=%d dbfs=%.2f sinad_db=%.1f thd_db=%.1f "
          "join_residual=%.2f verdict=%s" % (len(left), np.array_equal(left, right), left.max(),
                                             left.min(), 20 * np.log10(amp / 32768), sinad, thd,
                                             jmax, "pass" if ok else "fail"))
    return 0 if ok else 1


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sp = p.add_subparsers(dest="cmd", required=True)
    d = sp.add_parser("detect")
    d.add_argument("wav")
    d.add_argument("--freq", type=float, default=1170)
    d.add_argument("--from", dest="start", type=float, default=0.0)
    d.add_argument("--to", dest="end", type=float, default=0.0)
    d.add_argument("--events", action="store_true")
    v = sp.add_parser("validate")
    v.add_argument("wav")
    v.add_argument("--freq", type=float, default=1170)
    v.add_argument("--seed", type=int, default=353)
    v.add_argument("--each", type=int, default=5)
    u = sp.add_parser("dump")
    u.add_argument("raw")
    u.add_argument("--freq", type=float, default=1170)
    u.add_argument("--rate", type=int, default=44100)
    u.add_argument("--buf", type=int, default=11025)
    a = p.parse_args()
    return {"detect": cmd_detect, "validate": cmd_validate, "dump": cmd_dump}[a.cmd](a) or 0


if __name__ == "__main__":
    sys.exit(main())
