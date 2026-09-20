#!/usr/bin/env python3
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
"""Unit tests for check_hitches.py, using synthetic frames and records.

Run from the project root:  python3 -m unittest tests/test_check_hitches.py
"""

import contextlib
import io
import json
import os
import re
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


def cause_array(**kwargs):
    """The same, as the JSON array in enum order hitchtrace may write."""
    out = [0] * len(ch.BUCKETS)
    for name, ns in kwargs.items():
        out[ch.IDX[ch.normalize_bucket(name)]] = ns
    return out


def hop(comm, ns, cause="oncpu", flags=(), pid=4243):
    return {"pid": pid, "tgid": 4200, "comm": comm, "ns": ns, "cause": cause,
            "flags": list(flags), "kstack_id": -1}


def thread(comm, tid, root=False, largest=None, preemptor=None, open_ns=0,
           **buckets):
    """One threads[] entry, spelled as src/hitchtrace.c writes it.

    Buckets are named keyword arguments, as in causes(), but only the ones
    that partition a timeline: ht_thread has no resolved pair. `largest` is
    (ns, cause, blocked_in) and `preemptor` (comm, tid, ns); a thread that
    never went off-CPU has neither, and says so with nulls.
    """
    cause_ns = {name: 0 for name in ch.BUCKETS[:ch.PARTITION]}
    for name, ns in buckets.items():
        cause_ns[ch.normalize_bucket(name)] = ns
    largest_ns, largest_cause, blocked_in = largest or (0, None, None)
    out = {"tid": tid, "comm": comm, "root": root, "cause_ns": cause_ns,
           "open_ns": open_ns, "preemptor": None,
           "largest": {"ns": largest_ns, "cause": largest_cause,
                       "blocked_in": blocked_in, "kstack": []}}
    if preemptor:
        p_comm, p_tid, p_ns = preemptor
        out["preemptor"] = {"tid": p_tid, "tgid": 4200, "comm": p_comm,
                            "ns": p_ns}
    return out


def record(frame_id, cause_ns, worst_cause="block_task", worst_ns=None,
           hops=(), frame_ns=HITCH_NS, budget_ns=BUDGET_NS, flags=(),
           threads=None):
    if worst_ns is None:
        worst_ns = cause_ns[ch.normalize_bucket(worst_cause)]
    out = {"frame_id": frame_id, "root_pid": 4242, "root_tgid": 4200,
           "root_comm": "hb_root", "frame_start_ns": 1_000_000_000,
           "frame_end_ns": 1_000_000_000 + frame_ns, "frame_ns": frame_ns,
           "budget_ns": budget_ns, "nstalls": 2, "flags": list(flags),
           "cause_ns": cause_ns,
           "worst": {"ns": worst_ns, "start_ns": 1_000_100_000,
                     "cause": worst_cause, "kstack_id": 7, "ustack_id": -1,
                     "hops": list(hops)}}
    if threads is not None:		# absent: a record with no thread detail
        out["threads"] = list(threads)
    return out


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


# The same frame seen per thread: the root's line is the record's own
# timeline, the worker's is context (it blocked longer than the frame lasted
# for the root, which is fine: the two ran in parallel).
def worker_block_threads(**root_overrides):
    root = dict(oncpu=4_000_000, block_task=20_000_000)
    root.update(root_overrides)
    return [thread("hb_root", 4242, root=True,
                   largest=(19_000_000, "block_task", "pipe_read"), **root),
            thread("hb_worker", 4243, oncpu=2_000_000, block_task=21_000_000,
                   largest=(20_000_000, "block_task", "pipe_read"),
                   preemptor=("hb_hog0", 4250, 900_000))]


def threaded_record(frame_id, threads=None, **kwargs):
    if threads is None:
        threads = worker_block_threads()
    return worker_block_record(frame_id, threads=threads, **kwargs)


# root pinned with the hogs: runnable, and it can name what took its CPU
def preempt_record(frame_id, preemptor=("hb_hog0", 4250, 9_000_000)):
    return record(frame_id,
                  causes(oncpu=6_000_000, runnable=16_000_000,
                         runqueue=2_000_000),
                  worst_cause="runnable",
                  threads=[thread("hb_root", 4242, root=True,
                                  oncpu=6_000_000, runnable=16_000_000,
                                  runqueue=2_000_000,
                                  largest=(9_000_000, "runnable", None),
                                  preemptor=preemptor),
                           thread("hb_hog0", 4250, oncpu=20_000_000)])


# The same worker stall under the new taxonomy: the root waits on a condvar,
# so the wait is a futex wait, and it is still resolved through the waker.
def futex_record(frame_id, resolved="inherited", **kwargs):
    other = ("resolved_wait_oncpu" if resolved == "inherited"
             else "resolved_inherited")
    c = causes(oncpu=4_000_000, block_futex=20_000_000,
               **{ch.normalize_bucket(resolved): 18_000_000, other: 2_000_000})
    return record(frame_id, c, worst_cause="block_futex",
                  hops=[hop("hb_worker", 18_000_000, cause="block_io")],
                  **kwargs)


# root polled for an event that arrived late
def poll_record(frame_id):
    return record(frame_id, causes(oncpu=4_000_000, block_poll=20_000_000,
                                   resolved_inherited=18_000_000),
                  worst_cause="block_poll",
                  hops=[hop("hb_worker", 18_000_000, cause="block_io")])


# root never left the CPU: the frame is on-CPU time, most of it in minor
# faults carved out of HT_ONCPU when the frame closed
def fault_record(frame_id, oncpu=6_000_000, fault=16_000_000,
                 fault_major=1_000_000, reclaim=1_000_000, compact=0,
                 frame_ns=HITCH_NS, threads=None):
    return record(frame_id,
                  causes(oncpu=oncpu, oncpu_fault=fault,
                         oncpu_fault_major=fault_major,
                         oncpu_reclaim=reclaim, oncpu_compact=compact),
                  worst_cause="oncpu_fault", frame_ns=frame_ns,
                  threads=threads)


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


SPEC_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "run_hitch_tests.sh")
HEADER_PATH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "src", "hitchtrace.h")


def read_enum_causes():
    """enum ht_cause from src/hitchtrace.h, in order, as enum names.

    HT_CAUSE_PARTITION and HT_CAUSE_MAX are markers rather than buckets, and
    HT_RESOLVED_WAIT_ONCPU shares the first one's value.
    """
    with open(HEADER_PATH, encoding="utf-8") as f:
        body = f.read().split("enum ht_cause {", 1)[1].split("};", 1)[0]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    names = []
    for item in body.split(","):
        name = item.split("=")[0].strip()
        if name and name not in ("HT_CAUSE_PARTITION", "HT_CAUSE_MAX"):
            names.append(name)
    return names


def read_class_specs():
    """The CLASS_SPECS rows of run_hitch_tests.sh, each split on '|'.

    The shell table is what actually runs against a live target, so the
    buckets it names are checked here rather than discovered by root.
    """
    rows, inside = [], False
    with open(SPEC_PATH, encoding="utf-8") as f:
        for line in f:
            text = line.strip()
            if not inside:
                inside = text.startswith("CLASS_SPECS=(")
            elif text == ")":
                break
            elif text.startswith('"') and text.endswith('"'):
                rows.append(text[1:-1].split("|"))
    return rows


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
        for spelling in ("HT_BLOCK_TASK", "block_task", " Block-Task ", 13):
            self.assertEqual(ch.normalize_bucket(spelling), "block_task")
        self.assertEqual(ch.normalize_bucket("inherited"), "resolved_inherited")
        self.assertEqual(ch.normalize_bucket("wait_oncpu"),
                         "resolved_wait_oncpu")
        self.assertEqual(ch.enum_name("resolved_inherited"),
                         "HT_RESOLVED_INHERITED")

    def test_every_cause_answers_to_both_spellings(self):
        # the JSON key hitchtrace writes and the enum name the plan and the
        # shell table use, for every entry of enum ht_cause
        for i, name in enumerate(ch.BUCKETS):
            self.assertEqual(ch.normalize_bucket(name), name)
            self.assertEqual(ch.normalize_bucket(ch.enum_name(name)), name)
            self.assertEqual(ch.normalize_bucket(name.upper()), name)
            self.assertEqual(ch.normalize_bucket(i), name)

    def test_the_new_taxonomy_is_known(self):
        # the plan's 2.3 buckets, which the old table did not have
        for spelling, name in (("HT_ONCPU_FAULT", "oncpu_fault"),
                               ("HT_ONCPU_FAULT_MAJOR", "oncpu_fault_major"),
                               ("oncpu_reclaim", "oncpu_reclaim"),
                               ("HT_ONCPU_COMPACT", "oncpu_compact"),
                               ("block_futex", "block_futex"),
                               ("HT_BLOCK_POLL", "block_poll"),
                               ("HT_BLOCK_GPU", "block_gpu"),
                               ("block_present", "block_present")):
            self.assertEqual(ch.normalize_bucket(spelling), name)

    def test_the_old_aliases_still_work(self):
        for spelling in ("wait_oncpu", "waitoncpu", "resolved_waker_oncpu",
                         "HT_CAUSE_PARTITION"):
            self.assertEqual(ch.normalize_bucket(spelling),
                             "resolved_wait_oncpu")
        self.assertEqual(ch.normalize_bucket("oncpu_major_fault"),
                         "oncpu_fault_major")

    def test_unknown_bucket(self):
        with self.assertRaises(ValueError):
            ch.normalize_bucket("HT_GPU_WAIT")	# HT_BLOCK_GPU, not this
        with self.assertRaises(ValueError):
            ch.normalize_bucket(99)

    def test_partition_matches_the_contract(self):
        # HT_ONCPU..HT_BLOCK_OTHER is the partition; the rest is resolution.
        # The boundary is read off the table, never counted out by hand.
        self.assertEqual(ch.BUCKETS[0], "oncpu")
        self.assertEqual(ch.BUCKETS[ch.PARTITION - 1], "block_other")
        self.assertEqual(ch.BUCKETS[ch.PARTITION], "resolved_wait_oncpu")
        self.assertEqual(ch.PARTITION, ch.IDX["resolved_wait_oncpu"])
        # enum ht_cause: 15 partition buckets and the resolved pair
        self.assertEqual((ch.PARTITION, len(ch.BUCKETS)), (15, 17))

    def test_bucket_groups_follow_the_table(self):
        self.assertEqual([ch.BUCKETS[i] for i in ch.ONCPU_BUCKETS],
                         ["oncpu", "oncpu_fault", "oncpu_fault_major",
                          "oncpu_reclaim", "oncpu_compact"])
        self.assertEqual([ch.BUCKETS[i] for i in ch.ONCPU_STALLS],
                         ["oncpu_fault", "oncpu_fault_major", "oncpu_reclaim",
                          "oncpu_compact"])
        self.assertEqual([ch.BUCKETS[i] for i in ch.BLOCKED],
                         ["block_futex", "block_poll", "block_io",
                          "block_timer", "block_gpu", "block_present",
                          "block_task", "block_other"])
        # runnable and runqueue are off-CPU but nobody woke the thread out
        # of them, so the resolved pair does not split them
        for name in ("runnable", "runqueue"):
            self.assertNotIn(ch.IDX[name], ch.BLOCKED)
            self.assertNotIn(ch.IDX[name], ch.ONCPU_BUCKETS)

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
        rec["cause_ns"] = cause_array(oncpu=4_000_000, block_task=20_000_000,
                                      resolved_wait_oncpu=2_000_000,
                                      resolved_inherited=18_000_000)
        recs, skipped = ch.read_records(self.write("h.jsonl", [rec]))
        self.assertEqual(skipped, 0)
        self.assertEqual(recs[0].bucket("block_task"), 20_000_000)
        self.assertEqual(recs[0].largest(), "block_task")
        self.assertEqual(recs[0].excess_ns, HITCH_NS - BUDGET_NS)

    def test_cause_ns_as_a_partition_length_array(self):
        # a timeline without the resolved pair, as ht_thread carries it
        rec = worker_block_record(1)
        rec["cause_ns"] = cause_array(oncpu=4_000_000,
                                      block_futex=20_000_000)[:ch.PARTITION]
        recs, _ = ch.read_records(self.write("h.jsonl", [rec]))
        self.assertEqual(recs[0].largest(), "block_futex")

    def test_an_array_of_the_wrong_length_is_a_parse_error(self):
        rec = worker_block_record(1)
        rec["cause_ns"] = [0] * (len(ch.BUCKETS) - 1)
        with self.assertRaises(ch.ParseError) as cm:
            ch.read_records(self.write("h.jsonl", [rec]))
        self.assertIn("cause_ns has", str(cm.exception))

    def test_the_new_buckets_parse(self):
        rec = record(1, causes(oncpu=4_000_000, oncpu_fault=2_000_000,
                               block_futex=16_000_000, block_poll=2_000_000,
                               resolved_inherited=15_000_000),
                     worst_cause="block_futex")
        recs, _ = ch.read_records(self.write("h.jsonl", [rec]))
        self.assertEqual(recs[0].largest(), "block_futex")
        self.assertEqual(recs[0].bucket("oncpu_fault"), 2_000_000)
        self.assertEqual(recs[0].worst_cause, "block_futex")

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


class ThreadParseTest(unittest.TestCase):
    """threads[] is new, and old records simply do not carry it."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)

    def read(self, rec):
        path = os.path.join(self._tmp.name, "h.jsonl")
        write_jsonl(path, [rec])
        recs, _ = ch.read_records(path)
        return recs[0]

    def test_a_record_without_threads_has_none(self):
        self.assertEqual(self.read(worker_block_record(1)).threads, [])

    def test_threads_are_parsed_whole(self):
        rec = self.read(threaded_record(1))
        self.assertEqual([t.comm for t in rec.threads],
                         ["hb_root", "hb_worker"])
        root, worker = rec.threads
        self.assertTrue(root.root)
        self.assertEqual(rec.roots(), [root])
        self.assertEqual(root.bucket("block_task"), 20_000_000)
        self.assertEqual(root.largest.cause, "block_task")
        self.assertEqual(root.largest.blocked_in, "pipe_read")
        self.assertIsNone(root.preemptor)
        self.assertFalse(worker.root)
        self.assertEqual(worker.tid, 4243)
        self.assertEqual(worker.largest_bucket(), "block_task")
        self.assertEqual(worker.offcpu_ns(), 21_000_000)	# not the oncpu
        self.assertEqual(worker.preemptor.comm, "hb_hog0")
        self.assertEqual(worker.preemptor.ns, 900_000)

    def test_the_root_may_be_spelled_as_a_thread_flag(self):
        # hitchtrace writes "root": true, but ht_thread.flags carries the
        # same bit and is how every other flag field of a record is rendered
        rec = self.read(threaded_record(1, threads=[
            {"tid": 4242, "comm": "hb_root", "flags": ["HT_THREAD_ROOT"],
             "cause_ns": {"oncpu": 24_000_000}},
            {"tid": 4243, "comm": "hb_worker", "flags": 1 << 1,
             "cause_ns": {"block_task": 5_000_000}, "open_ns": 5_000_000}]))
        self.assertTrue(rec.threads[0].root)
        self.assertFalse(rec.threads[1].root)
        self.assertIn("blocked_end", rec.threads[1].flags)

    def test_preemptor_and_largest_as_flat_keys(self):
        # the same thread, spelled without the nested objects
        rec = self.read(threaded_record(1, threads=[
            {"pid": 4243, "comm": "hb_worker",
             "cause_ns": {"block_task": 21_000_000},
             "largest_ns": 20_000_000, "largest_cause": "block_task",
             "blocked_in": "pipe_read", "preemptor_pid": 4250,
             "preemptor_comm": "hb_hog0", "preemptor_ns": 900_000}]))
        th = rec.threads[0]
        self.assertEqual(th.largest.blocked_in, "pipe_read")
        self.assertEqual((th.preemptor.tid, th.preemptor.comm), (4250, "hb_hog0"))

    def test_an_empty_preemptor_is_nobody(self):
        rec = self.read(threaded_record(1, threads=[
            thread("hb_worker", 4243, block_task=1_000_000)]))
        self.assertIsNone(rec.threads[0].preemptor)

    def test_threads_must_be_an_array(self):
        rec = threaded_record(1)
        rec["threads"] = {"hb_root": 4242}
        with self.assertRaises(ch.ParseError) as cm:
            self.read(rec)
        self.assertIn("threads is dict", str(cm.exception))

    def test_an_unknown_bucket_in_a_thread_is_a_parse_error(self):
        threads = worker_block_threads()
        threads[0]["cause_ns"]["gpu_wait"] = 5
        with self.assertRaises(ch.ParseError):
            self.read(threaded_record(1, threads=threads))


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

    def test_thread_count_column(self):
        frames, recs = scene(
            injected=(("worker_block", "HT_BLOCK_TASK", 2),
                      ("timer_sleep", "HT_BLOCK_TIMER", 2)),
            make_record=lambda fid: (threaded_record(fid) if fid <= 14
                                     else timer_record(fid)))
        gt, hitch = self.files(frames, recs)
        text = self.assertPass(["join", gt, hitch])
        self.assertIn("threads", text.splitlines()[0])
        rows = {ln.split()[0]: ln.split() for ln in text.splitlines()[1:4]}
        self.assertEqual(rows["worker_block"][-1], "2")
        self.assertEqual(rows["timer_sleep"][-1], "0")	# no detail emitted
        self.assertEqual(rows[ch.NO_CLASS][-1], "-")	# no records at all

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

    def test_a_futex_wait_is_resolved_through_its_waker(self):
        # worker_block and worker_cpu now land in HT_BLOCK_FUTEX (the root
        # waits on a condvar), and the resolution is unchanged
        frames, recs = scene(injected=(("worker_block", "HT_BLOCK_FUTEX", 4),),
                             make_record=futex_record)
        gt, hitch = self.files(frames, recs)
        self.assertPass(["expect", gt, hitch, "--class", "worker_block",
                         "--bucket", "HT_BLOCK_FUTEX",
                         "--resolved", "inherited",
                         "--chain", "hb_worker", "--min-frames", "4"])
        self.assertFail(["expect", gt, hitch, "--class", "worker_block",
                         "--bucket", "HT_BLOCK_FUTEX",
                         "--resolved", "wait_oncpu"],
                        "wrong resolution",
                        "HT_RESOLVED_WAIT_ONCPU is 2.000 ms of the 20.000 ms "
                        "the frame spent blocked")

    def test_a_frame_that_blocked_nowhere_resolves_nothing(self):
        frames, recs = scene(injected=(("fault", "HT_ONCPU_FAULT", 2),),
                             make_record=fault_record)
        gt, hitch = self.files(frames, recs)
        self.assertFail(["expect", gt, hitch, "--class", "fault",
                         "--bucket", "HT_ONCPU_FAULT",
                         "--resolved", "inherited"],
                        "blocked nowhere, nothing to resolve")

    def test_every_new_bucket_can_be_the_diagnosis(self):
        # one class per 2.3 bucket, each dominated by the bucket it expects
        for cls, bucket, make in (("poll", "HT_BLOCK_POLL", poll_record),
                                  ("fault", "HT_ONCPU_FAULT", fault_record),
                                  ("worker_cpu", "HT_BLOCK_FUTEX",
                                   futex_record)):
            with self.subTest(cls=cls):
                frames, recs = scene(injected=((cls, bucket, 3),),
                                     make_record=make)
                gt, hitch = self.files(frames, recs)
                self.assertPass(["expect", gt, hitch, "--class", cls,
                                 "--bucket", bucket, "--min-frames", "3"])

    def test_a_fault_frame_is_not_plain_on_cpu_time(self):
        # the frame is on-CPU the whole time, but the faults are carved out
        # of HT_ONCPU, so they and not it are the diagnosis
        frames, recs = scene(injected=(("fault", "HT_ONCPU_FAULT", 3),),
                             make_record=fault_record)
        gt, hitch = self.files(frames, recs)
        self.assertPass(["expect", gt, hitch, "--class", "fault",
                         "--bucket", "HT_ONCPU_FAULT", "--min-frames", "3"])
        self.assertFail(["expect", gt, hitch, "--class", "fault",
                         "--bucket", "HT_ONCPU"],
                        "wrong bucket", "largest bucket is HT_ONCPU_FAULT")

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
# threads


class ThreadsCommandTest(CliTest):
    def scene(self, make_record=threaded_record, count=4, **kwargs):
        frames, recs = scene(injected=(("worker_block", "HT_BLOCK_TASK", count),),
                             make_record=make_record, **kwargs)
        return self.files(frames, recs)

    def test_worker_thread_is_found_and_described(self):
        gt, hitch = self.scene()
        text = self.assertPass(["threads", gt, hitch, "--class", "worker_block",
                                "--thread", "hb_worker",
                                "--bucket", "HT_BLOCK_TASK",
                                "--blocked-in", "pipe_", "--min-frames", "4"])
        self.assertIn("4/4", text)

    def test_missing_thread_fails(self):
        gt, hitch = self.scene()
        text = self.assertFail(["threads", gt, hitch, "--class", "worker_block",
                                "--thread", "hb_feeder"],
                               "no such thread", "hb_feeder")
        self.assertIn("threads: hb_root/4242, hb_worker/4243", text)

    def test_wrong_bucket_fails(self):
        gt, hitch = self.scene()
        self.assertFail(["threads", gt, hitch, "--class", "worker_block",
                         "--thread", "hb_worker", "--bucket", "HT_BLOCK_IO"],
                        "wrong bucket", "largest bucket is HT_BLOCK_TASK")

    def test_wrong_preemptor_fails(self):
        gt, hitch = self.scene()
        self.assertFail(["threads", gt, hitch, "--class", "worker_block",
                         "--thread", "hb_worker", "--preemptor", "kworker"],
                        "wrong preemptor", "preemptor is hb_hog0/4250")

    def test_blocked_in_mismatch_fails(self):
        gt, hitch = self.scene()
        self.assertFail(["threads", gt, hitch, "--class", "worker_block",
                         "--thread", "hb_worker",
                         "--blocked-in", "futex_wait"],
                        "blocked-in mismatch", "blocked in 'pipe_read'")

    def test_a_thread_with_no_blocking_stack_cannot_match_blocked_in(self):
        gt, hitch = self.scene(
            make_record=lambda fid: threaded_record(
                fid, threads=[thread("hb_root", 4242, root=True,
                                     oncpu=4_000_000, block_task=20_000_000)]))
        self.assertFail(["threads", gt, hitch, "--class", "worker_block",
                         "--blocked-in", "pipe_"],
                        "names no kernel function")

    def test_bucket_any_accepts_either_spelling_of_the_stall(self):
        # the worker's wait shows up as a timer wake on some runs
        gt, hitch = self.scene(
            make_record=lambda fid: threaded_record(
                fid, threads=worker_block_threads() + [
                    thread("hb_feeder", 4244, block_timer=22_000_000,
                           largest=(21_000_000, "block_timer",
                                    "hrtimer_nanosleep"))]))
        for one in ("HT_BLOCK_TASK", "HT_BLOCK_TIMER"):
            self.assertPass(["threads", gt, hitch, "--class", "worker_block",
                             "--thread", "hb_", "--bucket", one,
                             "--min-frames", "4"])
        self.assertPass(["threads", gt, hitch, "--class", "worker_block",
                         "--thread", "hb_feeder",
                         "--bucket-any", "HT_BLOCK_TASK,HT_BLOCK_TIMER",
                         "--min-frames", "4"])
        self.assertFail(["threads", gt, hitch, "--class", "worker_block",
                         "--thread", "hb_feeder",
                         "--bucket-any", "HT_BLOCK_TASK,HT_BLOCK_IO"],
                        "expected HT_BLOCK_TASK or HT_BLOCK_IO")

    def test_bucket_any_weighs_the_buckets_together(self):
        # one wait spelled two ways: neither half is half the off-CPU time,
        # but the pair is all of it
        gt, hitch = self.scene(
            make_record=lambda fid: threaded_record(
                fid, threads=worker_block_threads() + [
                    thread("hb_feeder", 4244, block_task=11_000_000,
                           block_timer=11_000_000,
                           largest=(11_000_000, "block_task", "pipe_write"))]))
        self.assertPass(["threads", gt, hitch, "--class", "worker_block",
                         "--thread", "hb_feeder",
                         "--bucket-any", "HT_BLOCK_TASK,HT_BLOCK_TIMER",
                         "--min-frac", "0.9", "--min-frames", "4"])
        self.assertFail(["threads", gt, hitch, "--class", "worker_block",
                         "--thread", "hb_feeder", "--bucket", "HT_BLOCK_TASK",
                         "--min-frac", "0.9"],
                        "below --min-frac")

    def test_bucket_any_spans_the_names_one_wait_can_take(self):
        # run_hitch_tests.sh's worker_block row: hb_worker blocks in a pipe
        # read, which the new rules may name TASK, FUTEX or TIMER
        for name in ("block_task", "block_futex", "block_timer"):
            with self.subTest(bucket=name):
                gt, hitch = self.scene(
                    make_record=lambda fid, n=name: threaded_record(
                        fid, threads=worker_block_threads() + [
                            thread("hb_worker2", 4245,
                                   largest=(20_000_000, n, "pipe_read"),
                                   **{n: 21_000_000})]))
                self.assertPass(["threads", gt, hitch, "--class",
                                 "worker_block", "--thread", "hb_worker2",
                                 "--bucket-any",
                                 "HT_BLOCK_TASK,HT_BLOCK_FUTEX,HT_BLOCK_TIMER",
                                 "--min-frames", "4"])

    def test_on_cpu_stalls_are_not_off_cpu_time(self):
        # hb_feeder faulted for 3 ms and blocked for 4: a fault is time on a
        # CPU, so all 4 ms of its off-CPU time are the wait --min-frac asks
        # about, not 4 of 7
        gt, hitch = self.scene(
            make_record=lambda fid: threaded_record(
                fid, threads=worker_block_threads() + [
                    thread("hb_feeder", 4244, oncpu=2_000_000,
                           oncpu_fault=3_000_000, block_task=4_000_000,
                           largest=(4_000_000, "block_task", "pipe_write"))]))
        self.assertPass(["threads", gt, hitch, "--class", "worker_block",
                         "--thread", "hb_feeder", "--bucket", "HT_BLOCK_TASK",
                         "--min-frac", "0.9", "--min-frames", "4"])

    def test_min_frac_is_measured_against_the_thread_off_cpu_time(self):
        # 6 ms of the worker's 20 ms off-CPU: the largest, but only 0.30 of it
        gt, hitch = self.scene(
            make_record=lambda fid: threaded_record(
                fid, threads=worker_block_threads() + [
                    thread("hb_feeder", 4244, oncpu=4_000_000,
                           runnable=5_000_000, block_task=6_000_000,
                           block_io=5_000_000, block_timer=4_000_000,
                           largest=(6_000_000, "block_task", "pipe_write"))]))
        self.assertPass(["threads", gt, hitch, "--class", "worker_block",
                         "--thread", "hb_feeder", "--bucket", "HT_BLOCK_TASK",
                         "--min-frac", "0.25", "--min-frames", "4"])
        self.assertFail(["threads", gt, hitch, "--class", "worker_block",
                         "--thread", "hb_feeder", "--bucket", "HT_BLOCK_TASK",
                         "--min-frac", "0.5"],
                        "below --min-frac", "0.50 of the 20.000 ms")

    def test_the_root_names_what_took_its_cpu(self):
        frames, recs = scene(injected=(("preempt", "HT_RUNNABLE", 3),),
                             make_record=preempt_record)
        gt, hitch = self.files(frames, recs)
        self.assertPass(["threads", gt, hitch, "--class", "preempt",
                         "--thread", "hb_root", "--bucket", "HT_RUNNABLE",
                         "--preemptor", "hb_hog", "--min-frames", "3"])

    def test_a_root_nothing_preempted_fails(self):
        frames, recs = scene(injected=(("preempt", "HT_RUNNABLE", 3),),
                             make_record=lambda fid: preempt_record(fid,
                                                                    preemptor=None))
        gt, hitch = self.files(frames, recs)
        self.assertFail(["threads", gt, hitch, "--class", "preempt",
                         "--thread", "hb_root", "--preemptor", "hb_hog"],
                        "nothing took this thread's CPU")

    def test_records_without_thread_detail_fail(self):
        gt, hitch = self.scene(make_record=worker_block_record)
        self.assertFail(["threads", gt, hitch, "--class", "worker_block",
                         "--thread", "hb_worker"],
                        "no threads", "carries no per-thread detail")

    def test_a_frame_with_no_record_at_all(self):
        gt, hitch = self.scene(records_for={13, 14})
        text = self.assertFail(["threads", gt, hitch, "--class", "worker_block",
                                "--thread", "hb_worker", "--min-frames", "3"],
                               "produced no record")
        self.assertIn("2/4", text)
        self.assertPass(["threads", gt, hitch, "--class", "worker_block",
                         "--thread", "hb_worker", "--min-frames", "2"])

    def test_diagnostics_print_the_thread_table(self):
        gt, hitch = self.scene()
        text = self.assertFail(["threads", gt, hitch, "--class", "worker_block",
                                "--thread", "hb_worker",
                                "--bucket", "HT_BLOCK_IO"],
                               "threads (2):")
        self.assertIn("* hb_root/4242: oncpu 4.000 ms  block_task 20.000 ms",
                      text)
        self.assertIn("largest block_task 20.000 ms in pipe_read", text)
        self.assertIn("preempted by hb_hog0/4250 for 0.900 ms", text)

    def test_without_thread_any_thread_may_answer(self):
        gt, hitch = self.scene()
        self.assertPass(["threads", gt, hitch, "--class", "worker_block",
                         "--preemptor", "hb_hog", "--min-frames", "4"])

    def test_unknown_class_names_what_is_there(self):
        gt, hitch = self.scene()
        self.assertFail(["threads", gt, hitch, "--class", "reclaim",
                         "--thread", "hb_worker"],
                        "no frame injected with 'reclaim'", "worker_block")

    def test_a_resolution_bucket_is_a_usage_error(self):
        gt, hitch = self.scene()
        with self.assertRaises(SystemExit) as cm:
            self.run_cli(["threads", gt, hitch, "--class", "worker_block",
                          "--bucket", "HT_RESOLVED_INHERITED"])
        self.assertEqual(cm.exception.code, 2)

    def test_a_bad_regex_is_a_usage_error(self):
        gt, hitch = self.scene()
        with self.assertRaises(SystemExit) as cm:
            self.run_cli(["threads", gt, hitch, "--class", "worker_block",
                          "--blocked-in", "pipe_("])
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

    def test_resolved_exceeds_the_blocked_time(self):
        rec = worker_block_record(1)
        rec["cause_ns"]["resolved_wait_oncpu"] = 20_000_000	# + 18 ms > 20 ms
        _, hitch = self.files([], [rec])
        self.assertFail(["invariant", hitch],
                        "exceeds the 20.000 ms the frame spent blocked")

    def test_a_futex_wait_may_be_resolved_too(self):
        # the pair splits any wait a task ended, not just HT_BLOCK_TASK
        _, hitch = self.files([], [futex_record(1)])
        self.assertPass(["invariant", hitch])
        rec = futex_record(2)
        rec["cause_ns"]["block_futex"] = 5_000_000	# 20 resolved of 5 ms
        rec["cause_ns"]["oncpu"] = 19_000_000		# ... still partitions
        _, hitch = self.files([], [rec])
        self.assertFail(["invariant", hitch], "exceeds the 5.000 ms")

    def test_no_records_is_vacuously_fine(self):
        _, hitch = self.files([], [])
        self.assertPass(["invariant", hitch])


class OncpuStallTest(CliTest):
    """--oncpu-stall: the buckets carved out of HT_ONCPU at frame close."""

    def test_a_fault_frame_adds_up(self):
        _, hitch = self.files([], [fault_record(1), fault_record(2)])
        text = self.assertPass(["invariant", hitch, "--oncpu-stall"])
        self.assertIn("2 record(s) partition frame_ns", text)
        self.assertIn("on-CPU stalls fit the frame", text)
        # the option is off by default and says nothing when it is
        self.assertNotIn("on-CPU stalls fit",
                         self.assertPass(["invariant", hitch]))

    def test_stalls_larger_than_the_frame(self):
        # 25 ms of faults carved out of a 24 ms frame: impossible
        rec = fault_record(1, oncpu=0, fault=25_000_000, fault_major=0,
                           reclaim=0)
        _, hitch = self.files([], [rec])
        text = self.assertFail(["invariant", hitch, "--oncpu-stall"],
                               "the on-CPU stalls sum to 25.000 ms",
                               "more than the 24.000 ms frame")
        self.assertIn("oncpu_fault 25.000 ms", text)

    def test_an_underflowed_oncpu_is_named(self):
        # the carve-out went below zero; HT_ONCPU is unsigned, so it wrapped
        rec = fault_record(1)
        rec["cause_ns"]["oncpu"] = 2 ** 64 - 3_000_000
        _, hitch = self.files([], [rec])
        self.assertFail(["invariant", hitch, "--oncpu-stall"],
                        "took it below zero")
        # without the option the wrap is only ever a partition failure
        text = self.assertFail(["invariant", hitch], "partition sums to")
        self.assertNotIn("below zero", text)

    def test_a_thread_carve_out_is_checked_too(self):
        # the record adds up and the root agrees with it; only hb_worker's
        # own line wrapped, which nothing else looks at
        threads = worker_block_threads() + [
            thread("hb_feeder", 4244, oncpu=2 ** 64 - 1_000_000,
                   largest=(1_000_000, "block_task", "pipe_write"))]
        _, hitch = self.files([], [threaded_record(1, threads=threads)])
        self.assertPass(["invariant", hitch])
        self.assertFail(["invariant", hitch, "--oncpu-stall"],
                        "hb_feeder/4244: HT_ONCPU is", "took it below zero")

    def test_a_threads_stalls_must_fit_the_frame(self):
        threads = worker_block_threads() + [
            thread("hb_feeder", 4244, oncpu_fault=20_000_000,
                   oncpu_reclaim=6_000_000,
                   largest=(1_000_000, "block_task", "pipe_write"))]
        _, hitch = self.files([], [threaded_record(1, threads=threads)])
        self.assertPass(["invariant", hitch])
        self.assertFail(["invariant", hitch, "--oncpu-stall"],
                        "hb_feeder/4244: the on-CPU stalls sum to 26.000 ms")

    def test_tolerance_applies_to_the_carve_out(self):
        # 400 us of faults past the end of the frame: the same slack the
        # partition check gives the same record
        rec = fault_record(1, oncpu=0, fault=HITCH_NS + 400_000,
                           fault_major=0, reclaim=0)
        _, hitch = self.files([], [rec])
        self.assertPass(["invariant", hitch, "--oncpu-stall"])
        self.assertFail(["invariant", hitch, "--oncpu-stall",
                         "--tol-us", "100"],
                        "more than the 24.000 ms frame")


class InvariantThreadsTest(CliTest):
    """threads[] and the record are two views of one frame; they must agree."""

    def test_the_root_line_matches_the_record(self):
        _, hitch = self.files([], [threaded_record(1), preempt_record(2)])
        text = self.assertPass(["invariant", hitch])
        self.assertIn("2 with per-thread detail", text)

    def test_records_without_threads_are_not_checked_for_a_root(self):
        _, hitch = self.files([], [worker_block_record(1)])
        text = self.assertPass(["invariant", hitch])
        self.assertIn("0 with per-thread detail", text)

    def test_exactly_one_thread_must_be_the_root(self):
        two = worker_block_threads()
        two[1]["root"] = True				# the worker too
        _, hitch = self.files([], [threaded_record(1, threads=two)])
        self.assertFail(["invariant", hitch],
                        "2 of 2 thread(s) are flagged as the root",
                        "hb_root/4242, hb_worker/4243")

        none = worker_block_threads()
        none[0]["root"] = False
        _, hitch = self.files([], [threaded_record(1, threads=none)])
        self.assertFail(["invariant", hitch],
                        "0 of 2 thread(s) are flagged as the root")

    def test_the_root_buckets_must_match_the_top_level_ones(self):
        # the root's line says the wait was a timer, the record says a task
        threads = worker_block_threads(block_task=0, block_timer=20_000_000)
        _, hitch = self.files([], [threaded_record(1, threads=threads)])
        text = self.assertFail(["invariant", hitch],
                               "the root thread hb_root/4242 and the record "
                               "disagree")
        self.assertIn("block_task: root 0.000 ms, record 20.000 ms", text)
        self.assertIn("block_timer: root 20.000 ms, record 0.000 ms", text)
        self.assertIn("threads (2):", text)		# the table is printed

    def test_a_small_disagreement_is_within_tolerance(self):
        threads = worker_block_threads(oncpu=4_400_000)	# 400 us over
        _, hitch = self.files([], [threaded_record(1, threads=threads)])
        self.assertPass(["invariant", hitch])
        self.assertFail(["invariant", hitch, "--tol-us", "100"],
                        "oncpu: root 4.400 ms, record 4.000 ms")

    def test_other_threads_never_have_to_add_up(self):
        # hb_worker reports 23 ms in a 24 ms frame and is off-CPU at the end:
        # it ran in parallel, so none of that is the frame's to explain
        threads = worker_block_threads()
        threads[1]["open_ns"] = 5_000_000
        _, hitch = self.files([], [threaded_record(1, threads=threads,
                                                   flags=["open_stall"])])
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


class HeaderTableTest(unittest.TestCase):
    """The bucket table is enum ht_cause; the header is where it comes from."""

    @unittest.skipUnless(os.path.exists(HEADER_PATH), "no src/hitchtrace.h")
    def test_the_table_is_the_enum(self):
        names = read_enum_causes()
        self.assertEqual([ch.enum_name(b) for b in ch.BUCKETS], names)
        self.assertEqual(names[ch.PARTITION], "HT_RESOLVED_WAIT_ONCPU")


class ShellTableTest(unittest.TestCase):
    """run_hitch_tests.sh's table names buckets this checker has to know.

    The end-to-end script cannot run here (it loads BPF programs), but its
    CLASS_SPECS rows are just arguments to check_hitches.py, and a row that
    names a bucket the table below does not have is a usage error at 3 a.m.
    on a machine with root, not here.
    """

    def setUp(self):
        self.rows = read_class_specs()
        self.assertTrue(self.rows, "no CLASS_SPECS rows in %s" % SPEC_PATH)
        self.by_class = {row[0]: row for row in self.rows}

    def test_every_row_is_a_usable_check(self):
        for row in self.rows:
            with self.subTest(cls=row[0]):
                # field 8 is optional: extra arguments for `expect`
                self.assertIn(len(row), (7, 8),
                              "row has %d fields" % len(row))
                if row[1]:			# the expected bucket
                    bucket = ch.normalize_bucket(row[1])
                    self.assertLess(ch.IDX[bucket], ch.PARTITION,
                                    "%s is not a partition bucket" % row[1])
                self.assertIn(row[2], ("", "inherited", "wait_oncpu"))
                if len(row) == 8 and row[7]:
                    # must parse as `expect` options, or the row is dead
                    args = ["expect", "gt", "hitch", "--class", row[0],
                            "--bucket", row[1] or "oncpu"] + row[7].split()
                    ch.build_parser().parse_args(args)

    def test_the_expected_bucket_of_every_class(self):
        expect = {"sleep": "block_timer", "worker_block": "block_futex",
                  "worker_cpu": "block_futex", "cpu_spike": "oncpu",
                  "preempt": "runnable", "thousand_cuts": "block_timer",
                  "io": "block_io", "poll": "block_poll",
                  "fault": "oncpu_fault"}
        for cls, bucket in expect.items():
            with self.subTest(cls=cls):
                self.assertIn(cls, self.by_class)
                self.assertEqual(ch.normalize_bucket(self.by_class[cls][1]),
                                 bucket)
        self.assertEqual(self.by_class["quiet_baseline"][1], "")

    def test_the_worker_classes_differ_by_resolution(self):
        # both wait on the same condvar; what tells them apart is the waker
        self.assertEqual(self.by_class["worker_block"][2], "inherited")
        self.assertEqual(self.by_class["worker_cpu"][2], "wait_oncpu")

    def test_the_worker_thread_check_takes_every_name_its_wait_can_take(self):
        args = self.by_class["worker_block"][5].split()
        self.assertIn("--thread", args)
        self.assertIn("hb_worker", args)
        names = args[args.index("--bucket-any") + 1].split(",")
        self.assertEqual([ch.normalize_bucket(b) for b in names],
                         ["block_task", "block_futex", "block_timer"])

    def test_the_new_classes_run_like_the_others(self):
        for cls in ("poll", "fault"):
            with self.subTest(cls=cls):
                row = self.by_class[cls]
                self.assertEqual(row[4], self.by_class["io"][4])  # min-frames
                self.assertEqual(row[6], "-c %s" % cls)

    def test_the_invariant_check_weighs_the_carve_outs(self):
        with open(SPEC_PATH, encoding="utf-8") as f:
            script = f.read()
        self.assertIn("check invariant", script)
        line = [ln for ln in script.splitlines() if "check invariant" in ln][0]
        self.assertIn("--oncpu-stall", line)


class AnySizeTest(CliTest):
    """A cause that shares its frame with unavoidable work (e.g. faults)."""

    def _scene(self):
        # the fault class: on-CPU work dominates, fault time is still the story
        frames = [gt_frame(1, injected="fault", expect="HT_ONCPU_FAULT",
                           frame_ns=40_000_000)]
        recs = [record(1, causes(oncpu=16_000_000, oncpu_fault=13_000_000,
                                 block_timer=11_000_000),
                       worst_cause="block_timer", frame_ns=40_000_000)]
        return self.files(frames, recs)

    def test_largest_required_by_default(self):
        gt, hitch = self._scene()
        self.assertFail(["expect", gt, hitch, "--class", "fault",
                         "--bucket", "HT_ONCPU_FAULT", "--min-frac", "0.3"],
                        "wrong bucket")

    def test_any_size_accepts_a_non_largest_bucket(self):
        gt, hitch = self._scene()
        text = self.assertPass(["expect", gt, hitch, "--class", "fault",
                                "--bucket", "HT_ONCPU_FAULT",
                                "--min-frac", "0.3", "--any-size"])
        self.assertIn("carrying", text)

    def test_any_size_still_enforces_min_frac(self):
        gt, hitch = self._scene()
        self.assertFail(["expect", gt, hitch, "--class", "fault",
                         "--bucket", "HT_ONCPU_FAULT", "--min-frac", "0.9",
                         "--any-size"], "below --min-frac")


if __name__ == "__main__":
    unittest.main()
