#!/usr/bin/env python3
"""Compare a leader and a follower B2B log from two Mixxx instances.

Usage:
    python3 tools/b2b/analyse_mixxx.py leader.csv follower.csv [--different-machines]

Reports:
  1. Deck position alignment: at every moment, is the follower's deck at the
     same point in the track as the leader's deck was `delay` earlier?
     (Same computer only: uses the computer's clock as ground truth.)
  2. Control timing: when the leader moved a fader/EQ/etc., how close to
     exactly `delay` later did the follower apply it?
  3. Position corrections the follower had to make.
  4. Clock sync accuracy.
"""

import argparse
import bisect
import csv
import statistics
import sys
from collections import defaultdict


def load(path):
    header, lines = {}, []
    with open(path) as f:
        for line in f:
            if line.startswith("# "):
                k, _, v = line[2:].strip().partition("=")
                header[k] = v
            else:
                lines.append(line)
    rows = []
    for r in csv.DictReader(lines):
        rows.append({
            "kind": r["kind"], "seq": int(r["seq"]), "beat": float(r["beat"]),
            "channel": int(r["channel"]), "value": float(r["value"]),
            "t": int(r["true_ns"]), "extra": float(r["extra"]),
        })
    return header, rows


def stats(values):
    if not values:
        return "no data"
    a = sorted(abs(v) for v in values)
    p = lambda q: a[min(len(a) - 1, int(len(a) * q))]
    return (f"n={len(values):<6} mean {statistics.fmean(values):+.3f} | |err| median {p(0.5):.3f}, "
            f"95% {p(0.95):.3f}, 99% {p(0.99):.3f}, worst {a[-1]:.3f} (ms)")


def local_clock(h, t):
    off = int(h.get("fake_offset_ns", 0))
    drift = float(h.get("fake_drift_ppm", 0)) * 1e-6
    origin = int(h.get("origin_ns", 0))
    return t + off + (t - origin) * drift


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("leader")
    ap.add_argument("follower")
    ap.add_argument("--different-machines", action="store_true")
    args = ap.parse_args()

    lh, lrows = load(args.leader)
    fh, frows = load(args.follower)
    delay = int(fh.get("follower_delay_ns", lh.get("follower_delay_ns", 150_000_000)))

    print("=" * 76)
    print("B2B Mixxx sync report")
    print("=" * 76)
    print(f"Follower delay {delay / 1e6:.0f} ms; fake network: {fh.get('sim_delay_ms', '0')} ms delay, "
          f"{fh.get('sim_jitter_ms', '0')} ms jitter, {fh.get('sim_loss_pct', '0')}% loss")

    # ---- 1. Position alignment ---------------------------------------------
    if not args.different_machines:
        lead_pos = defaultdict(list)  # deck -> [(t, pos, playing, speed)]
        for r in lrows:
            if r["kind"] == "P":
                lead_pos[r["seq"]].append((r["t"], r["beat"], r["value"] > 0.5, r["extra"]))
        for d in lead_pos:
            lead_pos[d].sort()
        times = {d: [x[0] for x in v] for d, v in lead_pos.items()}

        per_deck = defaultdict(list)
        timeline = defaultdict(list)
        f_start = None
        for r in frows:
            if r["kind"] != "P" or r["value"] < 0.5:
                continue
            d = r["seq"]
            if d not in lead_pos:
                continue
            target = r["t"] - delay
            i = bisect.bisect_right(times[d], target)
            if i == 0 or i >= len(times[d]):
                continue
            t0, p0, pl0, s0 = lead_pos[d][i - 1]
            t1, p1, pl1, s1 = lead_pos[d][i]
            if not (pl0 and pl1) or t1 - t0 > 200_000_000:
                continue
            dt = (t1 - t0) / 1e9
            if abs((p1 - p0) - s0 * dt) > 0.005:  # a seek/jump between the two samples
                continue
            lead_at = p0 + (p1 - p0) * (target - t0) / (t1 - t0)
            err_ms = (r["beat"] - lead_at) * 1000.0
            per_deck[d].append(err_ms)
            if f_start is None:
                f_start = r["t"]
            timeline[int((r["t"] - f_start) / 10e9)].append(err_ms)

        print()
        print("1. Deck position alignment (follower vs leader `delay` earlier)")
        all_err = [e for v in per_deck.values() for e in v]
        for d in sorted(per_deck):
            print(f"   Deck {d + 1}: {stats(per_deck[d])}")
        print(f"   All:    {stats(all_err)}")
        if timeline:
            print("   Over time (10 s windows, worst |error| ms):",
                  " ".join(f"{max(abs(e) for e in timeline[k]):.1f}" for k in sorted(timeline)))

    # ---- 1b. Position error as seen by the follower (valid on any machines) --
    derr = defaultdict(list)
    for r in frows:
        if r["kind"] == "D" and r["value"] > 0.5:
            derr[r["seq"]].append(r["extra"])
    print()
    print("1b. Follower vs leader deck position, judged against each leader snapshot")
    print("    (works across two computers; what the follower's drift correction sees)")
    for d in sorted(derr):
        print(f"   Deck {d + 1}: {stats(derr[d])}")

    # ---- 2. Control timing ---------------------------------------------------
    lead_ev = {r["seq"]: r for r in lrows if r["kind"] == "E"}
    fol_ev = {r["seq"]: r for r in frows if r["kind"] == "E"}
    matched_all = [s for s in lead_ev if s in fol_ev]
    # The burst of current values a follower receives when it first connects
    # is "late" by design; report it separately.
    f_first = min((fol_ev[s]["t"] for s in matched_all), default=0)
    initial = [s for s in matched_all
               if fol_ev[s]["extra"] > 500 and fol_ev[s]["t"] - f_first < 5e9]
    matched = [s for s in matched_all if s not in set(initial)]
    print()
    print("2. Control changes (faders, EQs, play, seeks...)")
    print(f"   Sent by leader: {len(lead_ev)}, applied by follower: {len(fol_ev)}, "
          f"matched: {len(matched_all)} ({len(initial)} of them the initial state on connect)")
    sched = [fol_ev[s]["extra"] for s in matched]
    print(f"   Applied vs intended moment (callback rounding): {stats(sched)}")
    if not args.different_machines:
        wall = [(fol_ev[s]["t"] - lead_ev[s]["t"] - delay) / 1e6 for s in matched]
        print(f"   Wall clock, follower vs leader+delay:          {stats(wall)}")
    late = [x for x in sched if x > 0 and x > 50]
    if late:
        print(f"   Arrived too late for their slot: {len(late)} (worst {max(late):.1f} ms)")

    # ---- 3. Corrections --------------------------------------------------------
    corr = [r for r in frows if r["kind"] == "K"]
    print()
    print(f"3. Follower position corrections: {len(corr)}")
    if corr:
        print(f"   Size before correcting: {stats([r['extra'] for r in corr])}")

    outside = [r for r in frows if r["kind"] == "L"]
    if outside:
        print(f"   Play/pause changes on the follower that did NOT come from B2B: {len(outside)}")
        base = min(r["t"] for r in frows)
        for r in outside[:10]:
            print(f"     at {(r['t'] - base) / 1e9:7.3f} s  deck {r['seq'] + 1}  play -> {r['value']:.0f}"
                  f"  (track position {r['beat']:.3f} s)")
    over = [r for r in frows if r["kind"] == "O"]
    if over:
        print(f"   Local changes on the follower that were undone: {len(over)}")

    # ---- 4. Clock sync -----------------------------------------------------------
    if not args.different_machines:
        clock_err = [(r["extra"] - (local_clock(lh, r["t"]) - local_clock(fh, r["t"]))) / 1e6
                     for r in frows if r["kind"] == "S"]
        print()
        print(f"4. Clock offset estimate error: {stats(clock_err)}")
    print("-" * 76)
    return 0


if __name__ == "__main__":
    sys.exit(main())
