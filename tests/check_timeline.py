#!/usr/bin/env python3
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
"""Check a hitch record's timeline against the same record's totals.

A record carries each thread's off-CPU time twice: summed into buckets on its
thread line, and (with the timeline on) as the individual intervals in
"events". The two are produced on different paths in the BPF program, so they
check each other: clipped to the frame window, a thread's intervals must add
up to its buckets.

  blocked   Σ [out, wake) of non-runnable intervals  ==  Σ block_* buckets
  sched     Σ [wake, in) + Σ runnable intervals       ==  runnable + runqueue

One difference is expected and allowed: a thread whose wait ends while the
frame is being closed. Its line was written a moment earlier and still says
"waiting at frame end" (open_ns, not in any bucket), while the timeline has
the finished interval. Then the events equal the buckets plus open_ns.

usage: check_timeline.py HITCH.jsonl [--tol-us N]
Exit status 1 if any thread of any record is off by more than the tolerance.
"""
import argparse
import json
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("jsonl")
    ap.add_argument("--tol-us", type=float, default=50.0)
    args = ap.parse_args()
    tol = args.tol_us * 1000

    nrec = nthreads = nevents = bad = dropped = without = late = 0
    worst = 0
    with open(args.jsonl) as f:
        for line in f:
            r = json.loads(line)
            if "frame_id" not in r:
                continue
            if "events" not in r:
                without += 1
                continue
            nrec += 1
            nevents += len(r["events"])
            if r.get("events_dropped"):
                dropped += 1
                continue	# incomplete by its own account
            lo, hi = r["frame_start_ns"], r["frame_end_ns"]
            clip = lambda a, b: max(0, min(b, hi) - max(a, lo))
            got = {}
            for e in r["events"]:
                g = got.setdefault(e["tid"], [0, 0])
                if e["cause"] == "runnable":
                    g[1] += clip(e["out_ns"], e["in_ns"])
                else:
                    g[0] += clip(e["out_ns"], e["wake_ns"])
                    g[1] += clip(e["wake_ns"], e["in_ns"])
            for t in r["threads"]:
                c = t["cause_ns"]
                want_b = sum(v for k, v in c.items() if k.startswith("block_"))
                want_s = c["runnable"] + c["runqueue"]
                have_b, have_s = got.get(t["tid"], [0, 0])
                err = max(abs(want_b - have_b), abs(want_s - have_s))
                if err > tol and t["open_ns"]:
                    closed = abs(want_b + want_s + t["open_ns"] - have_b - have_s)
                    if closed <= tol:
                        late += 1
                        err = closed
                worst = max(worst, err)
                nthreads += 1
                if err > tol:
                    bad += 1
                    if bad <= 5:
                        print(f"frame {r['frame_id']} {t['comm']}/{t['tid']}: "
                              f"blocked {want_b / 1e6:.3f} ms in buckets, "
                              f"{have_b / 1e6:.3f} in events; sched "
                              f"{want_s / 1e6:.3f} against {have_s / 1e6:.3f}")
    print(f"{nrec} records with a timeline ({without} without), {nevents} events; "
          f"{nthreads} thread lines checked, {bad} off by more than "
          f"{args.tol_us:.0f} us (worst {worst / 1e3:.1f} us; {late} waits ended "
          f"as the frame closed); "
          f"{dropped} records skipped for dropped events")
    return 1 if bad or not nrec else 0


if __name__ == "__main__":
    sys.exit(main())
