#!/usr/bin/env python3
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
"""hitchview: turn a hitchtrace JSONL file into one self-contained HTML page.

For every over-budget frame the page shows the frame's timeline (one lane per
thread of the process, every off-CPU interval at its real time, with who woke
or preempted it), where the frame's thread spent its time, what it was kept
waiting for (each stall group with its wakeup chain), and the other threads'
totals. The timeline needs records made with hitchtrace's timeline on, which
is the default; without it the page shows the totals and chains only.

    hitchtrace -p PID -o run.jsonl ...
    tools/hitchview.py run.jsonl                 # writes run.html next to it
    tools/hitchview.py run.jsonl -o view.html --title "bgcpu, 60 s"

The page needs no network and no other file.
"""
import argparse
import json
import os
import sys

TEMPLATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hitchview.html")
PLACEHOLDER = "/*DATA*/null"


def trim_hop(h):
    return {
        "pid": h.get("pid", 0), "comm": h.get("comm") or "",
        "ns": h.get("ns", 0), "sched_ns": h.get("sched_ns", 0),
        "cause": h.get("cause", "block_other"), "flags": h.get("flags", []),
        "blocked_in": h.get("blocked_in"), "preemptor": h.get("preemptor"),
    }


def trim_stall(s):
    return {
        "ns": s.get("ns", 0), "count": s.get("count", 1),
        "longest_ns": s.get("longest_ns", s.get("ns", 0)),
        "inherited_ns": s.get("inherited_ns", 0),
        "start_ns": s.get("start_ns", 0),
        "cause": s.get("cause", "block_other"), "other": bool(s.get("other")),
        "blocked_in": s.get("blocked_in"),
        "kstack": s.get("kstack") or [], "ustack": s.get("ustack") or [],
        "hops": [trim_hop(h) for h in s.get("hops", [])],
    }


def trim_thread(t):
    largest = t.get("largest") or {}
    return {
        "tid": t.get("tid", 0), "comm": t.get("comm") or "",
        "root": bool(t.get("root")), "cause_ns": t.get("cause_ns", {}),
        "open_ns": t.get("open_ns", 0),
        "largest": {"ns": largest.get("ns", 0),
                    "blocked_in": largest.get("blocked_in")},
        "preemptor": t.get("preemptor"),
    }


def trim_event(e, t0):
    """One logged interval, times relative to the frame's start."""
    by = e.get("by")
    return {
        "t": e["tid"], "o": e["out_ns"] - t0, "w": e["wake_ns"] - t0,
        "i": e["in_ns"] - t0, "c": e.get("cause", "block_other"),
        "f": e.get("flags", []), "k": e.get("blocked_in"),
        "b": None if not by else {"t": by.get("tid", 0), "g": by.get("tgid", 0),
                                  "c": by.get("comm") or "",
                                  "s": by.get("stall_ns", 0)},
    }


def trim_record(r):
    stalls = r.get("stalls")
    if stalls is None:
        # records from before stall groups: the single worst stall, if any
        worst = r.get("worst") or {}
        stalls = [worst] if worst.get("ns") else []
    return {
        "frame_id": r["frame_id"], "frame_ns": r["frame_ns"],
        "budget_ns": r["budget_ns"], "frame_start_ns": r["frame_start_ns"],
        "frame_end_ns": r["frame_end_ns"], "root_comm": r.get("root_comm") or "",
        "root_pid": r.get("root_pid", 0), "cause_ns": r["cause_ns"],
        "stalls": [trim_stall(s) for s in stalls],
        "threads": [trim_thread(t) for t in r.get("threads", [])],
        "events": ([trim_event(e, r["frame_start_ns"]) for e in r["events"]]
                   if "events" in r else None),
        "events_dropped": r.get("events_dropped", 0),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("jsonl", help="hitchtrace -o output")
    ap.add_argument("-o", "--output", help="HTML file to write (default: next to the input)")
    ap.add_argument("--title", help="page title (default: the input's name)")
    ap.add_argument("--frame", type=int, help="frame id to open on (default: the longest)")
    args = ap.parse_args()

    header, summary, records = {}, {}, []
    with open(args.jsonl) as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except ValueError as e:
                sys.exit(f"{args.jsonl}:{n}: not JSON: {e}")
            if "frame_id" in obj:
                records.append(trim_record(obj))
            elif obj.get("type") == "header":
                header = obj
            elif obj.get("type") == "summary":
                summary = obj

    records.sort(key=lambda r: r["frame_end_ns"])
    t0 = header.get("trace_start_ns") or (records[0]["frame_start_ns"] if records else 0)
    t1 = summary.get("trace_end_ns") or (records[-1]["frame_end_ns"] if records else t0)
    start = None
    if args.frame is not None:
        hits = [i for i, r in enumerate(records) if r["frame_id"] == args.frame]
        if not hits:
            sys.exit(f"no record for frame {args.frame}")
        start = hits[0]
    data = {
        "meta": {
            "title": args.title or os.path.basename(os.path.dirname(os.path.abspath(args.jsonl)))
                     + "/" + os.path.basename(args.jsonl),
            "pid": header.get("pid", records[0]["root_pid"] if records else 0),
            "budget_ns": header.get("budget_ns", records[0]["budget_ns"] if records else 0),
            "frames": summary.get("frames"),
            "t0_ns": t0, "t1_ns": t1, "start": start,
        },
        "records": records,
    }

    with open(TEMPLATE) as f:
        page = f.read()
    if PLACEHOLDER not in page:
        sys.exit(f"{TEMPLATE}: data placeholder not found")
    # "</" would end the script element early; "\u2028/9" end a JS line
    blob = (json.dumps(data, separators=(",", ":"))
            .replace("</", "<\\/").replace("\u2028", "\\u2028").replace("\u2029", "\\u2029"))
    out = args.output or os.path.splitext(args.jsonl)[0] + ".html"
    with open(out, "w") as f:
        f.write(page.replace(PLACEHOLDER, blob))
    print(f"{out}: {len(records)} hitch frames, {os.path.getsize(out) // 1024} KB")


if __name__ == "__main__":
    main()
