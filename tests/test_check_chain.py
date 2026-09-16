#!/usr/bin/env python3
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
"""Unit tests for check_chain.py, using synthetic folded lines.

Run from the project root:  python3 -m unittest tests/test_check_chain.py
"""

import contextlib
import io
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import check_chain as cc  # noqa: E402

# ---------------------------------------------------------------------------
# synthetic stacks, laid out as chaingraph prints them

SINK_USER = "_start;__libc_start_main;main;cg_stage_sink;cg_raw_syscall"      # root->leaf
SINK_KERNEL = ("entry_SYSCALL_64_after_hwframe;do_syscall_64;ksys_read;vfs_read;"
               "anon_pipe_read;schedule;__schedule")                           # root->leaf
WRITE_KERNEL = ("try_to_wake_up;autoremove_wake_function;__wake_up_common;"
                "__wake_up_sync_key;anon_pipe_write;vfs_write;ksys_write;"
                "do_syscall_64;entry_SYSCALL_64_after_hwframe")                # leaf->root
RELAY_USER = "cg_raw_syscall;cg_write_all;cg_stage_relay;main;__libc_start_main;_start"
SOURCE_USER = "cg_raw_syscall;cg_write_all;cg_stage_source;main;__libc_start_main;_start"
HARDIRQ_KERNEL = ("try_to_wake_up;hrtimer_wakeup;__hrtimer_run_queues;hrtimer_interrupt;"
                  "sysvec_apic_timer_interrupt;asm_sysvec_apic_timer_interrupt")
IDLE_TAIL = "pv_native_safe_halt;default_idle;do_idle;cpu_startup_entry"
SOFTIRQ_KERNEL = ("try_to_wake_up;process_timeout;call_timer_fn;run_timer_softirq;"
                  "handle_softirqs;__irq_exit_rcu;irq_exit_rcu;"
                  "sysvec_apic_timer_interrupt;asm_sysvec_apic_timer_interrupt")


def target_seg(label, user=SINK_USER, kernel=SINK_KERNEL):
    parts = [label]
    if user:
        parts.append(user)
    if user and kernel:
        parts.append("-")
    if kernel:
        parts.append(kernel)
    return ";".join(parts)


def waker_seg(label, kernel=WRITE_KERNEL, user=RELAY_USER):
    parts = []
    if kernel:
        parts.append(kernel)
    if kernel and user:
        parts.append("-")
    if user:
        parts.append(user)
    parts.append(label)
    return ";".join(parts)


def line(target, wakers=(), marker=None, us=1000):
    segs = [target] + list(wakers)
    if marker:
        segs.append(marker)
    return "%s %d" % (";--;".join(segs), us)


def sink_line(us=1000, label="cg_sink", relay2="cg_relay2", relay1="cg_relay1",
              source="cg_source"):
    return line(target_seg(label), [
        waker_seg(relay2),
        waker_seg(relay1),
        waker_seg(source, user=SOURCE_USER),
        waker_seg("[hardirq]", kernel=HARDIRQ_KERNEL, user=None),
    ], us=us)


FULL = ["cg_relay2", "cg_relay1", "cg_source", "[hardirq]"]


def entries_of(*lines):
    entries, errors = cc.parse_folded(l + "\n" for l in lines)
    assert not errors, errors
    return entries


def run_cli(argv, text):
    with tempfile.NamedTemporaryFile("w", suffix=".folded", delete=False) as f:
        f.write(text)
        path = f.name
    out = io.StringIO()
    try:
        with contextlib.redirect_stdout(out):
            rc = cc.main([argv[0], path] + list(argv[1:]))
    finally:
        os.unlink(path)
    return rc, out.getvalue()


# ---------------------------------------------------------------------------


class ParseLineTest(unittest.TestCase):
    def test_basic(self):
        frames, us = cc.parse_line("a;b;c 42\n")
        self.assertEqual(frames, ["a", "b", "c"])
        self.assertEqual(us, 42)

    def test_count_split_at_last_space(self):
        frames, us = cc.parse_line("app;operator new(unsigned long);-;x 7")
        self.assertEqual(frames, ["app", "operator new(unsigned long)", "-", "x"])
        self.assertEqual(us, 7)

    def test_blank(self):
        self.assertIsNone(cc.parse_line("\n"))
        self.assertIsNone(cc.parse_line("   "))

    def test_malformed(self):
        for bad in ("a;b;c", "a;b 1.5", "a;b -3", " 5", "a;b 12 "):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                cc.parse_line(bad)

    def test_parse_folded_collects_errors(self):
        entries, errors = cc.parse_folded(["a;b 1\n", "\n", "broken\n", "c 2\n"])
        self.assertEqual([e.lineno for e in entries], [1, 4])
        self.assertEqual(len(errors), 1)
        self.assertIn("line 3", errors[0])


class SegmentTest(unittest.TestCase):
    def test_split_only_on_exact_double_dash(self):
        frames = ["t", "-", "k--x", "---", "--", "w"]
        self.assertEqual(cc.segments(frames), [["t", "-", "k--x", "---"], ["w"]])

    def test_exact_chain_structure(self):
        frames, _ = cc.parse_line(sink_line())
        self.assertEqual(len(cc.segments(frames)), 5)
        self.assertEqual(cc.target_label(frames), "cg_sink")
        self.assertEqual(cc.waker_labels(frames), FULL)
        self.assertFalse(cc.has_marker(frames, cc.MARKER_TRUNCATED))
        self.assertFalse(cc.has_marker(frames, cc.MARKER_UNKNOWN))

    def test_functions_accept_segments(self):
        frames, _ = cc.parse_line(sink_line())
        segs = cc.segments(frames)
        self.assertEqual(cc.target_label(segs), "cg_sink")
        self.assertEqual(cc.waker_labels(segs), FULL)

    def test_truncated_marker(self):
        frames, _ = cc.parse_line(line(target_seg("cg_sink"),
                                       [waker_seg("cg_relay2"), waker_seg("cg_relay1")],
                                       cc.MARKER_TRUNCATED))
        self.assertEqual(cc.waker_labels(frames), ["cg_relay2", "cg_relay1"])
        self.assertTrue(cc.has_marker(frames, cc.MARKER_TRUNCATED))
        self.assertEqual(cc.segment_frames(frames, 3), [])

    def test_unknown_waker(self):
        frames, _ = cc.parse_line(line(target_seg("cg_sink"), [], cc.MARKER_UNKNOWN))
        self.assertEqual(cc.target_label(frames), "cg_sink")
        self.assertEqual(cc.waker_labels(frames), [])
        self.assertTrue(cc.has_marker(frames, cc.MARKER_UNKNOWN))
        self.assertFalse(cc.has_marker(frames, cc.MARKER_TRUNCATED))

    def test_user_kernel_separator(self):
        frames, _ = cc.parse_line(sink_line())
        seg0 = cc.segment_frames(frames, 0)
        self.assertNotIn("-", seg0)
        self.assertNotIn("cg_sink", seg0)          # the label is not a frame
        self.assertEqual(seg0[0], "_start")
        self.assertEqual(seg0[-1], "__schedule")
        self.assertIn("cg_stage_sink", seg0)
        seg1 = cc.segment_frames(frames, 1)
        self.assertEqual(seg1[0], "try_to_wake_up")
        self.assertEqual(seg1[-1], "_start")
        self.assertNotIn("cg_relay2", seg1)
        # hardirq waker: kernel frames only, no separator
        self.assertEqual(cc.segment_frames(frames, 4)[1], "hrtimer_wakeup")
        self.assertEqual(cc.segment_frames(frames, 9), [])

    def test_kernel_thread_without_user_frames(self):
        frames, _ = cc.parse_line(line(target_seg("kworker/u16:0", user=None),
                                       [waker_seg("[softirq]", user=None)]))
        self.assertEqual(cc.segment_frames(frames, 0), SINK_KERNEL.split(";"))
        self.assertEqual(cc.waker_labels(frames), ["[softirq]"])
        self.assertEqual(cc.validate_frames(frames), [])

    def test_label_only_segments(self):
        frames, _ = cc.parse_line("cg_sink;--;cg_relay2;--;[hardirq] 5")
        self.assertEqual(cc.target_label(frames), "cg_sink")
        self.assertEqual(cc.waker_labels(frames), ["cg_relay2", "[hardirq]"])
        self.assertEqual(cc.segment_frames(frames, 1), [])
        self.assertEqual(cc.validate_frames(frames), [])


class LabelTest(unittest.TestCase):
    def test_strip_tid(self):
        cases = {
            "cg_sink/1234": "cg_sink",
            "kworker/u16:0/77": "kworker/u16:0",
            "ksoftirqd/3/25": "ksoftirqd/3",
            "cg_relay1/55 [softirq]": "cg_relay1 [softirq]",
            "bash/900 [fork]": "bash [fork]",
            "my app/12": "my app",
            "[hardirq]": "[hardirq]",
            "[softirq]": "[softirq]",
            "cg_sink": "cg_sink",
        }
        for label, want in cases.items():
            with self.subTest(label=label):
                self.assertEqual(cc.strip_tid(label), want)

    def test_per_thread_labels(self):
        frames, _ = cc.parse_line(sink_line(label="cg_sink/101", relay2="cg_relay2/102",
                                            relay1="cg_relay1/103", source="cg_source/104"))
        self.assertEqual(cc.target_label(frames), "cg_sink/101")
        self.assertEqual(cc.target_label(frames, per_thread=True), "cg_sink")
        self.assertEqual(cc.waker_labels(frames, per_thread=True), FULL)

    def test_slash_comm_not_stripped_without_per_thread(self):
        frames, _ = cc.parse_line(line(target_seg("ksoftirqd/3", user=None),
                                       [waker_seg("kworker/u16:0", user=None)]))
        self.assertEqual(cc.target_label(frames), "ksoftirqd/3")
        self.assertEqual(cc.waker_labels(frames), ["kworker/u16:0"])

    def test_label_with_spaces(self):
        text = line(target_seg("cg_sink"), [waker_seg("cg_relay2"),
                                            waker_seg("cg_relay1 [softirq]"),
                                            waker_seg("[softirq]", user=None)], us=321)
        frames, us = cc.parse_line(text)
        self.assertEqual(us, 321)
        self.assertEqual(cc.waker_labels(frames),
                         ["cg_relay2", "cg_relay1 [softirq]", "[softirq]"])


class CheckChainTest(unittest.TestCase):
    def test_exact_match_sums_lines(self):
        entries = entries_of(sink_line(us=1000), sink_line(us=2500))
        ok, report = cc.check_chain(entries, "cg_sink", FULL, min_us=3500)
        self.assertTrue(ok, "\n".join(report))
        self.assertIn("3,500 us", report[0])

    def test_match_is_exact(self):
        longer = line(target_seg("cg_sink"),
                      [waker_seg(w) for w in FULL + ["swapper/0"]], us=10**7)
        shorter = line(target_seg("cg_sink"),
                       [waker_seg(w) for w in FULL[:3]], us=10**7)
        other_target = sink_line(label="cg_relay2", us=10**7)
        entries = entries_of(sink_line(us=100), longer, shorter, other_target)
        ok, report = cc.check_chain(entries, "cg_sink", FULL, min_us=100)
        self.assertTrue(ok, "\n".join(report))
        ok, report = cc.check_chain(entries, "cg_sink", FULL, min_us=101)
        self.assertFalse(ok)
        self.assertIn("below --min-us", "\n".join(report))

    def test_no_match(self):
        ok, report = cc.check_chain(entries_of(sink_line()), "cg_sink", ["cg_relay1"])
        self.assertFalse(ok)
        self.assertIn("no line matches chain: cg_sink <- cg_relay1", "\n".join(report))

    def test_truncated_marker_only_when_requested(self):
        trunc = line(target_seg("cg_sink"), [waker_seg("cg_relay2"), waker_seg("cg_relay1")],
                     cc.MARKER_TRUNCATED, us=5000)
        plain = line(target_seg("cg_sink"), [waker_seg("cg_relay2"), waker_seg("cg_relay1")],
                     us=7)
        entries = entries_of(trunc, plain)
        ok, report = cc.check_chain(entries, "cg_sink", ["cg_relay2", "cg_relay1"],
                                    truncated=True, min_us=5000)
        self.assertTrue(ok, "\n".join(report))
        ok, _ = cc.check_chain(entries, "cg_sink", ["cg_relay2", "cg_relay1"],
                               truncated=True, min_us=5001)
        self.assertFalse(ok)
        # without --truncated only the unmarked line counts
        ok, _ = cc.check_chain(entries, "cg_sink", ["cg_relay2", "cg_relay1"], min_us=7)
        self.assertTrue(ok)
        ok, _ = cc.check_chain(entries, "cg_sink", ["cg_relay2", "cg_relay1"], min_us=8)
        self.assertFalse(ok)

    def test_unknown_waker(self):
        entries = entries_of(line(target_seg("cg_sink"), [], cc.MARKER_UNKNOWN, us=900),
                             sink_line(us=50))
        ok, report = cc.check_chain(entries, "cg_sink", [], unknown_waker=True, min_us=900)
        self.assertTrue(ok, "\n".join(report))
        ok, _ = cc.check_chain(entries, "cg_sink", [], min_us=1)
        self.assertFalse(ok)

    def test_frame_regex(self):
        entries = entries_of(sink_line(us=4000))
        regexes = ["0:pipe_read|pipe_wait|anon_pipe_read", "0:cg_stage_sink",
                   "1:pipe_write", "1:cg_stage_relay", "3:cg_stage_source",
                   "4:hrtimer_wakeup"]
        ok, report = cc.check_chain(entries, "cg_sink", FULL, min_us=4000,
                                    frame_regexes=regexes)
        self.assertTrue(ok, "\n".join(report))

    def test_frame_regex_failures(self):
        entries = entries_of(sink_line(us=4000))
        for spec in ("1:cg_stage_sink",     # wrong segment
                     "0:^cg_sink$",         # labels are not frames
                     "4:cg_stage_source",   # hardirq waker has no user frames
                     "0:^-$",               # nor is the "-" separator
                     "7:.*"):               # no such segment
            with self.subTest(spec=spec):
                ok, report = cc.check_chain(entries, "cg_sink", FULL,
                                            frame_regexes=[spec])
                self.assertFalse(ok)
                text = "\n".join(report)
                self.assertIn("no frame in segment %s" % spec.split(":")[0], text)
                self.assertIn("segment frames of the heaviest matching lines", text)

    def test_frame_regex_any_matching_line(self):
        other = line(target_seg("cg_sink", user="[missing user stack]"),
                     [waker_seg(w, user="[unknown]") for w in FULL], us=10)
        entries = entries_of(other, sink_line(us=20))
        ok, report = cc.check_chain(entries, "cg_sink", FULL,
                                    frame_regexes=["0:cg_stage_sink", "2:^\\[unknown\\]$"])
        self.assertTrue(ok, "\n".join(report))

    def test_parse_frame_regex(self):
        seg, rx = cc.parse_frame_regex("2:a:b|c")
        self.assertEqual(seg, 2)
        self.assertEqual(rx.pattern, "a:b|c")
        for bad in ("x:foo", "1", "1:", ":foo", "1:("):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                cc.parse_frame_regex(bad)

    def test_per_thread(self):
        entries = entries_of(
            sink_line(label="cg_sink/11", relay2="cg_relay2/12", relay1="cg_relay1/13",
                      source="cg_source/14", us=600),
            sink_line(label="cg_sink/21", relay2="cg_relay2/22", relay1="cg_relay1/23",
                      source="cg_source/24", us=400))
        ok, _ = cc.check_chain(entries, "cg_sink", FULL, min_us=1)
        self.assertFalse(ok)
        ok, report = cc.check_chain(entries, "cg_sink", FULL, min_us=1000, per_thread=True)
        self.assertTrue(ok, "\n".join(report))

    def test_softirq_label_with_space(self):
        wakers = ["cg_relay2", "cg_relay1 [softirq]", "[softirq]"]
        text = line(target_seg("cg_sink"),
                    [waker_seg(wakers[0]), waker_seg(wakers[1]),
                     waker_seg(wakers[2], user=None)], us=77)
        ok, report = cc.check_chain(entries_of(text), "cg_sink", wakers, min_us=77)
        self.assertTrue(ok, "\n".join(report))
        ok, _ = cc.check_chain(entries_of(text), "cg_sink",
                               ["cg_relay2", "cg_relay1", "[softirq]"])
        self.assertFalse(ok)

    def test_slash_comms(self):
        text = line(target_seg("kworker/u16:0", user=None),
                    [waker_seg("ksoftirqd/3", user=None), waker_seg("[hardirq]", user=None)],
                    us=12)
        entries = entries_of(text)
        ok, report = cc.check_chain(entries, "kworker/u16:0", ["ksoftirqd/3", "[hardirq]"])
        self.assertTrue(ok, "\n".join(report))
        pt = line(target_seg("kworker/u16:0/99", user=None),
                  [waker_seg("ksoftirqd/3/25", user=None), waker_seg("[hardirq]", user=None)],
                  us=12)
        ok, report = cc.check_chain(entries_of(pt), "kworker/u16:0",
                                    ["ksoftirqd/3", "[hardirq]"], per_thread=True)
        self.assertTrue(ok, "\n".join(report))

    def test_failure_diagnostics(self):
        # the sink's chain lost its source: the best partial match is shown first
        partial = line(target_seg("cg_sink"),
                       [waker_seg("cg_relay2"), waker_seg("cg_relay1")], us=50)
        unrelated = [sink_line(label="task%d" % i, us=1000 + i) for i in range(8)]
        entries = entries_of(partial, *unrelated)
        ok, report = cc.check_chain(entries, "cg_sink", FULL, min_us=10)
        self.assertFalse(ok)
        text = "\n".join(report)
        self.assertTrue(report[0].startswith("FAILED chain check for: "
                                             "cg_sink <- cg_relay2 <- cg_relay1"))
        self.assertIn("chains observed for target cg_sink", text)
        self.assertIn("best partially matching lines:", text)
        best = text.split("best partially matching lines:")[1]
        self.assertEqual(best.count("\n  line "), 5)          # top 5 only
        first = best.split("\n  line ")[1]
        self.assertTrue(first.startswith("1, 50 us"), first)
        self.assertIn("target ok, 2/4 wakers match", first)
        self.assertIn("seg 0 (target): cg_sink;_start", first)
        # among non-matching targets, the heaviest come first
        self.assertIn("line 9, 1,007 us", best)

    def test_failure_diagnostics_no_target(self):
        entries = entries_of(sink_line(label="bash", us=5))
        ok, report = cc.check_chain(entries, "cg_sink", FULL)
        self.assertFalse(ok)
        text = "\n".join(report)
        self.assertIn("no line has target 'cg_sink'", text)
        self.assertIn("target differs", text)

    def test_parse_errors_fail(self):
        entries, errors = cc.parse_folded([sink_line(us=10) + "\n", "garbage\n"])
        ok, report = cc.check_chain(entries, "cg_sink", FULL, parse_errors=errors)
        self.assertFalse(ok)
        self.assertIn("malformed lines (1)", "\n".join(report))


class OnlyTargetTest(unittest.TestCase):
    def test_ok(self):
        ok, report = cc.check_only_target(entries_of(sink_line(), sink_line(us=3)), "cg_sink")
        self.assertTrue(ok, "\n".join(report))

    def test_other_target(self):
        entries = entries_of(sink_line(), sink_line(label="kworker/u16:0", us=99))
        ok, report = cc.check_only_target(entries, "cg_sink")
        self.assertFalse(ok)
        text = "\n".join(report)
        self.assertIn("1 of 2 line(s)", text)
        self.assertIn("target kworker/u16:0", text)
        self.assertIn("line 2, 99 us", text)

    def test_per_thread(self):
        entries = entries_of(sink_line(label="cg_sink/5"), sink_line(label="cg_sink/6"))
        self.assertFalse(cc.check_only_target(entries, "cg_sink")[0])
        self.assertTrue(cc.check_only_target(entries, "cg_sink", per_thread=True)[0])

    def test_empty_input_fails(self):
        ok, report = cc.check_only_target([], "cg_sink")
        self.assertFalse(ok)
        self.assertIn("no folded lines", "\n".join(report))


class FormatTest(unittest.TestCase):
    def test_valid(self):
        lines = [
            line(target_seg("cg_sink"), [], cc.MARKER_UNKNOWN, us=1),
            line(target_seg("cg_sink"), [waker_seg("cg_relay2")], cc.MARKER_TRUNCATED, us=2),
            sink_line(us=3),
            line(target_seg("kworker/u16:0", user=None),
                 [waker_seg("[hardirq]", user=None)], us=3),
        ]
        ok, report = cc.check_format(entries_of(*lines))
        self.assertTrue(ok, "\n".join(report))

    def test_invalid(self):
        cases = {
            "cg_sink;--;--;[hardirq] 1": "segment 1 is empty",
            "cg_sink;--;;--;[hardirq] 1": "segment 1 has an empty frame",
            ";--;w 1": "target segment has an empty frame",
            "cg_sink;-;x;--;w 1": "'-' without frames on both sides",
            "cg_sink;a;-;b;-;c;--;w 1": "2 '-' separators",
            "cg_sink;--;x;-;w 1": "'-' without frames on both sides",
            "cg_sink;--;w;--;[unknown waker] 1": "must directly follow",
            "cg_sink;--;[chain truncated] 1": "without any waker",
            "cg_sink;--;[chain truncated];--;w 1": "only valid as the last segment",
            "cg_sink;--;k;- 1": "invalid label '-'",
        }
        for text, want in cases.items():
            with self.subTest(text=text):
                ok, report = cc.check_format(entries_of(text))
                self.assertFalse(ok)
                self.assertIn(want, "\n".join(report))

    def test_sorted_ascending(self):
        entries = entries_of(sink_line(us=5), sink_line(us=4))
        ok, report = cc.check_format(entries)
        self.assertFalse(ok)
        self.assertIn("sorted ascending", "\n".join(report))
        self.assertTrue(cc.check_format(entries, sort_check=False)[0])


class CliTest(unittest.TestCase):
    TEXT = "\n".join([
        line(target_seg("cg_sink"), [waker_seg("cg_relay2"), waker_seg("cg_relay1")],
             cc.MARKER_TRUNCATED, us=1_000_000),
        sink_line(us=3_500_000),
    ]) + "\n"

    def test_chain_pass(self):
        rc, out = run_cli(["chain", "--target", "cg_sink",
                           "--wakers", "cg_relay2,cg_relay1,cg_source,[hardirq]",
                           "--min-us", "3000000",
                           "--frame-regex", "0:pipe_read|pipe_wait|anon_pipe_read",
                           "--frame-regex", "4:hrtimer_wakeup"], self.TEXT)
        self.assertEqual(rc, 0, out)
        self.assertTrue(out.startswith("ok: cg_sink <- cg_relay2"), out)

    def test_chain_truncated_pass(self):
        rc, out = run_cli(["chain", "--target", "cg_sink", "--wakers", "cg_relay2,cg_relay1",
                           "--truncated", "--min-us", "1000000"], self.TEXT)
        self.assertEqual(rc, 0, out)

    def test_chain_fail_prints_diagnostics(self):
        rc, out = run_cli(["chain", "--target", "cg_sink", "--wakers", "cg_relay2,cg_relay1",
                           "--min-us", "1"], self.TEXT)
        self.assertEqual(rc, 1)
        self.assertIn("FAILED chain check", out)
        self.assertIn("best partially matching lines:", out)
        self.assertIn("marker differs ([chain truncated])", out)

    def test_waker_label_with_space(self):
        text = line(target_seg("cg_sink"), [waker_seg("cg_relay1 [softirq]")], us=10) + "\n"
        rc, out = run_cli(["chain", "--target", "cg_sink", "--wakers", "cg_relay1 [softirq]"],
                          text)
        self.assertEqual(rc, 0, out)

    def test_unknown_waker_cli(self):
        text = line(target_seg("cg_sink"), [], cc.MARKER_UNKNOWN, us=10) + "\n"
        rc, out = run_cli(["chain", "--target", "cg_sink", "--unknown-waker"], text)
        self.assertEqual(rc, 0, out)

    def test_only_target_and_format(self):
        rc, out = run_cli(["only-target", "--target", "cg_sink"], self.TEXT)
        self.assertEqual(rc, 0, out)
        rc, out = run_cli(["only-target", "--target", "cg_relay2"], self.TEXT)
        self.assertEqual(rc, 1, out)
        rc, out = run_cli(["format"], self.TEXT)
        self.assertEqual(rc, 0, out)

    def test_summary(self):
        rc, out = run_cli(["summary", "--target", "cg_sink"], self.TEXT)
        self.assertEqual(rc, 0)
        self.assertIn("3,500,000 us", out)
        self.assertIn("cg_sink <- cg_relay2 <- cg_relay1 <- [chain truncated]", out)

    def test_usage_errors(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as cm:
                run_cli(["chain", "--target", "cg_sink", "--frame-regex", "x"], self.TEXT)
            self.assertEqual(cm.exception.code, 2)
            with self.assertRaises(SystemExit) as cm:
                run_cli(["chain", "--target", "cg_sink", "--wakers", "a",
                         "--unknown-waker"], self.TEXT)
            self.assertEqual(cm.exception.code, 2)
            rc = cc.main(["format", "/nonexistent/chaingraph.folded"])
        self.assertEqual(rc, 2)


class InterruptsTest(unittest.TestCase):
    def test_trimmed_interrupt_links_pass(self):
        ok, report = cc.check_interrupts(entries_of(
            sink_line(),
            line(target_seg("pg"), [waker_seg("[softirq]", kernel=SOFTIRQ_KERNEL, user=None)])))
        self.assertTrue(ok, report)
        self.assertIn("2 interrupt link(s)", report[0])

    def test_untrimmed_interrupt_link_fails(self):
        bad = line(target_seg("cg_source"),
                   [waker_seg("[hardirq]", kernel=HARDIRQ_KERNEL + ";" + IDLE_TAIL, user=None)],
                   us=777)
        ok, report = cc.check_interrupts(entries_of(sink_line(), bad))
        self.assertFalse(ok)
        text = "\n".join(report)
        self.assertIn("not trimmed", text)
        self.assertIn("do_idle", text)

    def test_interrupt_without_entry_frame_is_fine(self):
        # e.g. kernel stack missing, or an arch whose entry isn't recognised
        ok, report = cc.check_interrupts(entries_of(
            line(target_seg("t"), [waker_seg("[hardirq]", kernel="[missing kernel stack]",
                                             user=None)])))
        self.assertTrue(ok, report)

    def test_idle_label_through_irq_exit_fails(self):
        for label in ("swapper/3 [softirq]", "swapper/0"):
            bad = line(target_seg("pg"), [waker_seg(label, kernel=SOFTIRQ_KERNEL + ";" + IDLE_TAIL,
                                                    user=None)])
            ok, report = cc.check_interrupts(entries_of(bad))
            self.assertFalse(ok, label)
            self.assertIn("idle task", "\n".join(report))

    def test_idle_process_context_waker_is_fine(self):
        idle = "try_to_wake_up;flush_smp_call_function_queue;do_idle;cpu_startup_entry"
        ok, report = cc.check_interrupts(entries_of(
            line(target_seg("kworker/1:1"), [waker_seg("swapper/1", kernel=idle, user=None)])))
        self.assertTrue(ok, report)

    def test_cli(self):
        rc, out = run_cli(["interrupts"], sink_line() + "\n")
        self.assertEqual(rc, 0, out)

    def test_thaw_suffix_per_thread(self):
        self.assertEqual(cc.strip_tid("sshd/77 [thaw]"), "sshd [thaw]")


if __name__ == "__main__":
    unittest.main()
