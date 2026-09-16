#!/usr/bin/env python3
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
"""Assertions over chaingraph folded output.

Each folded line is "<FRAMES> <MICROSECONDS>", FRAMES joined by ';'. The
frame "--" separates segments:

  segment 0      <target label>[;user frames root->leaf][;-][;kernel frames root->leaf]
  segment 1..N   [kernel frames leaf->root][;-][;user frames leaf->root];<waker label>
  optional last  [chain truncated]   (depth limit hit)
             or  [unknown waker]     (directly after segment 0)

Segment 1 is the direct waker of the target, segment 2 woke segment 1, ...

Subcommands (exit status 0 = pass, 1 = check failed, 2 = usage/IO error):

  chain FILE --target T --wakers W1,W2,... [--truncated | --unknown-waker]
             [--min-us N] [--frame-regex SEG:REGEX ...] [--per-thread]
      Sum the time of lines whose target label and waker label sequence are
      exactly T and W1,W2,... (plus the trailing marker only if requested).
      Fail if nothing matched, the sum is below --min-us, or some
      --frame-regex has no stack frame in segment SEG matching REGEX
      (re.search) in any matching line. Labels and the "-" separator are not
      stack frames.

  only-target FILE --target T [--per-thread]
      Every line's target label is T (and there is at least one line).

  format FILE [--no-sort-check]
      Every line follows the grammar above; times are ascending.

  summary FILE [--target T] [--per-thread] [--top N]
      Print the heaviest chains, for debugging.

With --per-thread, a "/TID" suffix is stripped from task labels (only one
trailing "/<digits>" per label, so "kworker/u16:0/123" -> "kworker/u16:0").
FILE may be "-" for stdin.
"""

import argparse
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Iterable, List, Optional, Sequence, Tuple

SEGMENT_SEP = "--"
USER_KERNEL_SEP = "-"
MARKER_TRUNCATED = "[chain truncated]"
MARKER_UNKNOWN = "[unknown waker]"
MARKER_TORN = "[chain torn]"
MARKERS = (MARKER_TRUNCATED, MARKER_UNKNOWN, MARKER_TORN)

# Suffixes chaingraph appends to waker task labels.
LABEL_SUFFIXES = (" [softirq]", " [fork]", " [thaw]")

_TID_RE = re.compile(r"^(?P<base>.+)/\d+$")

DIAG_TOP = 5
SEGMENT_PRINT_MAX = 600


# --------------------------------------------------------------------------
# parsing primitives


def parse_line(line: str) -> Optional[Tuple[List[str], int]]:
    """Split a folded line into (frames, microseconds).

    Returns None for blank lines. Raises ValueError for a line without a
    numeric count after its last space.
    """
    line = line.rstrip("\r\n")
    if not line.strip():
        return None
    stack, sep, count = line.rpartition(" ")
    if not sep or not stack:
        raise ValueError("no ' <count>' at end of line")
    if not re.fullmatch(r"\d+", count):
        raise ValueError("count %r is not a non-negative integer" % count)
    return stack.split(";"), int(count)


def segments(frames: Sequence[str]) -> List[List[str]]:
    """Split frames into segments on the exact frame "--"."""
    segs: List[List[str]] = [[]]
    for frame in frames:
        if frame == SEGMENT_SEP:
            segs.append([])
        else:
            segs[-1].append(frame)
    return segs


def _segs(frames_or_segs) -> List[List[str]]:
    if frames_or_segs and isinstance(frames_or_segs[0], list):
        return list(frames_or_segs)
    return segments(frames_or_segs)


def strip_tid(label: str) -> str:
    """Remove a per-thread "/TID" from a task label, keeping suffixes."""
    suffix = ""
    stripped = True
    while stripped:
        stripped = False
        for s in LABEL_SUFFIXES:
            if label.endswith(s) and len(label) > len(s):
                label = label[: -len(s)]
                suffix = s + suffix
                stripped = True
    m = _TID_RE.match(label)
    if m:
        label = m.group("base")
    return label + suffix


def _norm(label: str, per_thread: bool) -> str:
    return strip_tid(label) if per_thread else label


def _marker_of(segs: List[List[str]]) -> Optional[str]:
    if len(segs) >= 2 and len(segs[-1]) == 1 and segs[-1][0] in MARKERS:
        return segs[-1][0]
    return None


def target_label(frames, per_thread: bool = False) -> str:
    """Label of the blocked task (first frame of segment 0)."""
    segs = _segs(frames)
    return _norm(segs[0][0], per_thread) if segs[0] else ""


def waker_labels(frames, per_thread: bool = False) -> List[str]:
    """Waker labels, direct waker first; a trailing marker is not a waker."""
    segs = _segs(frames)
    end = len(segs) - 1 if _marker_of(segs) else len(segs)
    return [_norm(seg[-1], per_thread) if seg else "" for seg in segs[1:end]]


def has_marker(frames, marker: str) -> bool:
    """True if the line ends with the marker segment (e.g. MARKER_TRUNCATED)."""
    return _marker_of(_segs(frames)) == marker


def segment_frames(frames, index: int) -> List[str]:
    """Stack frames of segment @index, without its label and "-" separator.

    Segment 0 is the target (label first), segments >= 1 are wakers (label
    last). Marker segments and missing segments have no frames.
    """
    segs = _segs(frames)
    if index < 0 or index >= len(segs):
        return []
    seg = segs[index]
    if index == 0:
        body = seg[1:]
    elif index == len(segs) - 1 and _marker_of(segs):
        body = []
    else:
        body = seg[:-1]
    return [f for f in body if f != USER_KERNEL_SEP]


def validate_frames(frames: Sequence[str]) -> List[str]:
    """Return a list of grammar problems (empty if the frames are valid)."""
    problems = []
    segs = segments(frames)
    marker = _marker_of(segs)
    for i, seg in enumerate(segs):
        what = "target segment" if i == 0 else "segment %d" % i
        if not seg:
            problems.append("%s is empty" % what)
            continue
        if any(f == "" for f in seg):
            problems.append("%s has an empty frame" % what)
        if i == len(segs) - 1 and marker:
            if marker == MARKER_UNKNOWN and i != 1:
                problems.append("%s must directly follow the target segment"
                                % MARKER_UNKNOWN)
            if marker == MARKER_TRUNCATED and i < 2:
                problems.append("%s without any waker" % MARKER_TRUNCATED)
            continue
        label = seg[0] if i == 0 else seg[-1]
        body = seg[1:] if i == 0 else seg[:-1]
        misplaced = [m for m in MARKERS if m in seg]
        for m in misplaced:
            problems.append("%s: %s is only valid as the last segment"
                            % (what, m))
        if label == USER_KERNEL_SEP and not misplaced:
            problems.append("%s has invalid label %r" % (what, label))
        seps = [j for j, f in enumerate(body) if f == USER_KERNEL_SEP]
        if len(seps) > 1:
            problems.append("%s has %d '-' separators" % (what, len(seps)))
        elif seps and (seps[0] == 0 or seps[0] == len(body) - 1):
            problems.append("%s has '-' without frames on both sides" % what)
    return problems


# --------------------------------------------------------------------------
# folded files


@dataclass
class Entry:
    lineno: int
    frames: List[str]
    us: int
    raw: str
    segs: List[List[str]] = field(default_factory=list)

    def __post_init__(self):
        if not self.segs:
            self.segs = segments(self.frames)

    def target(self, per_thread=False) -> str:
        return target_label(self.segs, per_thread)

    def wakers(self, per_thread=False) -> List[str]:
        return waker_labels(self.segs, per_thread)

    @property
    def marker(self) -> Optional[str]:
        return _marker_of(self.segs)

    def describe(self, per_thread=False) -> str:
        parts = [self.target(per_thread)] + self.wakers(per_thread)
        if self.marker:
            parts.append(self.marker)
        return " <- ".join(repr(p) if p == "" else p for p in parts)


def parse_folded(lines: Iterable[str]) -> Tuple[List[Entry], List[str]]:
    """Parse folded lines into entries; returns (entries, errors)."""
    entries, errors = [], []
    for lineno, line in enumerate(lines, 1):
        try:
            parsed = parse_line(line)
        except ValueError as e:
            errors.append("line %d: %s: %s" % (lineno, e, _clip(line.rstrip("\r\n"))))
            continue
        if parsed is None:
            continue
        frames, us = parsed
        entries.append(Entry(lineno, frames, us, line.rstrip("\r\n")))
    return entries, errors


def read_folded(path: str) -> Tuple[List[Entry], List[str]]:
    if path == "-":
        return parse_folded(sys.stdin)
    with open(path, encoding="utf-8", errors="replace") as f:
        return parse_folded(f)


# --------------------------------------------------------------------------
# reporting helpers


def _clip(text: str, limit: int = SEGMENT_PRINT_MAX) -> str:
    if len(text) <= limit:
        return text
    half = limit // 2 - 3
    return "%s ... %s" % (text[:half], text[-half:])


def _us(us: int) -> str:
    return "{:,} us ({:.3f} s)".format(us, us / 1e6)


def _format_entry(e: Entry, per_thread: bool, note: str = "") -> List[str]:
    out = ["  line %d, %s%s" % (e.lineno, _us(e.us), note),
           "    chain: %s" % e.describe(per_thread)]
    for i, seg in enumerate(e.segs):
        name = "target" if i == 0 else ("marker" if i == len(e.segs) - 1 and e.marker
                                        else "waker %d" % i)
        out.append("    seg %d (%s): %s" % (i, name, _clip(";".join(seg))))
    return out


def _chain_histogram(entries: List[Entry], per_thread: bool, top: int) -> List[str]:
    agg = defaultdict(lambda: [0, 0])
    for e in entries:
        a = agg[e.describe(per_thread)]
        a[0] += e.us
        a[1] += 1
    rows = sorted(agg.items(), key=lambda kv: -kv[1][0])[:top]
    return ["  %14s us  %4d line(s)  %s" % ("{:,}".format(us), n, chain)
            for chain, (us, n) in rows]


def _common_prefix(a: Sequence[str], b: Sequence[str]) -> int:
    n = 0
    for x, y in zip(a, b):
        if x != y:
            break
        n += 1
    return n


# --------------------------------------------------------------------------
# checks; each returns (ok, report lines)


def parse_frame_regex(spec: str) -> Tuple[int, "re.Pattern"]:
    seg, sep, regex = spec.partition(":")
    if not sep or not re.fullmatch(r"\d+", seg) or regex == "":
        raise ValueError("expected SEG:REGEX, got %r" % spec)
    try:
        return int(seg), re.compile(regex)
    except re.error as e:
        raise ValueError("bad regex in %r: %s" % (spec, e))


def check_chain(entries: List[Entry], target: str, wakers: Sequence[str],
                truncated: bool = False, unknown_waker: bool = False,
                min_us: int = 1, frame_regexes: Sequence[str] = (),
                per_thread: bool = False,
                parse_errors: Sequence[str] = ()) -> Tuple[bool, List[str]]:
    wakers = list(wakers)
    want_marker = (MARKER_TRUNCATED if truncated else
                   MARKER_UNKNOWN if unknown_waker else None)
    want = " <- ".join([target] + wakers + ([want_marker] if want_marker else []))
    regexes = [parse_frame_regex(s) + (s,) for s in frame_regexes]

    def matches(e: Entry) -> bool:
        return (e.target(per_thread) == target and
                e.wakers(per_thread) == wakers and e.marker == want_marker)

    matched = [e for e in entries if matches(e)]
    total = sum(e.us for e in matched)
    report, ok = [], True

    if parse_errors:
        ok = False
        report.append("malformed lines (%d):" % len(parse_errors))
        report += ["  " + err for err in parse_errors[:DIAG_TOP]]

    if not matched:
        ok = False
        report.append("no line matches chain: %s" % want)
    elif total < min_us:
        ok = False
        report.append("chain %s: %s in %d line(s), below --min-us %s"
                      % (want, _us(total), len(matched), "{:,}".format(min_us)))

    regex_fail = []
    for seg, rx, spec in regexes:
        if not any(any(rx.search(f) for f in segment_frames(e.segs, seg))
                   for e in matched):
            regex_fail.append((seg, rx, spec))
    if matched and regex_fail:
        ok = False
        for seg, rx, spec in regex_fail:
            report.append("no frame in segment %d matches /%s/ in any of the %d "
                          "matching line(s)" % (seg, rx.pattern, len(matched)))
        report.append("segment frames of the heaviest matching lines:")
        segs_needed = sorted({seg for seg, _, _ in regex_fail})
        for e in sorted(matched, key=lambda e: -e.us)[:DIAG_TOP]:
            report.append("  line %d, %s" % (e.lineno, _us(e.us)))
            for seg in segs_needed:
                frames = segment_frames(e.segs, seg)
                report.append("    seg %d: %s" % (
                    seg, _clip(";".join(frames)) if frames else "(no stack frames)"))

    if ok:
        report.insert(0, "ok: %s: %s in %d line(s) (min %s)%s" % (
            want, _us(total), len(matched), "{:,}".format(min_us),
            "; frames ok: " + ", ".join(s for _, _, s in regexes) if regexes else ""))
        return True, report

    report.insert(0, "FAILED chain check for: %s" % want)
    report.append("input: %d line(s), %s total" % (len(entries), _us(sum(e.us for e in entries))))
    same_target = [e for e in entries if e.target(per_thread) == target]
    if same_target:
        report.append("chains observed for target %s (heaviest first):" % target)
        report += _chain_histogram(same_target, per_thread, DIAG_TOP)
    else:
        report.append("no line has target %r; heaviest chains overall:" % target)
        report += _chain_histogram(entries, per_thread, DIAG_TOP)

    def score(e: Entry):
        return (e.target(per_thread) == target,
                _common_prefix(e.wakers(per_thread), wakers),
                e.wakers(per_thread) == wakers,
                e.marker == want_marker,
                e.us)

    best = sorted(entries, key=score, reverse=True)[:DIAG_TOP]
    if best:
        report.append("best partially matching lines:")
        for e in best:
            t_ok, prefix, all_ok, m_ok, _ = score(e)
            note = " [target %s, %d/%d wakers match%s, marker %s]" % (
                "ok" if t_ok else "differs", prefix, len(wakers),
                "" if all_ok or prefix < len(wakers) else " but extra wakers",
                "ok" if m_ok else "differs (%s)" % (e.marker or "none"))
            report += _format_entry(e, per_thread, note)
    return False, report


def check_only_target(entries: List[Entry], target: str, per_thread: bool = False,
                      parse_errors: Sequence[str] = ()) -> Tuple[bool, List[str]]:
    report = []
    ok = True
    if parse_errors:
        ok = False
        report.append("malformed lines (%d):" % len(parse_errors))
        report += ["  " + err for err in parse_errors[:DIAG_TOP]]
    if not entries:
        report.append("no folded lines in input")
        return False, ["FAILED only-target %s" % target] + report
    others = [e for e in entries if e.target(per_thread) != target]
    if others:
        ok = False
        report.append("%d of %d line(s) (%s) have a target other than %s:"
                      % (len(others), len(entries), _us(sum(e.us for e in others)), target))
        agg = defaultdict(lambda: [0, 0])
        for e in others:
            agg[e.target(per_thread)][0] += e.us
            agg[e.target(per_thread)][1] += 1
        for label, (us, n) in sorted(agg.items(), key=lambda kv: -kv[1][0])[:DIAG_TOP]:
            report.append("  %14s us  %4d line(s)  target %s" % ("{:,}".format(us), n, label))
        report.append("heaviest offending lines:")
        for e in sorted(others, key=lambda e: -e.us)[:DIAG_TOP]:
            report += _format_entry(e, per_thread)
    if ok:
        return True, ["ok: all %d line(s) have target %s (%s)"
                      % (len(entries), target, _us(sum(e.us for e in entries)))]
    return False, ["FAILED only-target %s" % target] + report


def check_format(entries: List[Entry], parse_errors: Sequence[str] = (),
                 sort_check: bool = True) -> Tuple[bool, List[str]]:
    problems = list(parse_errors)
    for e in entries:
        for p in validate_frames(e.frames):
            problems.append("line %d: %s: %s" % (e.lineno, p, _clip(e.raw, 300)))
    if sort_check:
        for prev, cur in zip(entries, entries[1:]):
            if cur.us < prev.us:
                problems.append("line %d: time %d is less than line %d's %d "
                                "(output must be sorted ascending)"
                                % (cur.lineno, cur.us, prev.lineno, prev.us))
                break
    if not entries and not parse_errors:
        problems.append("no folded lines in input")
    if problems:
        report = ["FAILED format: %d problem(s)" % len(problems)]
        report += ["  " + p for p in problems[:20]]
        if len(problems) > 20:
            report.append("  ... %d more" % (len(problems) - 20))
        return False, report
    return True, ["ok: %d well-formed line(s), %s total"
                  % (len(entries), _us(sum(e.us for e in entries)))]


INTERRUPT_LABELS = ("[hardirq]", "[softirq]", "[irq exit]", "[nmi]")
IRQ_ENTRY_RE = re.compile(r"^(asm_sysvec_|asm_common_interrupt|asm_fred_entrypoint|"
                          r"el1h_64_irq|el0t_64_irq)")
IDLE_LABEL_RE = re.compile(r"^swapper/\d+( \[softirq\])?$")
IRQ_EXIT_RE = re.compile(r"^(__)?irq_exit")


def check_interrupts(entries: List[Entry], parse_errors: Sequence[str] = ()
                     ) -> Tuple[bool, List[str]]:
    """Interrupt links end at the interrupt entry and never pose as the idle task.

    A waker segment labelled [hardirq]/[softirq]/[irq exit]/[nmi] whose stack
    contains an interrupt entry frame must end there (what lies below was
    only interrupted). A waker labelled swapper/N whose stack went through
    irq_exit is an interrupt that hit an idle CPU and must be labelled as one.
    """
    untrimmed, idle_posing = [], []
    for e in entries:
        for i, seg in enumerate(e.segs[1:], start=1):
            if not seg or seg[-1] in MARKERS:
                continue
            label, frames = seg[-1], segment_frames(e.segs, i)
            if label in INTERRUPT_LABELS:
                if any(IRQ_ENTRY_RE.match(f) for f in frames) and \
                   not IRQ_ENTRY_RE.match(frames[-1]):
                    untrimmed.append((e, i))
            elif IDLE_LABEL_RE.match(label) and any(IRQ_EXIT_RE.match(f) for f in frames):
                idle_posing.append((e, i))
    problems = list(parse_errors)
    for what, found in (("interrupt link not trimmed at the interrupt entry", untrimmed),
                        ("interrupt on an idle CPU labelled as the idle task", idle_posing)):
        if found:
            us = sum(e.us for e, _ in found)
            problems.append("%s: %d segment(s), %s" % (what, len(found), _us(us)))
            for e, i in sorted(found, key=lambda x: -x[0].us)[:DIAG_TOP]:
                problems.append("  line %d seg %d: %s" % (
                    e.lineno, i, _clip(";".join(e.segs[i]), 400)))
    if problems:
        return False, ["FAILED interrupts"] + problems
    n = sum(1 for e in entries for seg in e.segs[1:] if seg and seg[-1] in INTERRUPT_LABELS)
    return True, ["ok: %d interrupt link(s) trimmed at the interrupt entry, "
                  "none labelled as the idle task" % n]


def summary(entries: List[Entry], target: Optional[str], per_thread: bool,
            top: int) -> List[str]:
    sel = [e for e in entries if target is None or e.target(per_thread) == target]
    out = ["%d line(s), %s%s" % (len(sel), _us(sum(e.us for e in sel)),
                                 " for target %s" % target if target else "")]
    return out + _chain_histogram(sel, per_thread, top)


# --------------------------------------------------------------------------
# CLI


def _split_wakers(text: str) -> List[str]:
    return [w for w in (s.strip() for s in text.split(",")) if w] if text else []


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("chain", help="assert a chain carries enough time")
    c.add_argument("file")
    c.add_argument("--target", required=True, help="target task label")
    c.add_argument("--wakers", default="",
                   help="comma-separated waker labels, direct waker first")
    g = c.add_mutually_exclusive_group()
    g.add_argument("--truncated", action="store_true",
                   help="lines must end with [chain truncated]")
    g.add_argument("--unknown-waker", action="store_true",
                   help="lines must end with [unknown waker] (no --wakers)")
    c.add_argument("--min-us", type=int, default=1,
                   help="minimum summed microseconds (default 1)")
    c.add_argument("--frame-regex", action="append", default=[], metavar="SEG:REGEX",
                   help="a stack frame of segment SEG (0 = target, 1 = direct "
                        "waker, ...) must match REGEX in some matching line")
    c.add_argument("--per-thread", action="store_true",
                   help="strip /TID from labels before comparing")

    o = sub.add_parser("only-target", help="assert every line has this target")
    o.add_argument("file")
    o.add_argument("--target", required=True)
    o.add_argument("--per-thread", action="store_true")

    f = sub.add_parser("format", help="validate the folded grammar")
    f.add_argument("file")
    f.add_argument("--no-sort-check", action="store_true",
                   help="do not require ascending times")

    i = sub.add_parser("interrupts", help="interrupt links are trimmed and labelled")
    i.add_argument("file")

    s = sub.add_parser("summary", help="print the heaviest chains")
    s.add_argument("file")
    s.add_argument("--target")
    s.add_argument("--per-thread", action="store_true")
    s.add_argument("--top", type=int, default=20)
    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        entries, errors = read_folded(args.file)
    except OSError as e:
        print("check_chain: %s" % e, file=sys.stderr)
        return 2

    if args.cmd == "chain":
        wakers = _split_wakers(args.wakers)
        if args.unknown_waker and wakers:
            parser.error("--unknown-waker cannot be combined with --wakers")
        try:
            for spec in args.frame_regex:
                parse_frame_regex(spec)
        except ValueError as e:
            parser.error(str(e))
        ok, report = check_chain(entries, args.target, wakers,
                                 truncated=args.truncated,
                                 unknown_waker=args.unknown_waker,
                                 min_us=args.min_us,
                                 frame_regexes=args.frame_regex,
                                 per_thread=args.per_thread,
                                 parse_errors=errors)
    elif args.cmd == "only-target":
        ok, report = check_only_target(entries, args.target, args.per_thread, errors)
    elif args.cmd == "format":
        ok, report = check_format(entries, errors, not args.no_sort_check)
    elif args.cmd == "interrupts":
        ok, report = check_interrupts(entries, errors)
    else:
        for line in summary(entries, args.target, args.per_thread, args.top):
            print(line)
        return 0

    for line in report:
        print(line)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
