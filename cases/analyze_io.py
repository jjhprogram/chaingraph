#!/usr/bin/env python3
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
"""Score a `cases/run.sh iomain|iothread` run: does hitchtrace explain the hitches?

Ground truth is the game's own frame log (frames.jsonl): which frames loaded
an asset, and how long the game spent on it. A hitch record is a *load* frame
if it matches such a frame, and a *noise* frame otherwise (on the development
VM some frames run long with nothing going on).

Each question is answerable from one record alone.

For iomain (the main thread reads the asset itself):
  top       is the top-ranked stall group I/O?
  old rule  would the old rule -- the longest single blocked interval -- have
            picked I/O?
  amount    how much of the game's own load time does the I/O group account
            for, and in how many separate waits?
  ended by  is that group's chain an interrupt (the device completing)?

For iothread (the main thread waits for mg_stream, which reads):
  top       is the top-ranked group a wait ended by mg_stream?
  why       does the chain's first hop say mg_stream was blocked on I/O, for
            at least half of the wait?
  ended by  does the chain go on to an interrupt?
  headline  does the root's resolved split call at least half of the overrun
            an inherited stall?

A good tool answers yes on load frames and no on noise frames.

The records are ranked by each group's total time. Two other rankings are
replayed on the same records, to see what a different rule would put first:

  stall time   a wait on a task that was running throughout is the game
               waiting for its own work, so rank a task-ended group by its
               inherited time only
  over usual   rank by how much more of that cause the frame has than a
               usual frame (the median of the noise records), main variant
               only: a per-cause baseline cannot tell two futex waits apart

usage: analyze_io.py RUN_DIR
"""
import json
import statistics as st
import sys

ATTACH_NS = 1_500_000_000	# trace_start is taken before the BPF attach
DETACH_NS = 300_000_000
MATCH_NS = 500_000		# record's frame end vs the game's own


def load(path):
    with open(path) as f:
        return [json.loads(line) for line in f]


def pct(n, d):
    return f"{n}/{d} ({100 * n / d:.0f}%)" if d else "0/0"


def med(xs):
    xs = list(xs)
    return st.median(xs) if xs else 0.0


def is_irq(hop):
    return bool({"irq", "softirq", "irqexit"} & set(hop["flags"]))


def task_ended(stall):
    hops = stall["hops"]
    return bool(hops) and not ({"irq", "softirq", "irqexit", "idle", "preemptor"}
                               & set(hops[0]["flags"]))


def by_stall_time(rec):
    """The group a ranking by stall time would put first."""
    return max(rec["stalls"],
               key=lambda s: s["inherited_ns"] if task_ended(s) else s["ns"])


def by_excess(rec, usual):
    """The cause a ranking by excess over the usual frame would put first."""
    best, seen = None, set()
    for s in rec["stalls"]:		# largest first: it takes the baseline
        base = 0 if s["cause"] in seen else usual.get(s["cause"], 0)
        seen.add(s["cause"])
        score = max(0, s["ns"] - base)
        if best is None or score > best[0]:
            best = (score, s["cause"])
    return best[1] if best else None


def stream_group(rec):
    """(rank, group) of the wait that mg_stream ended, if the record has one."""
    for i, s in enumerate(rec["stalls"]):
        if s["hops"] and s["hops"][0]["comm"] == "mg_stream":
            return i, s
    return None, None


def old_rule(rec):
    """The cause the old single-stall rule would have reported."""
    blocked = [s for s in rec["stalls"] if s["cause"] not in ("runnable", "runqueue")]
    return max(blocked, key=lambda s: s["longest_ns"])["cause"] if blocked else None


def main():
    run = sys.argv[1].rstrip("/")
    lines = load(f"{run}/hitch.jsonl")
    hdr, summ = lines[0], lines[-1]
    recs = [r for r in lines if "frame_id" in r]
    frames = load(f"{run}/frames.jsonl")
    manifest = json.load(open(f"{run}/manifest.json"))
    threaded = manifest["variant"] == "iothread"
    budget_us = hdr["budget_ns"] / 1e3

    lo = hdr["trace_start_ns"] + ATTACH_NS
    hi = summ["trace_end_ns"] - DETACH_NS
    win = [f for f in frames
           if f["t_end_ns"] - f["frame_us"] * 1000 > lo and f["t_end_ns"] < hi]
    loads = [f for f in win if f["asset"]]
    quiet = [f for f in win if not f["asset"]]
    print(f"run {run} ({manifest['variant']}): {len(win)} frames traced, "
          f"{len(loads)} of them loaded a {manifest['asset']['mb']} MB asset")
    print("\nThe game's own profiler (frames.jsonl), median ms per phase:")
    print(f"  {'':16}{'frames':>7}{'over budget':>14}{'limiter':>9}{'asset':>7}"
          f"{'jobs':>7}{'upload':>8}")
    for name, fs in (("other frames", quiet), ("load frames", loads)):
        over = sum(f["frame_us"] > budget_us for f in fs)
        print(f"  {name:16}{len(fs):>7}{pct(over, len(fs)):>14}"
              f"{med(f['limiter_us'] for f in fs) / 1e3:>9.1f}"
              f"{med(f['asset_us'] for f in fs) / 1e3:>7.1f}"
              f"{med(f['jobs_us'] for f in fs) / 1e3:>7.1f}"
              f"{med(f['upload_us'] for f in fs) / 1e3:>8.1f}")

    # ---- match records to the game's frames by the present that closed them
    by_end = sorted(win, key=lambda f: f["t_end_ns"])
    groups = {"load": [], "noise": []}
    unmatched = 0
    for r in recs:
        if not lo < r["frame_start_ns"] or not r["frame_end_ns"] < hi:
            continue
        f = min(by_end, key=lambda f: abs(f["t_end_ns"] - r["frame_end_ns"]),
                default=None)
        if f is None or abs(f["t_end_ns"] - r["frame_end_ns"]) > MATCH_NS:
            unmatched += 1
            continue
        groups["load" if f["asset"] else "noise"].append((r, f))
    if unmatched:
        print(f"\n({unmatched} records matched no frame of the game's log)")

    usual = {}
    if groups["noise"]:
        usual = {k: st.median(r["cause_ns"][k] for r, _ in groups["noise"])
                 for k in groups["noise"][0][0]["cause_ns"]}

    print("\nhitchtrace records (over-budget frames), by ground truth:")
    for key, pairs in groups.items():
        n = len(pairs)
        if not n:
            print(f"  {key}: none")
            continue
        top = old = ended = why = headline = hist = 0
        alt_stall = alt_excess = anywhere = 0
        share, waits, hop_share, stalls_n, ranks = [], [], [], [], []
        for r, f in pairs:
            excess = r["frame_ns"] - r["budget_ns"]
            asset_ns = f["asset_us"] * 1000
            s0 = r["stalls"][0] if r["stalls"] else None
            stalls_n.append(r["nstalls"])
            if not threaded:
                io = [s for s in r["stalls"] if s["cause"] == "block_io"]
                if s0 and s0["cause"] == "block_io":
                    top += 1
                if old_rule(r) == "block_io":
                    old += 1
                if r["stalls"] and by_stall_time(r)["cause"] == "block_io":
                    alt_stall += 1
                if by_excess(r, usual) == "block_io":
                    alt_excess += 1
                if io:
                    waits.append(io[0]["count"])
                    if asset_ns:
                        share.append(io[0]["ns"] / asset_ns)
                    if io[0]["hops"] and is_irq(io[0]["hops"][0]):
                        ended += 1
            else:
                hops = s0["hops"] if s0 else []
                h0 = hops[0] if hops else None
                by_stream = bool(h0 and h0["comm"] == "mg_stream")
                if s0 and s0["cause"] == "block_futex" and by_stream:
                    top += 1
                if old_rule(r) == "block_futex":
                    old += 1
                rank, sg = stream_group(r)
                if sg:
                    ranks.append(rank + 1)
                    g0 = sg["hops"][0]
                    if g0["cause"] == "block_io" and g0["ns"] >= 0.5 * sg["longest_ns"] \
                       and len(sg["hops"]) > 1 and is_irq(sg["hops"][1]):
                        anywhere += 1
                if r["stalls"]:
                    t = by_stall_time(r)
                    if t["hops"] and t["hops"][0]["comm"] == "mg_stream":
                        alt_stall += 1
                if by_stream and h0["cause"] == "block_io":
                    if "hist" in h0["flags"]:
                        hist += 1
                    if asset_ns:
                        hop_share.append(h0["ns"] / asset_ns)
                    if h0["ns"] >= 0.5 * s0["longest_ns"]:
                        why += 1
                    if len(hops) > 1 and is_irq(hops[1]):
                        ended += 1
                if r["cause_ns"]["resolved_inherited"] >= 0.5 * excess:
                    headline += 1

        print(f"  {key} frames: {n} records, overrun median "
              f"{med((r['frame_ns'] - r['budget_ns']) / 1e6 for r, _ in pairs):.1f} ms, "
              f"{med(stalls_n):.0f} off-CPU intervals per frame")
        if not threaded:
            print(f"    top       the top-ranked stall group is I/O:            {pct(top, n)}")
            print(f"    old rule  the longest single wait is I/O:               {pct(old, n)}")
            if share:
                print(f"    amount    I/O group / the game's own load time:         "
                      f"median {100 * med(share):.0f}%, in a median {med(waits):.0f} waits")
            print(f"    ended by  the I/O group's chain is an interrupt:        {pct(ended, n)}")
            print(f"    replayed  top is I/O when ranked by stall time:         {pct(alt_stall, n)}")
            print(f"    replayed  top is I/O when ranked over the usual frame:  {pct(alt_excess, n)}")
        else:
            print(f"    top       the top-ranked group is a wait ended by mg_stream: {pct(top, n)}")
            print(f"    why       first hop: mg_stream blocked on I/O, >= half:      {pct(why, n)}"
                  + (f"  (hop / the game's own load time: median {100 * med(hop_share):.0f}%)"
                     if hop_share else ""))
            print(f"    ended by  second hop is an interrupt:                        {pct(ended, n)}")
            print(f"    headline  root's inherited stall >= half the overrun:        {pct(headline, n)}")
            print(f"    anywhere  some group: mg_stream on I/O, then an interrupt:   {pct(anywhere, n)}"
                  + (f"  (ranked 1st/2nd/3rd: {ranks.count(1)}/{ranks.count(2)}/{ranks.count(3)})"
                     if ranks else ""))
            print(f"    replayed  mg_stream's wait on top, ranked by stall time:     {pct(alt_stall, n)}")
            if hist:
                print(f"    ({hist} first hops were flagged as a lower bound: history too short)")


if __name__ == "__main__":
    main()
