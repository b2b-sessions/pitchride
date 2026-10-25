#!/usr/bin/env python3
"""Report on an "a deck each" test (run_two_mixxx.sh with TOGETHER=1).

Usage:
    python3 tools/b2b/analyse_together.py OUT_DIR

1. Decks: at every moment, is each deck at the same point in its track on
   both computers, as heard? (Same computer, so its clock is ground truth.)
2. Moves: how late did each move from the friend arrive, compared with the
   moment it was meant to be heard? (> 0 = late; due within half a buffer.)
3. Sound: the two recordings, lined up. Both should be the same mix: for
   each move, when it is heard in each recording, and how different the two
   mixes are around it.
"""

import glob
import os
import re
import statistics
import sys
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from analyse_mixxx import load, stats  # noqa: E402


def heard_positions(rows):
    by_deck = {}
    for r in rows:
        if r["kind"] == "P" and r["value"] > 0.5:
            by_deck.setdefault(r["seq"], []).append((r["t"], r["beat"], r["extra"]))
    return by_deck


def alignment(host_rows, guest_rows):
    host, guest = heard_positions(host_rows), heard_positions(guest_rows)
    for deck in sorted(set(host) & set(guest)):
        g = guest[deck]
        gt = [x[0] for x in g]
        errs = []
        for t, pos, _ in host[deck]:
            i = np.searchsorted(gt, t)
            if i == 0 or i >= len(g):
                continue
            t0, p0, s0 = g[i - 1]
            t1, p1, _ = g[i]
            if t1 - t0 > 0.2e9 or abs((p1 - p0) - s0 * (t1 - t0) / 1e9) > 0.01:
                continue  # a gap or a jump between the two samples
            guest_pos = p0 + s0 * (t - t0) / 1e9
            errs.append((pos - guest_pos) * 1000.0)
        if len(errs) > 20:
            errs = errs[10:]  # settling after a start
        print(f"  deck {deck + 1}: host vs guest, heard   {stats(errs)}")


def lateness(rows, who):
    # Not refreshes, track loads (main thread), or the first seconds.
    t0 = min(r["t"] for r in rows) + 3e9
    late = [r["extra"] for r in rows if r["kind"] == "E" and r["beat"] == 10.0 and r["seq"] < 0x80000000
            and r["t"] > t0]
    print(f"  moves from the {who}:   {stats(late)}")


def read_wav(path):
    with wave.open(path) as w:
        rate = w.getframerate()
        n = w.getnframes()
        width = w.getsampwidth()
        data = w.readframes(n)
    if width == 2:
        x = np.frombuffer(data, dtype="<i2").astype(np.float32) / 32768.0
    elif width == 4:
        x = np.frombuffer(data, dtype="<i4").astype(np.float32) / 2147483648.0
    else:
        raise SystemExit(f"{path}: {width * 8}-bit WAV not handled")
    x = x.reshape(-1, 2).mean(axis=1)
    return rate, x


def envelope(x, rate, hop_ms=2.0):
    hop = int(rate * hop_ms / 1000)
    n = len(x) // hop
    frames = x[: n * hop].reshape(n, hop)
    return np.sqrt((frames ** 2).mean(axis=1) + 1e-12), hop


def line_up(a, b, rate):
    """Samples to drop from the start of b so it lines up with a."""
    ea, hop = envelope(a, rate, 5.0)
    eb, _ = envelope(b, rate, 5.0)
    ea, eb = ea - ea.mean(), eb - eb.mean()
    corr = np.correlate(ea[: min(len(ea), 4000)], eb[: min(len(eb), 6000)], mode="full")
    coarse = (np.argmax(corr) - (min(len(eb), 6000) - 1)) * hop  # b lags a by -coarse samples
    shift = -coarse
    # Fine: cross-correlate the raw sound around the coarse estimate.
    start = rate * 5
    win = rate * 2
    seg_a = a[start: start + win]
    best, best_lag = -1e9, 0
    for lag in range(-hop * 2, hop * 2 + 1):
        s = start + shift + lag
        if s < 0 or s + win > len(b):
            continue
        c = float(np.dot(seg_a, b[s: s + win]))
        if c > best:
            best, best_lag = c, lag
    return shift + best_lag


def step_time(env, around, hop_ms, span_ms=120):
    """Where the loudness changes most within span_ms of `around` (frames)."""
    lo = max(1, around - int(span_ms / hop_ms))
    hi = min(len(env) - 1, around + int(span_ms / hop_ms))
    if hi - lo < 4:
        return None
    k = 3  # compare a few frames either side, so single beats don't win
    best, best_i = 0.0, None
    for i in range(lo + k, hi - k):
        before = env[i - k: i].mean()
        after = env[i: i + k].mean()
        d = abs(20 * np.log10(after / before))
        if d > best:
            best, best_i = d, i
    return best_i


def sound(out, host_log, guest_log):
    ra = sorted(glob.glob(os.path.join(out, "rec-leader", "*.wav")))
    rb = sorted(glob.glob(os.path.join(out, "rec-follower", "*.wav")))
    if not ra or not rb:
        print("  (no recordings)")
        return
    rate, a = read_wav(ra[-1])
    rate_b, b = read_wav(rb[-1])
    if rate != rate_b:
        print("  recordings at different sample rates")
        return
    shift = line_up(a, b, rate)
    b = b[shift:] if shift >= 0 else np.concatenate([np.zeros(-shift, dtype=np.float32), b])
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    # Finer, over a longer stretch.
    seg = slice(rate * 8, min(n, rate * 30))
    lag = max(range(-1000, 1001), key=lambda L: float(np.dot(a[seg], np.roll(b, L)[seg])))
    b = np.roll(b, lag)
    print(f"  recordings lined up ({(shift - lag) / rate * 1000:+.2f} ms), {n / rate:.1f} s in common")
    try:
        from scipy.signal import butter, sosfiltfilt
        for lo, hi, name in ((20, 200, "bass (kick, bassline)"), (200, 2000, "mids")):
            sos = butter(4, [lo, hi], btype="band", fs=rate, output="sos")
            fa, fb = sosfiltfilt(sos, a[seg]), sosfiltfilt(sos, b[seg])
            print(f"  {name:22}: the two mixes differ by {20 * np.log10(np.std(fa - fb) / np.std(fa)):6.1f} dB "
                  f"(-25 dB or lower: the same)")
    except ImportError:
        pass

    # How alike are they, overall and around each move?
    hop_ms = 1.0
    ea, hop = envelope(a, rate, hop_ms)
    eb, _ = envelope(b, rate, hop_ms)
    diff, _ = envelope(a - b, rate, hop_ms)
    playing = ea > 0.01

    # The moves, from the host's log of when it recorded (the autopilot logs
    # each move; the recording started at the 4 s move).
    moves = []
    for path, who in ((host_log, "host"), (guest_log, "guest")):
        start = None
        for line in open(path, errors="replace"):
            m = re.search(r"^(\d\d):(\d\d):(\d\d)\.(\d+).*B2B autopilot move: (.*)$", line)
            if not m:
                continue
            t = int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3]) + float("0." + m[4])
            what = m[5]
            if "toggle_recording = 1" in what:
                start = t if who == "host" else start
                continue
            if start is None and who == "host":
                continue
            moves.append((t, who, what))
    host_start = None
    for line in open(host_log, errors="replace"):
        if "toggle_recording = 1" in line and host_start is None:  # the first press starts it
            m = re.search(r"^(\d\d):(\d\d):(\d\d)\.(\d+)", line)
            host_start = int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3]) + float("0." + m[4])
    if host_start is None:
        return
    # Each move changes one deck's tone (deck 1: 1 kHz, deck 2: 3 kHz; the
    # tone test tracks): find when its level changed in each recording.
    from scipy.signal import butter, sosfiltfilt

    def band_env(x, f):
        sos = butter(4, [f * 0.8, f * 1.25], btype="band", fs=rate, output="sos")
        y = sosfiltfilt(sos, x)
        hop = rate // 2000  # 0.5 ms
        n = len(y) // hop
        return np.sqrt((y[: n * hop].reshape(n, hop) ** 2).mean(axis=1) + 1e-12)

    envs = {1000: (band_env(a, 1000), band_env(b, 1000)), 3000: (band_env(a, 3000), band_env(b, 3000))}
    hop_ms = 0.5

    def crossing(env, around, span=500):
        lo, hi = max(0, around - span), min(len(env), around + span)
        before = np.median(env[max(0, lo - 400): lo])
        after = np.median(env[hi: hi + 400])
        if before <= 0 or after <= 0 or abs(20 * np.log10(after / before)) < 3:
            return None
        mid = (before + after) / 2
        w = env[lo:hi]
        idx = np.where((w - mid) * np.sign(after - before) > 0)[0]
        return lo + idx[0] if len(idx) else None

    # The recording starts a little after its button press: find how much
    # from the first clear move (searching wide), then look close.
    calib = 0
    for t, who, what in sorted(moves):
        if "parameter" in what or "volume" in what:
            f = 1000 if "Channel1" in what else 3000
            around = int((t - host_start) * 1000 / hop_ms)
            i = crossing(envs[f][0], around, span=4000)
            if i is not None:
                calib = i - around
                print(f"  (the host's recording started {calib * hop_ms:.0f} ms after its button press)")
                break
    gaps = []
    for t, who, what in sorted(moves):
        if any(k in what for k in ("quantize", "sync_enabled", "play =", "toggle_recording")):
            continue
        if "Channel1" in what or "crossfader = 1" in what:
            f = 1000
        elif "Channel2" in what or "crossfader = -1" in what:
            f = 3000
        else:
            f = 1000 if "crossfader = 0" in what and who == "guest" else 3000
        ea, eb = envs[f]
        around = int((t - host_start) * 1000 / hop_ms) + calib
        ia, ib = crossing(ea, around, 400), crossing(eb, around, 400)
        if ia is None or ib is None:
            print(f"  {who:5} {what:52} (no clear change in the {f} Hz tone)")
            continue
        gap = (ib - ia) * hop_ms
        gaps.append(gap)
        print(f"  {who:5} {what:52} heard {gap:+6.1f} ms apart")
    if gaps:
        print(f"  moves heard apart: median {statistics.median([abs(g) for g in gaps]):.1f} ms, "
              f"worst {max(abs(g) for g in gaps):.1f} ms (measured to about 0.5 ms)")


def main():
    out = sys.argv[1]
    _, host = load(os.path.join(out, "leader.csv"))
    _, guest = load(os.path.join(out, "follower.csv"))
    print("1. Decks, as heard on both computers (host minus guest):")
    alignment(host, guest)
    print("2. Moves from the friend, applied vs when they were due (ms, + = late):")
    lateness(guest, "host  (on the guest)")
    lateness(host, "guest (on the host)")
    print("3. What each computer recorded:")
    sound(out, os.path.join(out, "leader", "mixxx.log"), os.path.join(out, "follower", "mixxx.log"))


if __name__ == "__main__":
    main()
