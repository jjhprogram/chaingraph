#!/usr/bin/env python3
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
"""Score a `cases/run.sh mem|memthp|slack|ipc|cpuspike` run.

Does a hitchtrace record alone explain the hitch? Each over-budget record is
set against ground truth that does not come from hitchtrace, and sorted into
*affected* frames (the case's cause was at work) and *noise* frames (it was
not; on the development VM some frames run long with nothing going on). A
good tool answers the case's questions yes on the first and no on the second.

  mem, memthp   affected: the frame walked the world (the game's log), under
                a memory limit. Does the record put the time on memory: major
                faults, and I/O waits whose kernel stack is a page fault?
  slack         affected: the limiter slept with its timer slack raised (the
                preload's log). Does the record show the sleep, and can it
                tell a late wakeup from an intended one?
  ipc           affected: bgload was bursting on the server's CPU (its log).
                Does the record show the waits on the server, that the server
                was kept off the CPU, and by whom?
  cpuspike      affected: the frame burned CPU on the main thread (the game's
                log). A negative control: does the record show the time as
                running, and blame no stall?

Rankings other than the record's own (by total) are replayed on the same
records: the old rule (longest single wait), and "stall time" (a wait on a
thread that was running does not count, and neither does a timer sleep).

usage: analyze_cases.py RUN_DIR
"""
import json
import statistics as st
import sys

ATTACH_NS = 1_500_000_000	# trace_start is taken before the BPF attach
DETACH_NS = 300_000_000
MATCH_NS = 500_000		# record's frame end vs the game's own
MIN_OVERLAP_NS = 1_000_000
FAULT_FRAMES = ("do_swap_page", "handle_mm_fault", "exc_page_fault",
                "do_user_addr_fault")


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


def stall_time(stall):
    if stall["cause"] == "block_timer":
        return 0
    return stall["inherited_ns"] if task_ended(stall) else stall["ns"]


def by_stall_time(rec):
    return max(rec["stalls"], key=stall_time) if rec["stalls"] else None


def old_rule(rec):
    """The group whose single longest wait the old rule would have reported."""
    blocked = [s for s in rec["stalls"] if s["cause"] not in ("runnable", "runqueue")]
    return max(blocked, key=lambda s: s["longest_ns"]) if blocked else None


def is_fault_io(stall):
    return (stall["cause"] == "block_io" and not stall["other"]
            and any(f in FAULT_FRAMES for f in stall["kstack"]))


def from_server(stall):
    return bool(stall["hops"]) and stall["hops"][0]["comm"] == "mg_server"


def main():
    run = sys.argv[1].rstrip("/")
    lines = load(f"{run}/hitch.jsonl")
    hdr, summ = lines[0], lines[-1]
    recs = [r for r in lines if "frame_id" in r]
    frames = load(f"{run}/frames.jsonl")
    manifest = json.load(open(f"{run}/manifest.json"))
    variant = manifest["variant"]
    budget_us = hdr["budget_ns"] / 1e3
    bursts = load(f"{run}/bursts.jsonl") if variant in ("slack", "ipc") else []

    def overlap(s, e):
        return sum(max(0, min(e, b["end_ns"]) - max(s, b["start_ns"])) for b in bursts)

    lo = hdr["trace_start_ns"] + ATTACH_NS
    hi = summ["trace_end_ns"] - DETACH_NS
    win = [f for f in frames
           if f["t_end_ns"] - f["frame_us"] * 1000 > lo and f["t_end_ns"] < hi]

    def affected(f):
        if variant in ("slack", "ipc"):
            return overlap(f["t_end_ns"] - f["frame_us"] * 1000,
                           f["t_end_ns"]) >= MIN_OVERLAP_NS
        return bool(f["step"])

    aff = [f for f in win if affected(f)]
    quiet = [f for f in win if not affected(f)]
    phase = {"mem": "touch_us", "memthp": "touch_us", "slack": "late_us",
             "ipc": "calls_us", "cpuspike": "burn_us"}[variant]
    print(f"run {run} ({variant}): {len(win)} frames traced")
    print("\nThe game's own log (frames.jsonl), median per frame:")
    for name, fs in (("other frames", quiet), ("affected frames", aff)):
        over = sum(f["frame_us"] > budget_us for f in fs)
        extra = ""
        if variant.startswith("mem"):
            extra = (f", {med(f['majflt'] for f in fs):.0f} major and "
                     f"{med(f['minflt'] for f in fs):.0f} minor faults")
        print(f"  {name:16}{len(fs):>6} frames, {pct(over, len(fs)):>13} over budget, "
              f"{phase[:-3]} {med(f[phase] for f in fs) / 1e3:.1f} ms{extra}")
    usual = med(f[phase] for f in quiet) * 1000		# ns, a usual frame's worth

    by_end = sorted(win, key=lambda f: f["t_end_ns"])
    groups = {"affected": [], "noise": []}
    for r in recs:
        if not lo < r["frame_start_ns"] or not r["frame_end_ns"] < hi:
            continue
        f = min(by_end, key=lambda f: abs(f["t_end_ns"] - r["frame_end_ns"]), default=None)
        if f is None or abs(f["t_end_ns"] - r["frame_end_ns"]) > MATCH_NS:
            continue
        groups["affected" if affected(f) else "noise"].append((r, f))

    usual_c = {}
    if groups["noise"]:
        usual_c = {k: st.median(r["cause_ns"][k] for r, _ in groups["noise"])
                   for k in groups["noise"][0][0]["cause_ns"]}

    print("\nhitchtrace records (over-budget frames), by ground truth:")
    for key, pairs in groups.items():
        n = len(pairs)
        if not n:
            print(f"  {key}: none")
            continue
        q = {}		# question -> count

        def yes(name, cond):
            q[name] = q.get(name, 0) + bool(cond)

        amounts, counts, extra_ms = [], [], []
        for r, f in pairs:
            c = r["cause_ns"]
            excess = r["frame_ns"] - r["budget_ns"]
            top, top_st, top_old = r["stalls"][0] if r["stalls"] else None, \
                by_stall_time(r), old_rule(r)
            game_ns = f[phase] * 1000

            if variant.startswith("mem"):
                fio = [s for s in r["stalls"] if is_fault_io(s)]
                fault_ns = (c["oncpu_fault"] + c["oncpu_fault_major"]
                            + c["oncpu_reclaim"] + c["oncpu_compact"])
                mem_ns = fault_ns + sum(s["ns"] for s in fio)
                yes("names memory: an I/O group whose stack is a page fault", fio)
                yes("memory (faults + that I/O) is >= half the overrun", mem_ns >= 0.5 * excess)
                yes("that I/O group is top-ranked, by total", top and is_fault_io(top))
                yes("... ranked by stall time", top_st and is_fault_io(top_st))
                yes("... under the old rule (longest single wait)", top_old and is_fault_io(top_old))
                if fio:
                    yes("its wait count is the game's major-fault count, +-2",
                        abs(fio[0]["count"] - f["majflt"]) <= 2)
                    counts.append(fio[0]["count"])
                if game_ns:
                    amounts.append(mem_ns / game_ns)
            elif variant == "slack":
                timer = [s for s in r["stalls"] if s["cause"] == "block_timer"]
                yes("the timer sleep is top-ranked, by total", top and top["cause"] == "block_timer")
                yes("the record has any field saying the sleep ended late", False)
                if timer:
                    amounts.append(timer[0]["ns"] / (f["limiter_us"] * 1000 or 1))
                    extra_ms.append(timer[0]["ns"] / 1e6)
            elif variant == "ipc":
                srv = [s for s in r["stalls"] if from_server(s)]
                h0 = srv[0]["hops"][0] if srv else None
                kept = bool(h0) and h0["cause"] == "runnable"
                named = kept and (h0.get("preemptor") or {}).get("comm", "").startswith("bgload")
                yes("a group of waits ended by mg_server", srv)
                yes("... saying the server was kept off the CPU", kept)
                yes("... and naming bgload as its preemptor", named)
                yes("that group is top-ranked, by total", top and from_server(top))
                yes("... ranked by stall time", top_st and from_server(top_st))
                yes("... under the old rule (longest single wait)", top_old and from_server(top_old))
                if srv:
                    counts.append(srv[0]["count"])
                    if game_ns > usual:
                        amounts.append(srv[0]["inherited_ns"] / (game_ns - usual))
            elif variant == "cpuspike":
                run_extra = c["oncpu"] - usual_c.get("oncpu", 0)
                st_top = stall_time(top_st) if top_st else 0
                yes("running is >= half the overrun", c["oncpu"] >= 0.5 * excess)
                yes("no stall worth a quarter of the overrun (stall-time rule)",
                    st_top < 0.25 * excess)
                yes("inherited stalls under a quarter of the overrun",
                    c["resolved_inherited"] < 0.25 * excess)
                if game_ns:
                    amounts.append(run_extra / game_ns)

        print(f"  {key} frames: {n} records, overrun median "
              f"{med((r['frame_ns'] - r['budget_ns']) / 1e6 for r, _ in pairs):.1f} ms, "
              f"{med(len(r.get('events') or []) for r, _ in pairs):.0f} timeline intervals")
        for name, v in q.items():
            print(f"    {name + ':':62}{pct(v, n)}")
        if amounts:
            what = {"mem": "memory time / the game's own walk time",
                    "memthp": "memory time / the game's own walk time",
                    "slack": "timer group / the game's own limiter time",
                    "ipc": "server group's inherited time / the game's extra call time",
                    "cpuspike": "extra running time / the game's own burn time"}[variant]
            print(f"    {what + ':':62}median {100 * med(amounts):.0f}%")
        if counts:
            print(f"    {'waits in that group:':62}median {med(counts):.0f}")
        if extra_ms:
            print(f"    {'timer group, median:':62}{med(extra_ms):.1f} ms")


if __name__ == "__main__":
    main()
