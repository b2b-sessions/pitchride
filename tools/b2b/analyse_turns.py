#!/usr/bin/env python3
"""Check two Mixxx B2B logs from one computer across handovers.

Usage:
    python3 tools/b2b/analyse_turns.py host.csv guest.csv

For every moment one side is listening, its deck should be at the point in
the track where the live side's deck was (listener offset - live offset)
earlier. Both logs use the computer's real clock, so this is ground truth.
Reports the error per period, split into "catching up" and "settled", plus
the handovers and how late the listener applied the live side's moves.
"""

import bisect
import csv
import statistics
import sys
from collections import defaultdict


def load(path):
    rows = []
    with open(path) as f:
        lines = [l for l in f if not l.startswith("# ")]
    for r in csv.DictReader(lines):
        rows.append(r)
    return rows


def pct(values, q):
    if not values:
        return float("nan")
    s = sorted(values)
    return s[min(len(s) - 1, int(q * len(s)))]


def summary(values):
    if not values:
        return "n=0"
    a = [abs(v) for v in values]
    return (f"n={len(a):<6} |err| median {statistics.median(a):.2f}, 95% {pct(a, .95):.2f}, "
            f"99% {pct(a, .99):.2f}, worst {max(a):.2f} ms")


def parse(rows):
    roles = []      # (t, live, offset_ms, catchup)
    pos = defaultdict(list)  # deck -> [(t, pos, playing)]
    hand = []
    late = []       # (t, ms late) for applied events
    corr = []
    for r in rows:
        k, t = r["kind"], int(r["true_ns"])
        if k == "F":
            roles.append((t, int(r["seq"]) == 1, float(r["beat"]), float(r["value"])))
        elif k == "P":
            pos[int(r["channel"])].append((t, float(r["beat"]), float(r["value"]) > 0.5))
        elif k == "H":
            hand.append((t, int(r["seq"]), float(r["beat"]), float(r["value"])))
        elif k == "E" and float(r["extra"]) != 0.0:
            late.append((t, float(r["extra"])))
        elif k == "K":
            corr.append((t, int(r["channel"]), float(r["extra"])))
    return roles, pos, hand, late, corr


def role_at(roles, times, t):
    i = bisect.bisect_right(times, t) - 1
    return roles[i] if i >= 0 else None


def interp(track, times, t):
    i = bisect.bisect_right(times, t)
    if i == 0 or i >= len(track):
        return None
    (t0, p0, pl0), (t1, p1, pl1) = track[i - 1], track[i]
    if t1 - t0 > 200e6 or not (pl0 and pl1):
        return None
    if p1 < p0 or (p1 - p0) > 3.0 * (t1 - t0) / 1e9:
        return None  # the live deck jumped in between (loop, cue, seek): can't interpolate
    return p0 + (p1 - p0) * (t - t0) / (t1 - t0)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    sides = {"host": parse(load(sys.argv[1])), "guest": parse(load(sys.argv[2]))}
    t_start = min(s[0][0][0] for s in sides.values() if s[0])

    print("=" * 76)
    print("B2B taking turns report")
    print("=" * 76)
    print("\nHandovers (seconds since start):")
    events = []
    for name, (_, _, hand, _, _) in sides.items():
        for t, live, shift, dur in hand:
            if live:
                events.append((t, f"  {(t - t_start) / 1e9:7.2f}  {name} goes live"))
            else:
                how = f"easing {shift:.0f} ms behind over {dur:.1f} s" if dur > 0 else f"moved {shift:.0f} ms behind at once"
                events.append((t, f"  {(t - t_start) / 1e9:7.2f}  {name} hands over, {how}"))
    for _, line in sorted(events):
        print(line)

    for name, other in (("host", "guest"), ("guest", "host")):
        roles, pos, _, late, corr = sides[name]
        oroles, opos, _, _, _ = sides[other]
        rtimes = [r[0] for r in roles]
        ortimes = [r[0] for r in oroles]
        settled, catching = defaultdict(list), defaultdict(list)
        for deck, track in pos.items():
            otrack = opos.get(deck, [])
            otimes = [x[0] for x in otrack]
            for t, p, playing in track:
                me = role_at(roles, rtimes, t)
                if not me or me[1] or not playing:
                    continue  # only judge while listening and playing
                # Find the live side's offset (it is constant while live).
                them = role_at(oroles, ortimes, t)
                if not them or not them[1]:
                    continue  # handover in flight
                lag_ns = (me[2] - them[2]) * 1e6
                expected = interp(otrack, otimes, t - lag_ns)
                if expected is None:
                    continue
                err = (p - expected) * 1000.0
                if abs(err) > 1000:
                    continue  # different track loaded (a load in progress)
                (catching if me[3] >= 0 else settled)[deck].append(err)
        print(f"\n{name.capitalize()} listening: deck position vs the live side (ground truth)")
        for deck in sorted(set(settled) | set(catching)):
            print(f"   Deck {deck + 1} settled:     {summary(settled[deck])}")
            if catching[deck]:
                print(f"   Deck {deck + 1} catching up: {summary(catching[deck])}")
        lates = [(ms, r[3] >= 0) for t, ms in late if (r := role_at(roles, rtimes, t)) and not r[1]]
        big_settled = [ms for ms, c in lates if ms > 5 and not c]
        big_catching = [ms for ms, c in lates if ms > 5 and c]
        print(f"   Moves applied: {len(lates)}; more than 5 ms late: {len(big_settled)} settled"
              + (f" (worst {max(big_settled):.0f} ms)" if big_settled else "")
              + f", {len(big_catching)} while catching up"
              + (f" (worst {max(big_catching):.0f} ms)" if big_catching else ""))
        print(f"   Position corrections: {len(corr)}")
    print("-" * 76)


if __name__ == "__main__":
    main()
