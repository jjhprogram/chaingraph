#!/usr/bin/env python3
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
"""Score a `cases/run.sh bgcpu` run: does hitchtrace explain the hitches?

Ground truth is bgload's own burst log (bursts.jsonl), not anything hitchtrace
says. A hitch record is a *burst* frame if its window overlaps a burst by at
least MIN_OVERLAP_NS, and a *noise* frame otherwise (on the development VM
some frames run long with nothing injected).

For every hitch record the script asks four questions, each answerable from
the record alone:

  culprit   does any thread line of the game name bgload as its preemptor?
  thread    did some worker spend at least half of the frame's overrun
            runnable-but-not-running (runnable + runqueue on its own line)?
  chain     does the worst stall's first hop say "preempted" AND explain at
            least half of the overrun? Does it name bgload as the preemptor?
  headline  does the root's resolved split call at least half of the overrun
            an inherited stall (rather than "waiting on a running task")?

A good tool answers yes on burst frames and no on noise frames.

usage: analyze_bgcpu.py RUN_DIR
"""
import json
import statistics as st
import sys
from collections import Counter

MIN_OVERLAP_NS = 1_000_000
ATTACH_NS = 1_500_000_000	# trace_start is taken before the BPF attach
DETACH_NS = 300_000_000


def load(path):
    with open(path) as f:
        return [json.loads(line) for line in f]


def pct(n, d):
    return f"{n}/{d} ({100 * n / d:.0f}%)" if d else "0/0"


def med(xs):
    xs = list(xs)
    return st.median(xs) if xs else 0.0


def main():
    run = sys.argv[1].rstrip("/")
    lines = load(f"{run}/hitch.jsonl")
    hdr, summ = lines[0], lines[-1]
    recs = [r for r in lines if "frame_id" in r]
    bursts = load(f"{run}/bursts.jsonl")
    frames = load(f"{run}/frames.jsonl")
    budget_us = hdr["budget_ns"] / 1e3

    def overlap(s, e):
        return sum(max(0, min(e, b["end_ns"]) - max(s, b["start_ns"]))
                   for b in bursts)

    # ---- the game's own view (its frame log), inside the traced window
    lo = hdr["trace_start_ns"] + ATTACH_NS
    hi = summ["trace_end_ns"] - DETACH_NS
    win = [f for f in frames
           if f["t_end_ns"] - f["frame_us"] * 1000 > lo and f["t_end_ns"] < hi]
    fburst = [f for f in win
              if overlap(f["t_end_ns"] - f["frame_us"] * 1000, f["t_end_ns"])
              >= MIN_OVERLAP_NS]
    fquiet = [f for f in win if f not in fburst]
    print(f"run {run}: {len(win)} frames traced, {len(bursts)} bgload bursts "
          f"(wall time median {med((b['end_ns'] - b['start_ns']) / 1e6 for b in bursts):.1f} ms)")
    print("\nThe game's own profiler (frames.jsonl), median ms per phase:")
    print(f"  {'':22}{'frames':>7}{'over budget':>14}{'limiter':>9}{'jobs':>7}"
          f"{'upload':>8}{'present':>9}")
    for name, fs in (("quiet frames", fquiet), ("frames during a burst", fburst)):
        over = sum(f["frame_us"] > budget_us for f in fs)
        print(f"  {name:22}{len(fs):>7}{pct(over, len(fs)):>14}"
              f"{med(f['limiter_us'] for f in fs) / 1e3:>9.1f}"
              f"{med(f['jobs_us'] for f in fs) / 1e3:>7.1f}"
              f"{med(f['upload_us'] for f in fs) / 1e3:>8.1f}"
              f"{med(f['present_us'] for f in fs) / 1e3:>9.1f}")

    # ---- hitchtrace's view
    groups = {"burst": [], "noise": []}
    for r in recs:
        if not lo < r["frame_start_ns"] or not r["frame_end_ns"] < hi:
            continue
        key = ("burst" if overlap(r["frame_start_ns"], r["frame_end_ns"])
               >= MIN_OVERLAP_NS else "noise")
        groups[key].append(r)

    print("\nhitchtrace records (over-budget frames), by ground truth:")
    for key, rs in groups.items():
        n = len(rs)
        if not n:
            print(f"  {key}: none")
            continue
        culprit = thread = chain = headline = chain_says_preempted = 0
        chain_names = 0
        inh, won, wrun, hop_ns, excess_ms, named_ms = [], [], [], [], [], []
        credited = []	# chain's inherited time / the waker's real delay
        preemptors = Counter()
        for r in rs:
            c = r["cause_ns"]
            excess = r["frame_ns"] - r["budget_ns"]
            excess_ms.append(excess / 1e6)
            inh.append(c["resolved_inherited"] / 1e6)
            won.append(c["resolved_wait_oncpu"] / 1e6)
            workers = [t for t in r["threads"] if t["comm"].startswith("mg_worker")]
            runnable = [t["cause_ns"]["runnable"] + t["cause_ns"]["runqueue"]
                        for t in workers]
            wrun.append(max(runnable, default=0) / 1e6)
            named = [t["preemptor"]["ns"] for t in r["threads"]
                     if t.get("preemptor")
                     and t["preemptor"]["comm"].startswith("bgload")]
            for t in r["threads"]:
                if t.get("preemptor"):
                    who = t["preemptor"]["comm"].rstrip("0123456789")
                    preemptors[who] += t["preemptor"]["ns"] / 1e6
            if named:
                culprit += 1
                named_ms.append(max(named) / 1e6)
            if max(runnable, default=0) >= 0.5 * excess:
                thread += 1
            hops = r["worst"]["hops"]
            h0 = hops[0] if hops else None
            # records from before the link fix have no preemptor on a hop
            if h0 and (h0.get("preemptor") or {}).get("comm", "").startswith("bgload"):
                chain_names += 1
            if h0 and h0["cause"] == "runnable":
                chain_says_preempted += 1
                hop_ns.append(h0["ns"] / 1e6)
                waker = [t for t in workers if t["tid"] == h0["pid"]]
                if waker:
                    real = (waker[0]["cause_ns"]["runnable"]
                            + waker[0]["cause_ns"]["runqueue"])
                    if real:
                        credited.append(h0["ns"] / real)
                if h0["ns"] >= 0.5 * excess:
                    chain += 1
            if c["resolved_inherited"] >= 0.5 * excess:
                headline += 1

        print(f"  {key} frames: {n} records, overrun median {med(excess_ms):.1f} ms")
        print(f"    culprit   bgload named as a preemptor on a thread line:   {pct(culprit, n)}"
              + (f", longest named preemption median {med(named_ms):.1f} ms" if named_ms else ""))
        print(f"    thread    a worker runnable for >= half the overrun:       {pct(thread, n)}"
              f", most-delayed worker median {med(wrun):.1f} ms")
        print(f"    chain     first hop 'preempted' and >= half the overrun:   {pct(chain, n)}"
              f"  (says 'preempted' at all: {pct(chain_says_preempted, n)}, "
              f"explaining median {med(hop_ns):.1f} ms)")
        print(f"    chain     first hop names bgload as the preemptor:         {pct(chain_names, n)}")
        print(f"    headline  root's inherited stall >= half the overrun:      {pct(headline, n)}"
              f"  (median inherited {med(inh):.1f} ms vs 'waiting on a running task' {med(won):.1f} ms)")
        if credited:
            print(f"    of the waking worker's real runnable time, the chain credits a "
                  f"median {100 * med(credited):.0f}%")
        top = ", ".join(f"{k} {v:.0f} ms" for k, v in preemptors.most_common(4))
        print(f"    preemptors named, total over these records: {top}")


if __name__ == "__main__":
    main()
