#!/usr/bin/env python3
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
"""Unit tests for check_hitches.py, using synthetic frames and records.

Run from the project root:  python3 -m unittest tests/test_check_hitches.py
"""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import check_hitches as ch  # noqa: E402

BUDGET_NS = 16_667_000
NORMAL_NS = 12_000_000
HITCH_NS = 24_000_000


# ---------------------------------------------------------------------------
# synthetic inputs, laid out as hitchbench and hitchtrace write them


def gt_frame(frame_id, injected=None, expect=None, frame_ns=None, root_tid=4242):
    """One ground-truth line; uninjected frames are the normal, quick ones."""
    if frame_ns is None:
        frame_ns = HITCH_NS if injected else NORMAL_NS
    return {"frame_id": frame_id, "injected": injected, "expect": expect,
            "frame_us": frame_ns // 1000, "t_end_ns": 1_000_000_000 + frame_id,
            "root_tid": root_tid}


def causes(**kwargs):
    """A cause_ns object; named buckets, everything else zero."""
    out = {name: 0 for name in ch.BUCKETS}
    for name, ns in kwargs.items():
        out[ch.normalize_bucket(name)] = ns
    return out


def hop(comm, ns, cause="oncpu", flags=(), pid=4243):
    return {"pid": pid, "tgid": 4200, "comm": comm, "ns": ns, "cause": cause,
            "flags": list(flags), "kstack_id": -1}


def record(frame_id, cause_ns, worst_cause="block_task", worst_ns=None,
           hops=(), frame_ns=HITCH_NS, budget_ns=BUDGET_NS, flags=()):
    if worst_ns is None:
        worst_ns = cause_ns[ch.normalize_bucket(worst_cause)]
    return {"frame_id": frame_id, "root_pid": 4242, "root_tgid": 4200,
            "root_comm": "hb_root", "frame_start_ns": 1_000_000_000,
            "frame_end_ns": 1_000_000_000 + frame_ns, "frame_ns": frame_ns,
            "budget_ns": budget_ns, "nstalls": 2, "flags": list(flags),
            "cause_ns": cause_ns,
            "worst": {"ns": worst_ns, "start_ns": 1_000_100_000,
                      "cause": worst_cause, "kstack_id": 7, "ustack_id": -1,
                      "hops": list(hops)}}


# root blocked on a worker that was itself blocked: inherited, one hop
def worker_block_record(frame_id, **kwargs):
    c = causes(oncpu=4_000_000, block_task=20_000_000,
               resolved_inherited=18_000_000, resolved_wait_oncpu=2_000_000)
    c.update(kwargs.pop("cause_overrides", {}))
    return record(frame_id, c,
                  hops=[hop("hb_worker", 18_000_000, cause="block_io")],
                  **kwargs)


# root blocked on a worker that was simply computing: waiting for work
def worker_busy_record(frame_id):
    return record(frame_id,
                  causes(oncpu=4_000_000, block_task=20_000_000,
                         resolved_wait_oncpu=19_000_000,
                         resolved_inherited=1_000_000),
                  hops=[hop("hb_worker", 19_000_000, cause="oncpu",
                            flags=["HT_HOP_ONCPU"])])


# root slept on a timer and was woken from the timer interrupt
def timer_record(frame_id):
    return record(frame_id, causes(oncpu=4_000_000, block_timer=20_000_000),
                  worst_cause="block_timer",
                  hops=[hop("swapper/3", 20_000_000, cause="oncpu",
                            flags=["HT_HOP_IRQ", "HT_HOP_IDLE"], pid=0)])


def scene(n_normal=12, injected=(("worker_block", "HT_BLOCK_TASK", 4),),
          make_record=worker_block_record, records_for=None):
    """Build (ground truth, records) for `n_normal` clean frames plus classes.

    `injected` is (class, expected bucket, count); `records_for` limits which
    injected frame_ids get a record (default: all of them).
    """
    frames, recs, frame_id = [], [], 0
    for _ in range(n_normal):
        frame_id += 1
        frames.append(gt_frame(frame_id))
    for cls, expect, count in injected:
        for _ in range(count):
            frame_id += 1
            frames.append(gt_frame(frame_id, injected=cls, expect=expect))
            if records_for is None or frame_id in records_for:
                recs.append(make_record(frame_id))
    return frames, recs


def write_jsonl(path, objs):
    with open(path, "w", encoding="utf-8") as f:
        for obj in objs:
            f.write(json.dumps(obj) + "\n")


class CliTest(unittest.TestCase):
    """Runs check_hitches.main() on files in a per-test temporary directory."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = self._tmp.name
        self.addCleanup(self._tmp.cleanup)

    def files(self, frames, records):
        gt = os.path.join(self.tmp, "gt.jsonl")
        hitch = os.path.join(self.tmp, "hitch.jsonl")
        write_jsonl(gt, frames)
        write_jsonl(hitch, records)
        return gt, hitch

    def run_cli(self, argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = ch.main(list(argv))
        return rc, out.getvalue() + err.getvalue()

    def assertPass(self, argv):
        rc, text = self.run_cli(argv)
        self.assertEqual(rc, 0, "expected pass, got %d:\n%s" % (rc, text))
        return text

    def assertFail(self, argv, *needles):
        rc, text = self.run_cli(argv)
        self.assertEqual(rc, 1, "expected failure, got %d:\n%s" % (rc, text))
        for needle in needles:
            self.assertIn(needle, text)
        return text


# ---------------------------------------------------------------------------
# normalization and parsing


class NormalizeTest(unittest.TestCase):
    def test_bucket_spellings(self):
        for spelling in ("HT_BLOCK_TASK", "block_task", " Block-Task ", 3):
            self.assertEqual(ch.normalize_bucket(spelling), "block_task")
        self.assertEqual(ch.normalize_bucket("inherited"), "resolved_inherited")
        self.assertEqual(ch.normalize_bucket("wait_oncpu"),
                         "resolved_wait_oncpu")
        self.assertEqual(ch.enum_name("resolved_inherited"),
                         "HT_RESOLVED_INHERITED")

    def test_unknown_bucket(self):
        with self.assertRaises(ValueError):
            ch.normalize_bucket("HT_GPU_WAIT")
        with self.assertRaises(ValueError):
            ch.normalize_bucket(99)

    def test_partition_matches_the_contract(self):
        # HT_ONCPU..HT_BLOCK_OTHER is the partition; the rest is resolution.
        self.assertEqual(ch.BUCKETS[:ch.PARTITION][0], "oncpu")
        self.assertEqual(ch.BUCKETS[ch.PARTITION - 1], "block_other")
        self.assertEqual(len(ch.BUCKETS), 9)

    def test_flags_as_bitmask_or_names(self):
        self.assertEqual(ch.normalize_flags(1 << 0 | 1 << 4, ch.HOP_FLAG_BITS),
                         {"irq", "oncpu"})
        self.assertEqual(ch.normalize_flags(["HT_HOP_IRQ"], ch.HOP_FLAG_BITS),
                         {"irq"})
        self.assertEqual(ch.normalize_flags(None, ch.HOP_FLAG_BITS), set())
        self.assertEqual(ch.normalize_flags(1 << 2, ch.FRAME_FLAG_BITS),
                         {"lost"})


class ParseTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)

    def write(self, name, objs):
        path = os.path.join(self._tmp.name, name)
        write_jsonl(path, objs)
        return path

    def test_cause_ns_as_array(self):
        rec = worker_block_record(1)
        rec["cause_ns"] = [4_000_000, 0, 0, 20_000_000, 0, 0, 0,
                           2_000_000, 18_000_000]
        recs, skipped = ch.read_records(self.write("h.jsonl", [rec]))
        self.assertEqual(skipped, 0)
        self.assertEqual(recs[0].bucket("block_task"), 20_000_000)
        self.assertEqual(recs[0].largest(), "block_task")
        self.assertEqual(recs[0].excess_ns, HITCH_NS - BUDGET_NS)

    def test_non_record_lines_are_ignored(self):
        path = self.write("h.jsonl", [{"stats": {"frames": 10}},
                                      worker_block_record(1)])
        recs, skipped = ch.read_records(path)
        self.assertEqual((len(recs), skipped), (1, 1))

    def test_swapped_arguments_are_named(self):
        path = self.write("gt.jsonl", [gt_frame(1, "worker_block",
                                                "HT_BLOCK_TASK")])
        with self.assertRaises(ch.ParseError) as cm:
            ch.read_records(path)
        self.assertIn("ground-truth", str(cm.exception))

    def test_bad_json_is_a_parse_error(self):
        path = os.path.join(self._tmp.name, "h.jsonl")
        with open(path, "w", encoding="utf-8") as f:
            f.write("{not json}\n")
        with self.assertRaises(ch.ParseError):
            ch.read_records(path)

    def test_unknown_bucket_name_in_a_record(self):
        rec = worker_block_record(1)
        rec["cause_ns"]["gpu_wait"] = 5
        with self.assertRaises(ch.ParseError):
            ch.read_records(self.write("h.jsonl", [rec]))


class ChainMatchTest(unittest.TestCase):
    def hops(self):
        return [ch.parse_hop(h, "t") for h in
                (hop("hb_worker", 5, cause="block_io"),
                 hop("swapper/3", 5, flags=["HT_HOP_IRQ"], pid=0))]

    def test_prefix_and_substring(self):
        self.assertIsNone(ch.match_chain(self.hops(), ["hb_work"]))
        self.assertIsNone(ch.match_chain(self.hops(), ["hb_worker", "[hardirq]"]))
        self.assertIsNone(ch.match_chain(self.hops(), ["*", "swapper"]))

    def test_mismatch_and_too_short(self):
        self.assertIn("expected 'kworker'",
                      ch.match_chain(self.hops(), ["kworker"]))
        self.assertIn("need 3", ch.match_chain(self.hops(), ["a", "b", "c"]))
        self.assertIn("need 1", ch.match_chain([], ["hb_worker"]))


# ---------------------------------------------------------------------------
# join


class JoinTest(CliTest):
    def test_table_counts_every_class(self):
        frames, recs = scene(
            injected=(("worker_block", "HT_BLOCK_TASK", 4),
                      ("timer_sleep", "HT_BLOCK_TIMER", 2)),
            make_record=lambda fid: (worker_block_record(fid) if fid <= 16
                                     else timer_record(fid)))
        gt, hitch = self.files(frames, recs)
        text = self.assertPass(["join", gt, hitch])
        self.assertIn("worker_block", text)
        self.assertIn("timer_sleep", text)
        self.assertIn(ch.NO_CLASS, text)
        self.assertIn("18 frame(s), 6 record(s)", text)
        self.assertIn("4 (100.0%)", text)

    def test_missing_record_and_wrong_bucket_are_visible(self):
        frames, recs = scene(injected=(("worker_block", "HT_BLOCK_TASK", 3),),
                             records_for={13, 14})
        # frame 14 is attributed to the wrong bucket
        recs[1] = record(14, causes(oncpu=20_000_000, block_task=4_000_000),
                         worst_cause="oncpu")
        gt, hitch = self.files(frames, recs)
        text = self.assertPass(["join", gt, hitch, "--verbose"])
        self.assertIn("2 (66.7%)", text)		# records for 3 frames
        self.assertIn("1 (50.0%)", text)		# one of those on bucket
        self.assertIn("no record for frame(s): 15", text)
        self.assertIn("frame 14", text)

    def test_records_outside_the_ground_truth_are_reported(self):
        frames, recs = scene(injected=(("worker_block", "HT_BLOCK_TASK", 1),))
        recs.append(worker_block_record(9999))
        gt, hitch = self.files(frames, recs)
        text = self.assertPass(["join", gt, hitch])
        self.assertIn("1 record(s) for frame_ids absent", text)


# ---------------------------------------------------------------------------
# expect


class ExpectTest(CliTest):
    def test_inherited_chain_passes(self):
        frames, recs = scene()
        gt, hitch = self.files(frames, recs)
        text = self.assertPass(["expect", gt, hitch, "--class", "worker_block",
                                "--bucket", "HT_BLOCK_TASK",
                                "--resolved", "inherited",
                                "--chain", "hb_worker", "--min-frames", "4"])
        self.assertIn("4/4", text)

    def test_wait_oncpu_resolution(self):
        frames, recs = scene(injected=(("worker_busy", "HT_BLOCK_TASK", 3),),
                             make_record=worker_busy_record)
        gt, hitch = self.files(frames, recs)
        self.assertPass(["expect", gt, hitch, "--class", "worker_busy",
                         "--bucket", "HT_BLOCK_TASK",
                         "--resolved", "wait_oncpu", "--min-frames", "3"])
        self.assertFail(["expect", gt, hitch, "--class", "worker_busy",
                         "--bucket", "HT_BLOCK_TASK",
                         "--resolved", "inherited"],
                        "wrong resolution", "HT_RESOLVED_INHERITED")

    def test_interrupt_hop_matches_hardirq(self):
        frames, recs = scene(injected=(("timer_sleep", "HT_BLOCK_TIMER", 2),),
                             make_record=timer_record)
        gt, hitch = self.files(frames, recs)
        self.assertPass(["expect", gt, hitch, "--class", "timer_sleep",
                         "--bucket", "HT_BLOCK_TIMER",
                         "--chain", "[hardirq]", "--min-frames", "2"])
        self.assertFail(["expect", gt, hitch, "--class", "timer_sleep",
                         "--bucket", "HT_BLOCK_TIMER", "--chain", "hb_worker"],
                        "chain mismatch", "[hardirq]")

    def test_chain_mismatch_prints_the_frames(self):
        frames, recs = scene()
        gt, hitch = self.files(frames, recs)
        text = self.assertFail(["expect", gt, hitch, "--class", "worker_block",
                                "--bucket", "HT_BLOCK_TASK",
                                "--chain", "hb_worker,kworker"],
                               "chain mismatch", "block_task 20.000 ms")
        self.assertIn("chain has 1 hop(s)", text)

    def test_missing_records_fail_min_frames(self):
        frames, recs = scene(injected=(("worker_block", "HT_BLOCK_TASK", 4),),
                             records_for={13})
        gt, hitch = self.files(frames, recs)
        text = self.assertFail(["expect", gt, hitch, "--class", "worker_block",
                                "--bucket", "HT_BLOCK_TASK",
                                "--min-frames", "3"],
                               "produced no record")
        self.assertIn("1/4", text)
        # one frame is still diagnosed, so --min-frames 1 passes
        self.assertPass(["expect", gt, hitch, "--class", "worker_block",
                         "--bucket", "HT_BLOCK_TASK", "--min-frames", "1"])

    def test_wrong_bucket_fails(self):
        frames, recs = scene()
        recs = [record(r["frame_id"],
                       causes(oncpu=20_000_000, block_task=4_000_000),
                       worst_cause="oncpu") for r in recs]
        gt, hitch = self.files(frames, recs)
        self.assertFail(["expect", gt, hitch, "--class", "worker_block",
                         "--bucket", "HT_BLOCK_TASK"],
                        "wrong bucket", "largest bucket is HT_ONCPU")

    def test_min_frac_guards_a_thin_majority(self):
        # 5 ms of a 24 ms frame: the largest bucket, and 0.68 of the 7.333 ms
        # excess, which is enough at --min-frac 0.5 but not at 0.9
        frames, recs = scene(
            make_record=lambda fid: record(
                fid, causes(oncpu=4_750_000, runnable=4_750_000,
                            runqueue=4_750_000, block_io=4_750_000,
                            block_task=5_000_000, resolved_inherited=5_000_000)))
        gt, hitch = self.files(frames, recs)
        self.assertPass(["expect", gt, hitch, "--class", "worker_block",
                         "--bucket", "HT_BLOCK_TASK", "--min-frac", "0.5",
                         "--min-frames", "4"])
        self.assertFail(["expect", gt, hitch, "--class", "worker_block",
                         "--bucket", "HT_BLOCK_TASK", "--min-frac", "0.9"],
                        "below --min-frac")

    def test_unknown_class_names_what_is_there(self):
        frames, recs = scene()
        gt, hitch = self.files(frames, recs)
        self.assertFail(["expect", gt, hitch, "--class", "reclaim",
                         "--bucket", "HT_BLOCK_TASK"],
                        "no frame injected with 'reclaim'", "worker_block")

    def test_bad_bucket_name_is_a_usage_error(self):
        frames, recs = scene()
        gt, hitch = self.files(frames, recs)
        with self.assertRaises(SystemExit) as cm:
            self.run_cli(["expect", gt, hitch, "--class", "worker_block",
                          "--bucket", "HT_NOPE"])
        self.assertEqual(cm.exception.code, 2)


# ---------------------------------------------------------------------------
# quiet


class QuietTest(CliTest):
    def test_silent_on_normal_frames(self):
        frames, recs = scene()
        gt, hitch = self.files(frames, recs)
        text = self.assertPass(["quiet", gt, hitch])
        self.assertIn("0 of 12 uninjected frame(s) produced a record", text)
        self.assertIn("were over the", text)

    def test_false_positives_fail_and_are_printed(self):
        frames, recs = scene()
        recs.append(record(3, causes(oncpu=6_000_000, runnable=12_000_000),
                           worst_cause="runnable", frame_ns=18_000_000))
        gt, hitch = self.files(frames, recs)
        text = self.assertFail(["quiet", gt, hitch], "frame 3", "runnable")
        self.assertIn("1 of 12", text)
        # ... and are tolerated when the caller allows them
        self.assertPass(["quiet", gt, hitch, "--max-false", "1"])

    def test_over_budget_but_uninjected_rate_is_reported(self):
        frames, recs = scene(n_normal=0)
        for i in range(90, 100):			# 4 of 10 clean frames slow
            frames.append(gt_frame(i, frame_ns=20_000_000 if i < 94
                                   else NORMAL_NS))
        gt, hitch = self.files(frames, recs)
        text = self.assertPass(["quiet", gt, hitch])
        self.assertIn("4 of 10 uninjected frame(s) were over the", text)
        self.assertIn("40.0%", text)

    def test_budget_override_without_records(self):
        frames, _ = scene(n_normal=4, injected=())
        gt, hitch = self.files(frames, [])
        text = self.assertPass(["quiet", gt, hitch, "--budget-us", "10000"])
        self.assertIn("4 of 4 uninjected frame(s) were over the 10.000 ms", text)


# ---------------------------------------------------------------------------
# invariant


class InvariantTest(CliTest):
    def test_partition_sums_to_frame_ns(self):
        _, recs = scene()
        _, hitch = self.files([], recs)
        text = self.assertPass(["invariant", hitch])
        self.assertIn("4 record(s)", text)

    def test_tolerance(self):
        rec = worker_block_record(1)
        rec["cause_ns"]["oncpu"] += 400_000		# 400 us short of frame_ns
        _, hitch = self.files([], [rec])
        self.assertPass(["invariant", hitch])
        self.assertFail(["invariant", hitch, "--tol-us", "100"],
                        "partition sums to", "frame 1")

    def test_partition_violation(self):
        good = worker_block_record(1)
        bad = worker_block_record(2)
        bad["cause_ns"]["block_task"] = 2_000_000	# 18 ms unaccounted for
        _, hitch = self.files([], [good, bad])
        text = self.assertFail(["invariant", hitch],
                               "1 of 2 record(s) do not add up", "frame 2")
        self.assertIn("frame_ns is 24.000 ms", text)
        self.assertNotIn("frame 1:", text)

    def test_resolved_exceeds_block_task(self):
        rec = worker_block_record(1)
        rec["cause_ns"]["resolved_wait_oncpu"] = 20_000_000	# + 18 ms > 20 ms
        _, hitch = self.files([], [rec])
        self.assertFail(["invariant", hitch], "exceeds HT_BLOCK_TASK")

    def test_no_records_is_vacuously_fine(self):
        _, hitch = self.files([], [])
        self.assertPass(["invariant", hitch])


class TraceWindowTest(CliTest):
    """hitchtrace's header/summary lines bound which frames were traced."""

    def test_frames_outside_the_window_are_not_counted_as_misses(self):
        frames, recs = scene(n_normal=0,
                             injected=(("worker_block", "HT_BLOCK_TASK", 4),),
                             records_for=(1, 2))
        # tracing covered only the first two injected frames
        gt, hitch = self.files(frames, recs)
        write_jsonl(hitch, [{"type": "header", "trace_start_ns": 0,
                             "pid": 4200, "budget_ns": BUDGET_NS}] + recs +
                    [{"type": "summary", "trace_end_ns": 1_000_000_002,
                      "frames": 2, "hitches": 2, "records": 2}])
        text = self.assertPass(["expect", gt, hitch, "--class", "worker_block",
                                "--bucket", "HT_BLOCK_TASK", "--min-frames", "2"])
        self.assertIn("frame(s) fall inside the trace window", text)

    def test_window_absent_keeps_every_frame(self):
        frames, recs = scene()
        gt, hitch = self.files(frames, recs)
        text = self.assertPass(["join", gt, hitch])
        self.assertNotIn("trace window", text)


class QuietHonestyTest(CliTest):
    """A record for a frame that really overran is the gate working."""

    def test_uninjected_but_genuinely_slow_frames_are_not_false_positives(self):
        frames, recs = scene(n_normal=0, injected=())
        frames.append(gt_frame(50, frame_ns=NORMAL_NS))         # quick
        frames.append(gt_frame(51, frame_ns=20_000_000))        # really slow
        recs = [record(51, causes(oncpu=20_000_000), worst_cause="oncpu",
                       frame_ns=20_000_000)]
        gt, hitch = self.files(frames, recs)
        text = self.assertPass(["quiet", gt, hitch, "--max-false", "0"])
        self.assertIn("the gate was right", text)

    def test_records_for_frames_within_budget_still_fail(self):
        frames, recs = scene(n_normal=0, injected=())
        frames.append(gt_frame(60, frame_ns=NORMAL_NS))
        recs = [record(60, causes(oncpu=NORMAL_NS), worst_cause="oncpu",
                       frame_ns=NORMAL_NS)]
        gt, hitch = self.files(frames, recs)
        self.assertFail(["quiet", gt, hitch, "--max-false", "0"], "frame 60")


if __name__ == "__main__":
    unittest.main()
