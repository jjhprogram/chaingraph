#!/usr/bin/env python3
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
"""Old ranking rule against the new one, on the same hitchbench records.

hitchtrace used to explain one stall per frame: the root's longest single
blocked interval. It now groups the root's off-CPU intervals by cause and by
who ended them, and ranks the groups by total time. A record carries enough
to replay the old rule (each group's longest interval; preemption and
run-queue time never qualified), so both can be scored on identical frames:
how often does the top-ranked stall have the cause the injector produced?

usage: rank_rules.py DIR     # DIR holds <class>.gt.jsonl / <class>.hitch.jsonl,
                             # as tests/run_hitch_tests.sh writes to tests/out
"""
import json
import statistics as st
import sys

# the off-CPU classes; cpu_spike and fault stall on-CPU and have no such group
EXPECT = {
    "sleep": "block_timer", "poll": "block_poll", "io": "block_io",
    "preempt": "runnable", "worker_block": "block_futex",
    "worker_cpu": "block_futex", "thousand_cuts": "block_timer",
    "present_long": "block_present", "present_off": "block_timer",
}


def main():
    d = sys.argv[1].rstrip("/")
    tot_old = tot_new = tot = 0
    print(f"{'class':15}{'injected':>9}{'old rule':>10}{'new rule':>10}")
    for cls, expect in EXPECT.items():
        with open(f"{d}/{cls}.gt.jsonl") as f:
            gt = {g["frame_id"]: g for g in map(json.loads, f)}
        with open(f"{d}/{cls}.hitch.jsonl") as f:
            recs = [r for r in map(json.loads, f) if "frame_id" in r
                    and gt.get(r["frame_id"], {}).get("injected")]
        old = new = 0
        for r in recs:
            blocked = [s for s in r["stalls"]
                       if s["cause"] not in ("runnable", "runqueue")]
            if blocked and max(blocked, key=lambda s: s["longest_ns"])["cause"] == expect:
                old += 1
            if r["stalls"] and r["stalls"][0]["cause"] == expect:
                new += 1
        note = ""
        if cls == "thousand_cuts" and recs:
            note = "   top group: median %d waits" % st.median(
                r["stalls"][0]["count"] for r in recs)
        print(f"{cls:15}{len(recs):>9}{old:>10}{new:>10}{note}")
        tot_old += old
        tot_new += new
        tot += len(recs)
    print(f"{'total':15}{tot:>9}{tot_old:>10}{tot_new:>10}")


if __name__ == "__main__":
    main()
