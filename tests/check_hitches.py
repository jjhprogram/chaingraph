#!/usr/bin/env python3
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
"""Assertions over hitchtrace records, joined to hitchbench ground truth.

Two JSONL inputs.

GROUND TRUTH (hitchbench), one object per frame it ran:

  {"frame_id": 412, "injected": "worker_block", "expect": "HT_BLOCK_TASK",
   "frame_us": 24000, "t_end_ns": 5123456789, "root_tid": 4242}

"injected" is null on a frame with no injection, and "expect" is then null
or absent. "frame_us" may be spelled "frame_ns".

RECORDS (hitchtrace), one object per over-budget frame: a JSON rendering of
struct ht_record (src/hitchtrace.h).

  {"frame_id": 412, "root_pid": 4242, "root_tgid": 4200, "root_comm": "hb_root",
   "frame_start_ns": 5099456789, "frame_end_ns": 5123456789,
   "frame_ns": 24000000, "budget_ns": 16667000, "nstalls": 3, "flags": [],
   "cause_ns": {"oncpu": 4000000, "oncpu_fault": 0, "oncpu_reclaim": 0,
                "runnable": 0, "runqueue": 0, "block_futex": 0, "block_io": 0,
                "block_task": 20000000, "block_other": 0, ...,
                "resolved_wait_oncpu": 2000000, "resolved_inherited": 18000000},
   "worst": {"ns": 19000000, "start_ns": 5103456789, "cause": "block_task",
             "kstack_id": 7, "ustack_id": -1,
             "hops": [{"pid": 4243, "tgid": 4200, "comm": "hb_worker",
                       "ns": 18000000, "cause": "block_io", "flags": [],
                       "kstack_id": 9}]},
   "threads": [{"tid": 4242, "comm": "hb_root", "root": true,
                "cause_ns": {"oncpu": 4000000, "block_task": 20000000, ...},
                "open_ns": 0, "preemptor": null,
                "largest": {"ns": 19000000, "cause": "block_task",
                            "blocked_in": "pipe_read", "kstack": [...]}},
               {"tid": 4243, "comm": "hb_worker", "root": false,
                "cause_ns": {"block_io": 18000000, ...}, "open_ns": 0,
                "largest": {"ns": 18000000, "cause": "block_io",
                            "blocked_in": "folio_wait_bit", "kstack": [...]},
                "preemptor": {"tid": 4250, "tgid": 4200, "comm": "hb_hog0",
                              "ns": 900000}}]}

"threads" is struct ht_thread, one entry per thread of the process that was
scheduled during the frame, the root among them marked "root". Its cause_ns
holds only the partition buckets, "largest" is its longest single off-CPU
interval (null cause and blocked_in when it never left the CPU) and
"preemptor" the thread that took its CPU for the longest, or null. The root's
line is a second view of the record's own cause_ns and partitions frame_ns;
the other threads ran in parallel, so their off-CPU time is context and is
never added to the frame. A record without the array simply carries no
per-thread detail, which is not an error.

Both writers add fields this checker does not look at (hitchbench's
"expect_resolved" and "note", hitchtrace's stacks and "blocked_in"); they are
ignored, as is any object without a "frame_id" (a header or a stats line).

A bucket is one entry of enum ht_cause, and its JSON key is the enum name
lowercased without the HT_ prefix ("HT_BLOCK_FUTEX" -> "block_futex"). How
many there are is read off that list and never assumed anywhere.

Spellings that are accepted anywhere a bucket is named: with or without the
HT_ prefix, any case ("HT_BLOCK_TASK", "block_task"), plus "inherited" and
"wait_oncpu" for the two HT_RESOLVED_* buckets. "cause_ns" may also be a
JSON array in enum order (HT_CAUSE_PARTITION or HT_CAUSE_MAX entries; a
thread's own array stops at the partition), "flags" may be the
HT_HOP_*/HT_FRAME_*/HT_THREAD_* bitmask instead of a list of names, and a
"cause" of "unknown" reads as no cause at all. A thread's "largest" and
"preemptor" are also read from flat largest_*/preemptor_* keys.

Subcommands (exit status 0 = pass, 1 = check failed, 2 = usage/IO error):

  join GT.jsonl HITCH.jsonl [--verbose]
      Per injector class: how many frames were injected, how many produced a
      record, how many of those name the class's expected bucket as the
      largest partition bucket, and the median number of threads a record of
      the class reports. Prints a table; never fails.

  expect GT.jsonl HITCH.jsonl --class C --bucket B [--resolved R]
         [--chain comm1,comm2,...] [--min-frac F] [--min-frames N]
      Fail unless at least N frames injected with class C have a record
      whose largest partition bucket is B, carrying at least F of the
      frame's excess (frame_ns - budget_ns), whose blocked time is dominated
      by the HT_RESOLVED_<R> bucket if --resolved is given, and
      whose worst stall's hops match --chain in order (substring per hop,
      "*" matches any hop; an interrupt hop also answers to "[hardirq]",
      "[softirq]", "[irqexit]", an idle waker to "[idle]").

  threads GT.jsonl HITCH.jsonl --class C [--thread COMM]
          [--bucket B | --bucket-any B1,B2,...] [--min-frac F]
          [--preemptor COMM] [--blocked-in REGEX] [--min-frames N]
      Fail unless at least N frames injected with class C have a record
      carrying a thread whose comm contains COMM (substring) and answering
      every condition given: its largest partition bucket is B (or one of
      the --bucket-any buckets), which together hold at least F of that
      thread's own off-CPU time; the thread that took its CPU is --preemptor
      (substring); the kernel function its longest off-CPU interval blocked
      in matches --blocked-in (a regular expression). Without --thread any
      one thread of the record may answer them.

  quiet GT.jsonl HITCH.jsonl [--max-false N] [--budget-us U]
      Fail if more than N frames with injected=null produced a record. Also
      reports how many uninjected frames the ground truth itself puts over
      budget, which is the noise floor the gate cannot be blamed for.

  present HITCH.jsonl [--min-frac F] [--min-records N]
      Fail unless at least F of the records carry HT_BLOCK_PRESENT time.
      Between the frame marker (present entry) and the present-end marker
      the root is waiting for the display, so a paced app spends the wait of
      its healthy frames there; a bracket that never opened leaves that time
      named after whatever the app blocked in -- HT_BLOCK_TIMER for a loop
      paced with a sleep -- and the failure says which bucket took it.
      Records are only the over-budget frames, so there may be none: that
      passes and says so, unless --min-records demands more.

  invariant HITCH.jsonl [--tol-us T] [--oncpu-stall] [--no-present]
      Fail if a record's partition buckets (HT_ONCPU..HT_BLOCK_OTHER) do not
      sum to frame_ns within T, or if HT_RESOLVED_WAIT_ONCPU +
      HT_RESOLVED_INHERITED exceeds the frame's blocked time (every
      HT_BLOCK_* bucket together: the pair splits any wait a task ended) by
      more than T. Where a record carries per-thread detail, also fail
      unless exactly one of its threads is the root and that root's buckets
      agree, within T, with the record's own: the two are the same timeline
      seen twice. With --oncpu-stall, also weigh the buckets carved out of
      HT_ONCPU at frame close (HT_ONCPU_FAULT, HT_ONCPU_FAULT_MAJOR,
      HT_ONCPU_RECLAIM, HT_ONCPU_COMPACT) against the frame they came from:
      together they cannot outlast it, and what the carve-out left in
      HT_ONCPU cannot either -- the counter is unsigned, so going below zero
      shows up as a bucket far larger than the frame rather than a negative
      one. With --no-present, instead assert that nothing at all is named
      HT_BLOCK_PRESENT: that bracket is opened by one probe and closed by
      another, so a run with the present marker disabled must leave the
      bucket empty in every record and every thread.

Any FILE may be "-" for stdin.
"""

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Set, Tuple

# enum ht_cause, in order, as src/hitchtrace.h spells it. The entries before
# HT_CAUSE_PARTITION (the first resolution bucket) sum to frame_ns; the two
# HT_RESOLVED_* buckets split the frame's blocked time by what the waker was
# doing and are reported separately. This tuple is the only place the set of
# buckets is written down: the partition boundary and every group below are
# derived from it, so adding a cause here is all it takes.
BUCKETS = ("oncpu", "oncpu_fault", "oncpu_fault_major", "oncpu_reclaim",
           "oncpu_compact", "runnable", "runqueue",
           "block_futex", "block_poll", "block_io", "block_timer",
           "block_gpu", "block_present", "block_task", "block_other",
           "resolved_wait_oncpu", "resolved_inherited")
PARTITION = BUCKETS.index("resolved_wait_oncpu")	# HT_CAUSE_PARTITION
IDX = {name: i for i, name in enumerate(BUCKETS)}
ONCPU = IDX["oncpu"]
WAIT_ONCPU = IDX["resolved_wait_oncpu"]
INHERITED = IDX["resolved_inherited"]
# Time the thread held a CPU: HT_ONCPU plus the kernel stalls carved out of
# it at frame close. ONCPU_STALLS is just the carve-outs (--oncpu-stall).
ONCPU_BUCKETS = tuple(i for i in range(PARTITION)
                      if BUCKETS[i].startswith("oncpu"))
ONCPU_STALLS = tuple(i for i in ONCPU_BUCKETS if i != ONCPU)
# Time the thread was blocked: what the HT_RESOLVED_* pair splits. Runnable
# and runqueue time is off-CPU too, but nobody woke the thread out of it.
BLOCKED = tuple(i for i in range(PARTITION) if BUCKETS[i].startswith("block_"))
# What the present bracket fills: the display pacing the app, rather than
# whatever syscall the wait inside present happened to block in.
PRESENT = IDX["block_present"]

BUCKET_ALIASES = {
    "wait_oncpu": "resolved_wait_oncpu",
    "waitoncpu": "resolved_wait_oncpu",
    "inherited": "resolved_inherited",
    "resolved_waker_oncpu": "resolved_wait_oncpu",
    "cause_partition": "resolved_wait_oncpu",	# HT_CAUSE_PARTITION aliases it
    "oncpu_minor_fault": "oncpu_fault",
    "oncpu_major_fault": "oncpu_fault_major",
}

# HT_HOP_* bits, and the label an interrupt hop answers to in --chain.
HOP_FLAG_BITS = ((1 << 0, "irq"), (1 << 1, "softirq"), (1 << 2, "irqexit"),
                 (1 << 3, "idle"), (1 << 4, "oncpu"), (1 << 5, "trunc"),
                 (1 << 6, "stale"))
HOP_TAGS = (("irq", "[hardirq]"), ("softirq", "[softirq]"),
            ("irqexit", "[irqexit]"), ("idle", "[idle]"))
# HT_FRAME_* bits.
FRAME_FLAG_BITS = ((1 << 0, "open_stall"), (1 << 1, "no_root_state"),
                   (1 << 2, "lost"), (1 << 3, "threads_full"))
# HT_THREAD_* bits.
THREAD_FLAG_BITS = ((1 << 0, "root"), (1 << 1, "blocked_end"))

# A --resolved bucket must hold at least this much of HT_BLOCK_TASK.
RESOLVED_MIN_FRAC = 0.5
DIAG_TOP = 5			# offending frames printed per failure
NO_CLASS = "(none)"		# stands in for injected=null in the join table


class ParseError(Exception):
    """An input line does not hold what this checker was told to expect."""


# --------------------------------------------------------------------------
# value normalization


def normalize_bucket(value) -> str:
    """Canonical bucket name for an enum index, an enum name or a short name.

    Raises ValueError for anything else, including the "unknown" hitchtrace
    prints for a cause outside enum ht_cause; callers that can live without
    a cause use optional_bucket().
    """
    if isinstance(value, bool):
        raise ValueError("%r is not a bucket" % (value,))
    if isinstance(value, int):
        if 0 <= value < len(BUCKETS):
            return BUCKETS[value]
        raise ValueError("bucket index %d is out of range" % value)
    if not isinstance(value, str):
        raise ValueError("%r is not a bucket" % (value,))
    name = value.strip().lower().replace("-", "_").replace(" ", "_")
    if name.startswith("ht_"):
        name = name[3:]
    name = BUCKET_ALIASES.get(name, name)
    if name not in IDX:
        raise ValueError("unknown bucket %r (known: %s)"
                         % (value, ", ".join(BUCKETS)))
    return name


def optional_bucket(value) -> Optional[str]:
    """Like normalize_bucket(), but None for a missing or unknown cause."""
    if value is None:
        return None
    if isinstance(value, str) and value.strip().lower() in ("", "unknown", "-"):
        return None
    return normalize_bucket(value)


def enum_name(bucket: str) -> str:
    return "HT_" + bucket.upper()


def blocked_ns(cause_ns: Sequence[int]) -> int:
    """Everything one timeline spent blocked, across the HT_BLOCK_* buckets."""
    return sum(cause_ns[i] for i in BLOCKED)


def largest_blocked(cause_ns: Sequence[int]) -> Optional[str]:
    """The largest HT_BLOCK_* bucket of one timeline, None if it never blocked.

    What a wait ended up being called, which is the question when a bucket
    that should have held it is empty.
    """
    best = max(BLOCKED, key=lambda i: cause_ns[i])
    return BUCKETS[best] if cause_ns[best] else None


def normalize_flags(value, bits) -> Set[str]:
    """Flag names from a bitmask, a list of names, or a mapping to booleans."""
    if value is None:
        return set()
    if isinstance(value, bool):
        raise ValueError("%r is not a flag set" % (value,))
    if isinstance(value, int):
        return {name for bit, name in bits if value & bit}
    if isinstance(value, dict):
        value = [k for k, v in value.items() if v]
    if isinstance(value, str):
        value = value.split("|")
    if not isinstance(value, (list, tuple)):
        raise ValueError("%r is not a flag set" % (value,))
    out = set()
    for item in value:
        if not isinstance(item, str):
            raise ValueError("%r is not a flag name" % (item,))
        name = item.strip().lower()
        for prefix in ("ht_hop_", "ht_frame_", "ht_thread_", "ht_"):
            if name.startswith(prefix):
                name = name[len(prefix):]
                break
        if name:
            # A name outside HT_*_FLAG_BITS is kept as it stands: an unknown
            # flag is information, not a schema error.
            out.add(name)
    return out


def _as_int(value, what: str) -> int:
    if isinstance(value, bool) or value is None:
        raise ValueError("%s: %r is not a number" % (what, value))
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(round(value))
    raise ValueError("%s: %r is not a number" % (what, value))


def _pick(obj: dict, names: Sequence[str]):
    for name in names:
        if name in obj and obj[name] is not None:
            return obj[name]
    return None


def _ns(ns: Optional[int]) -> str:
    if ns is None:
        return "?"
    return "%.3f ms" % (ns / 1e6)


def _pct(num: int, den: int) -> str:
    return "n/a" if not den else "%.1f%%" % (100.0 * num / den)


# --------------------------------------------------------------------------
# input model


@dataclass
class Hop:
    comm: str = ""
    pid: Optional[int] = None
    tgid: Optional[int] = None
    ns: Optional[int] = None
    cause: Optional[str] = None
    flags: Set[str] = field(default_factory=set)

    def labels(self) -> List[str]:
        """Everything this hop answers to in a --chain pattern."""
        out = [self.comm] if self.comm else []
        out += [tag for flag, tag in HOP_TAGS if flag in self.flags]
        return out

    def describe(self) -> str:
        who = self.comm or "?"
        if self.pid is not None:
            who += "/%d" % self.pid
        extra = [tag for flag, tag in HOP_TAGS if flag in self.flags]
        if extra:
            who += " " + " ".join(extra)
        return "%s(%s, %s)" % (who, self.cause or "?", _ns(self.ns))


@dataclass
class Largest:
    """A thread's longest single off-CPU interval of the frame."""
    ns: Optional[int] = None
    cause: Optional[str] = None
    blocked_in: str = ""

    def describe(self) -> str:
        out = "largest %s %s" % (self.cause or "?", _ns(self.ns))
        if self.blocked_in:
            out += " in %s" % self.blocked_in
        return out


@dataclass
class Preemptor:
    """The thread that took this one's CPU for the longest."""
    tid: Optional[int] = None
    comm: str = ""
    ns: Optional[int] = None

    def describe(self) -> str:
        who = self.comm or "?"
        if self.tid is not None:
            who += "/%d" % self.tid
        return "%s for %s" % (who, _ns(self.ns))


@dataclass
class Thread:
    """What one thread of the process did during the frame (struct ht_thread).

    Only the root's cause_ns partitions frame_ns. The others ran in parallel,
    so their off-CPU time is context: it is never summed into the frame.
    """
    tid: Optional[int] = None
    comm: str = ""
    root: bool = False
    cause_ns: List[int] = field(default_factory=lambda: [0] * len(BUCKETS))
    open_ns: int = 0
    largest: Largest = field(default_factory=Largest)
    preemptor: Optional[Preemptor] = None
    flags: Set[str] = field(default_factory=set)

    def bucket(self, name: str) -> int:
        return self.cause_ns[IDX[name]]

    def largest_bucket(self) -> str:
        """The largest partition bucket; ties go to the lowest enum index."""
        best = 0
        for i in range(1, PARTITION):
            if self.cause_ns[i] > self.cause_ns[best]:
                best = i
        return BUCKETS[best]

    def offcpu_ns(self) -> int:
        """Everything outside the on-CPU buckets: what --min-frac measures.

        A page fault or a reclaim is time the thread held a CPU, so it counts
        with HT_ONCPU and not against the stall the thread is being judged on.
        """
        return sum(self.cause_ns[i] for i in range(PARTITION)
                   if i not in ONCPU_BUCKETS)

    def label(self) -> str:
        who = self.comm or "?"
        if self.tid is not None:
            who += "/%d" % self.tid
        return who

    def describe(self) -> List[str]:
        part = "  ".join("%s %s" % (BUCKETS[i], _ns(self.cause_ns[i]))
                         for i in range(PARTITION) if self.cause_ns[i])
        out = ["%s %s: %s" % ("*" if self.root else " ", self.label(),
                              part or "(all zero)")]
        extra = []
        if self.largest.ns:
            extra.append(self.largest.describe())
        if self.preemptor is not None:
            extra.append("preempted by %s" % self.preemptor.describe())
        if self.open_ns or "blocked_end" in self.flags:
            extra.append("off-CPU at frame end for %s" % _ns(self.open_ns))
        if extra:
            out.append("    " + "; ".join(extra))
        return out


@dataclass
class Record:
    lineno: int
    frame_id: int
    frame_ns: int
    budget_ns: int
    cause_ns: List[int]
    worst_ns: Optional[int] = None
    worst_cause: Optional[str] = None
    hops: List[Hop] = field(default_factory=list)
    flags: Set[str] = field(default_factory=set)
    root_comm: str = ""
    root_pid: Optional[int] = None
    nstalls: Optional[int] = None
    threads: List[Thread] = field(default_factory=list)

    def bucket(self, name: str) -> int:
        return self.cause_ns[IDX[name]]

    @property
    def excess_ns(self) -> int:
        return self.frame_ns - self.budget_ns

    def largest(self) -> str:
        """The largest partition bucket; ties go to the lowest enum index."""
        best = 0
        for i in range(1, PARTITION):
            if self.cause_ns[i] > self.cause_ns[best]:
                best = i
        return BUCKETS[best]

    def roots(self) -> List[Thread]:
        return [t for t in self.threads if t.root]

    def thread_lines(self) -> List[str]:
        """The record's thread table, indented like describe()'s own lines."""
        if not self.threads:
            return ["  threads: (none reported)"]
        out = ["  threads (%d):" % len(self.threads)]
        for t in self.threads:
            out += ["    " + ln for ln in t.describe()]
        return out

    def describe(self, threads: bool = False) -> List[str]:
        head = ("frame %d: frame_ns %s, budget %s, excess %s, root %s%s"
                % (self.frame_id, _ns(self.frame_ns), _ns(self.budget_ns),
                   _ns(self.excess_ns), self.root_comm or "?",
                   "/%d" % self.root_pid if self.root_pid is not None else ""))
        if self.flags:
            head += ", flags %s" % ",".join(sorted(self.flags))
        head += " (line %d)" % self.lineno
        part = ["%s %s" % (BUCKETS[i], _ns(self.cause_ns[i]))
                for i in range(PARTITION) if self.cause_ns[i]]
        resolved = ["%s %s" % (BUCKETS[i], _ns(self.cause_ns[i]))
                    for i in (WAIT_ONCPU, INHERITED) if self.cause_ns[i]]
        out = [head,
               "  buckets: %s" % ("  ".join(part) if part else "(all zero)")]
        if resolved:
            out.append("  resolved: %s" % "  ".join(resolved))
        worst = "  worst: %s %s" % (self.worst_cause or "?", _ns(self.worst_ns))
        worst += "; hops: %s" % (" <- ".join(h.describe() for h in self.hops)
                                 if self.hops else "(none)")
        out.append(worst)
        if threads:
            out += self.thread_lines()
        return out


@dataclass
class Frame:
    """One ground-truth frame."""
    lineno: int
    frame_id: int
    injected: Optional[str] = None
    expect: Optional[str] = None
    frame_ns: Optional[int] = None
    t_end_ns: Optional[int] = None
    root_tid: Optional[int] = None


# --------------------------------------------------------------------------
# parsing


def _parse_jsonl(stream, path: str) -> List[Tuple[int, dict]]:
    items = []
    for lineno, line in enumerate(stream, 1):
        text = line.strip()
        if not text or text.startswith("#"):
            continue
        try:
            obj = json.loads(text)
        except ValueError as e:
            raise ParseError("%s:%d: not JSON: %s" % (path, lineno, e))
        if not isinstance(obj, dict):
            raise ParseError("%s:%d: expected a JSON object, got %s"
                             % (path, lineno, type(obj).__name__))
        items.append((lineno, obj))
    return items


def read_jsonl(path: str) -> List[Tuple[int, dict]]:
    if path == "-":
        return _parse_jsonl(sys.stdin, "<stdin>")
    with open(path, encoding="utf-8", errors="replace") as f:
        return _parse_jsonl(f, path)


def parse_cause_ns(value, where: str) -> List[int]:
    out = [0] * len(BUCKETS)
    if isinstance(value, (list, tuple)):
        if len(value) not in (PARTITION, len(BUCKETS)):
            raise ParseError("%s: cause_ns has %d entries, expected %d or %d"
                             % (where, len(value), PARTITION, len(BUCKETS)))
        for i, item in enumerate(value):
            out[i] = _as_int(item, "%s: cause_ns[%d]" % (where, i))
        return out
    if not isinstance(value, dict):
        raise ParseError("%s: cause_ns is %s, expected an object or an array"
                         % (where, type(value).__name__))
    for key, item in value.items():
        try:
            name = normalize_bucket(key)
        except ValueError as e:
            raise ParseError("%s: cause_ns: %s" % (where, e))
        out[IDX[name]] = _as_int(item, "%s: cause_ns[%s]" % (where, name))
    return out


def parse_hop(obj, where: str) -> Hop:
    if not isinstance(obj, dict):
        raise ParseError("%s: hop is %s, expected an object"
                         % (where, type(obj).__name__))
    try:
        cause = optional_bucket(_pick(obj, ("cause", "stall", "state")))
    except ValueError as e:
        raise ParseError("%s: hop cause: %s" % (where, e))
    try:
        flags = normalize_flags(_pick(obj, ("flags",)), HOP_FLAG_BITS)
    except ValueError as e:
        raise ParseError("%s: hop flags: %s" % (where, e))
    ns = _pick(obj, ("ns", "time_ns"))
    pid = _pick(obj, ("pid", "tid"))
    tgid = _pick(obj, ("tgid", "pid_tgid"))
    comm = _pick(obj, ("comm", "name", "label"))
    return Hop(comm=comm if isinstance(comm, str) else "",
               pid=None if pid is None else _as_int(pid, "%s: hop pid" % where),
               tgid=None if tgid is None else _as_int(tgid, "%s: hop tgid" % where),
               ns=None if ns is None else _as_int(ns, "%s: hop ns" % where),
               cause=cause, flags=flags)


def parse_preemptor(obj: dict, where: str) -> Optional[Preemptor]:
    """The thread that took this one's CPU, nested or as preemptor_* keys."""
    nested = _pick(obj, ("preemptor",))
    if nested is None:
        tid = _pick(obj, ("preemptor_pid", "preemptor_tid"))
        comm = _pick(obj, ("preemptor_comm",))
        ns = _pick(obj, ("preemptor_ns",))
        if tid is None and comm is None and ns is None:
            return None
        nested = {"pid": tid, "comm": comm, "ns": ns}
    elif not isinstance(nested, dict):
        raise ParseError("%s: preemptor is %s, expected an object or null"
                         % (where, type(nested).__name__))
    tid = _pick(nested, ("pid", "tid"))
    comm = _pick(nested, ("comm", "name"))
    ns = _pick(nested, ("ns", "time_ns"))
    # An all-empty preemptor is how "nobody took the CPU" comes across.
    if tid in (None, 0) and not comm and not ns:
        return None
    return Preemptor(tid=None if tid is None else _as_int(tid, "%s: preemptor pid" % where),
                     comm=comm if isinstance(comm, str) else "",
                     ns=None if ns is None else _as_int(ns, "%s: preemptor ns" % where))


def parse_largest(obj: dict, where: str) -> Largest:
    """The thread's longest off-CPU interval, nested or as largest_* keys."""
    nested = _pick(obj, ("largest",))
    if nested is None:
        nested = {"ns": _pick(obj, ("largest_ns",)),
                  "cause": _pick(obj, ("largest_cause",)),
                  "blocked_in": _pick(obj, ("largest_blocked_in", "blocked_in"))}
    elif not isinstance(nested, dict):
        raise ParseError("%s: largest is %s, expected an object"
                         % (where, type(nested).__name__))
    try:
        cause = optional_bucket(_pick(nested, ("cause",)))
    except ValueError as e:
        raise ParseError("%s: largest cause: %s" % (where, e))
    ns = _pick(nested, ("ns", "time_ns"))
    blocked_in = _pick(nested, ("blocked_in", "where"))
    return Largest(ns=None if ns is None else _as_int(ns, "%s: largest ns" % where),
                   cause=cause,
                   blocked_in=blocked_in if isinstance(blocked_in, str) else "")


def parse_thread(obj, where: str) -> Thread:
    if not isinstance(obj, dict):
        raise ParseError("%s: thread is %s, expected an object"
                         % (where, type(obj).__name__))
    try:
        flags = normalize_flags(_pick(obj, ("flags",)), THREAD_FLAG_BITS)
    except ValueError as e:
        raise ParseError("%s: thread flags: %s" % (where, e))
    cause_ns = [0] * len(BUCKETS)
    raw = _pick(obj, ("cause_ns", "causes"))
    if raw is not None:
        cause_ns = parse_cause_ns(raw, "%s: thread" % where)
    tid = _pick(obj, ("pid", "tid"))
    comm = _pick(obj, ("comm", "name"))
    open_ns = _pick(obj, ("open_ns",))
    root = _pick(obj, ("root", "is_root"))
    return Thread(tid=None if tid is None else _as_int(tid, "%s: thread pid" % where),
                  comm=comm if isinstance(comm, str) else "",
                  root="root" in flags or root is True,
                  cause_ns=cause_ns,
                  open_ns=0 if open_ns is None
                          else _as_int(open_ns, "%s: thread open_ns" % where),
                  largest=parse_largest(obj, where),
                  preemptor=parse_preemptor(obj, where),
                  flags=flags)


def parse_threads(value, where: str) -> List[Thread]:
    """The record's per-thread detail; absent reads as none reported."""
    if value is None:
        return []
    if not isinstance(value, (list, tuple)):
        raise ParseError("%s: threads is %s, expected an array"
                         % (where, type(value).__name__))
    return [parse_thread(t, where) for t in value]


def parse_record(obj: dict, lineno: int, path: str) -> Record:
    where = "%s:%d" % (path, lineno)
    if "cause_ns" not in obj and "totals_ns" not in obj and "causes" not in obj:
        if "injected" in obj or "frame_us" in obj:
            raise ParseError("%s: this looks like a ground-truth frame, not a "
                             "hitchtrace record (arguments swapped?)" % where)
        raise ParseError("%s: record has no cause_ns" % where)

    frame_id = _as_int(_pick(obj, ("frame_id", "frame")), "%s: frame_id" % where)
    start = _pick(obj, ("frame_start_ns", "t_start_ns"))
    end = _pick(obj, ("frame_end_ns", "t_end_ns"))
    frame_ns = _pick(obj, ("frame_ns",))
    if frame_ns is None and _pick(obj, ("frame_us",)) is not None:
        frame_ns = _as_int(obj["frame_us"], "%s: frame_us" % where) * 1000
    if frame_ns is None and start is not None and end is not None:
        frame_ns = (_as_int(end, "%s: frame_end_ns" % where) -
                    _as_int(start, "%s: frame_start_ns" % where))
    if frame_ns is None:
        raise ParseError("%s: record has no frame_ns" % where)
    budget = _pick(obj, ("budget_ns",))
    if budget is None and _pick(obj, ("budget_us",)) is not None:
        budget = _as_int(obj["budget_us"], "%s: budget_us" % where) * 1000
    if budget is None:
        raise ParseError("%s: record has no budget_ns" % where)

    cause_ns = parse_cause_ns(_pick(obj, ("cause_ns", "totals_ns", "causes")), where)
    try:
        flags = normalize_flags(_pick(obj, ("flags",)), FRAME_FLAG_BITS)
    except ValueError as e:
        raise ParseError("%s: flags: %s" % (where, e))

    worst = _pick(obj, ("worst", "worst_stall", "stall", "largest"))
    worst_ns = worst_cause = None
    hops_raw = _pick(obj, ("hops",))
    if worst is not None:
        if not isinstance(worst, dict):
            raise ParseError("%s: worst is %s, expected an object"
                             % (where, type(worst).__name__))
        if _pick(worst, ("ns",)) is not None:
            worst_ns = _as_int(worst["ns"], "%s: worst.ns" % where)
        try:
            worst_cause = optional_bucket(_pick(worst, ("cause",)))
        except ValueError as e:
            raise ParseError("%s: worst.cause: %s" % (where, e))
        if hops_raw is None:
            hops_raw = _pick(worst, ("hops", "chain"))
    hops = []
    if hops_raw is not None:
        if not isinstance(hops_raw, (list, tuple)):
            raise ParseError("%s: hops is %s, expected an array"
                             % (where, type(hops_raw).__name__))
        hops = [parse_hop(h, where) for h in hops_raw]

    root_pid = _pick(obj, ("root_pid", "root_tid", "tid"))
    root_comm = _pick(obj, ("root_comm", "comm"))
    nstalls = _pick(obj, ("nstalls",))
    threads = parse_threads(_pick(obj, ("threads",)), where)
    return Record(lineno=lineno, frame_id=frame_id, frame_ns=frame_ns,
                  budget_ns=budget, cause_ns=cause_ns, worst_ns=worst_ns,
                  worst_cause=worst_cause, hops=hops, flags=flags,
                  root_comm=root_comm if isinstance(root_comm, str) else "",
                  root_pid=(None if root_pid is None
                            else _as_int(root_pid, "%s: root_pid" % where)),
                  nstalls=(None if nstalls is None
                           else _as_int(nstalls, "%s: nstalls" % where)),
                  threads=threads)


def parse_frame(obj: dict, lineno: int, path: str) -> Frame:
    where = "%s:%d" % (path, lineno)
    if "cause_ns" in obj:
        raise ParseError("%s: this looks like a hitchtrace record, not a "
                         "ground-truth frame (arguments swapped?)" % where)
    frame_id = _as_int(_pick(obj, ("frame_id", "frame")), "%s: frame_id" % where)
    injected = obj.get("injected")
    if injected is not None and not isinstance(injected, str):
        raise ParseError("%s: injected is %s, expected a string or null"
                         % (where, type(injected).__name__))
    expect = obj.get("expect")
    if expect is not None:
        if isinstance(expect, dict):		# {"bucket": "HT_..."} form
            expect = _pick(expect, ("bucket", "cause"))
        try:
            expect = optional_bucket(expect)
        except ValueError as e:
            raise ParseError("%s: expect: %s" % (where, e))
    frame_ns = _pick(obj, ("frame_ns",))
    if frame_ns is None and _pick(obj, ("frame_us",)) is not None:
        frame_ns = _as_int(obj["frame_us"], "%s: frame_us" % where) * 1000
    elif frame_ns is not None:
        frame_ns = _as_int(frame_ns, "%s: frame_ns" % where)
    t_end = _pick(obj, ("t_end_ns", "frame_end_ns"))
    root_tid = _pick(obj, ("root_tid", "root_pid", "tid"))
    return Frame(lineno=lineno, frame_id=frame_id, injected=injected,
                 expect=expect, frame_ns=frame_ns,
                 t_end_ns=(None if t_end is None
                           else _as_int(t_end, "%s: t_end_ns" % where)),
                 root_tid=(None if root_tid is None
                           else _as_int(root_tid, "%s: root_tid" % where)))


def read_records(path: str) -> Tuple[List[Record], int]:
    """Records plus the number of non-record (header/stats) objects skipped."""
    items = read_jsonl(path)
    records, skipped = [], 0
    for lineno, obj in items:
        if _pick(obj, ("frame_id", "frame")) is None:
            skipped += 1
            continue
        records.append(parse_record(obj, lineno, path))
    return records, skipped


def read_window(path: str) -> Tuple[Optional[int], Optional[int]]:
    """The tracing window, from hitchtrace's header/summary lines (if any)."""
    start = end = None
    for _, obj in read_jsonl(path):
        if not isinstance(obj, dict):
            continue
        if obj.get("trace_start_ns") is not None:
            start = _as_int(obj["trace_start_ns"], "trace_start_ns")
        if obj.get("trace_end_ns") is not None:
            end = _as_int(obj["trace_end_ns"], "trace_end_ns")
    return start, end


def in_window(frames: Sequence[Frame], start: Optional[int],
              end: Optional[int]) -> List[Frame]:
    """Ground-truth frames that ended while tracing was running.

    Frames outside the window were never offered to the tool, so counting
    them as misses would understate it.
    """
    if start is None and end is None:
        return list(frames)
    kept = []
    for f in frames:
        if f.t_end_ns is None:
            kept.append(f)
            continue
        if start is not None and f.t_end_ns < start:
            continue
        if end is not None and f.t_end_ns > end:
            continue
        kept.append(f)
    return kept


def read_ground_truth(path: str) -> Tuple[List[Frame], int]:
    items = read_jsonl(path)
    frames, skipped = [], 0
    for lineno, obj in items:
        if _pick(obj, ("frame_id", "frame")) is None:
            skipped += 1
            continue
        frames.append(parse_frame(obj, lineno, path))
    return frames, skipped


def index_records(records: Sequence[Record]) -> Tuple[Dict[int, Record], List[int]]:
    """Map frame_id -> record (first wins), plus the duplicated frame_ids."""
    by_id: Dict[int, Record] = {}
    dups: List[int] = []
    for rec in records:
        if rec.frame_id in by_id:
            dups.append(rec.frame_id)
        else:
            by_id[rec.frame_id] = rec
    return by_id, dups


# --------------------------------------------------------------------------
# checks shared by the subcommands


def split_chain(text: str) -> List[str]:
    return [c for c in (s.strip() for s in text.split(",")) if c] if text else []


def match_chain(hops: Sequence[Hop], patterns: Sequence[str]) -> Optional[str]:
    """None if the hops match, else why they do not."""
    if len(patterns) > len(hops):
        return ("chain has %d hop(s) (%s), need %d"
                % (len(hops), " <- ".join(h.describe() for h in hops) or "none",
                   len(patterns)))
    for i, pattern in enumerate(patterns):
        if pattern == "*":
            continue
        want = pattern.lower()
        if not any(want in label.lower() for label in hops[i].labels()):
            return "hop %d is %s, expected %r" % (i, hops[i].describe(), pattern)
    return None


def check_frame(rec: Optional[Record], bucket: str, min_frac: float,
                resolved: Optional[str],
                chain: Sequence[str],
                need_largest: bool = True) -> List[Tuple[str, str]]:
    """(kind, detail) for each way this injected frame is not diagnosed."""
    if rec is None:
        return [("no record", "no record for this frame")]
    reasons = []
    largest = rec.largest()
    if need_largest and largest != bucket:
        reasons.append(("wrong bucket",
                        "largest bucket is %s (%s), expected %s"
                        % (enum_name(largest), _ns(rec.bucket(largest)),
                           enum_name(bucket))))
    got = rec.bucket(bucket)
    excess = rec.excess_ns
    need = min_frac * excess if excess > 0 else 0.0
    if got <= 0 or got < need:
        reasons.append(("below --min-frac",
                        "%s is %s, needs %s (%.2f of the %s excess)"
                        % (enum_name(bucket), _ns(got), _ns(int(need)),
                           min_frac, _ns(excess))))
    if resolved is not None:
        # Any wait a task ended is resolved through its waker, whichever
        # HT_BLOCK_* bucket the wait itself landed in, so the resolution is
        # weighed against everything the frame spent blocked.
        block = blocked_ns(rec.cause_ns)
        mine = rec.bucket(resolved)
        other = rec.cause_ns[WAIT_ONCPU if resolved == BUCKETS[INHERITED]
                             else INHERITED]
        if block <= 0:
            reasons.append(("wrong resolution",
                            "the frame blocked nowhere, nothing to resolve"))
        elif mine < RESOLVED_MIN_FRAC * block or mine < other:
            reasons.append(("wrong resolution",
                            "%s is %s of the %s the frame spent blocked "
                            "(other resolution %s)"
                            % (enum_name(resolved), _ns(mine), _ns(block),
                               _ns(other))))
    if chain:
        why = match_chain(rec.hops, chain)
        if why:
            reasons.append(("chain mismatch", why))
    return reasons


def thread_reasons(th: Thread, buckets: Sequence[str], min_frac: float,
                   preemptor: str,
                   blocked_in: Optional[re.Pattern]) -> List[Tuple[str, str]]:
    """(kind, detail) for each condition this thread does not answer."""
    reasons = []
    if buckets:
        # Several acceptable buckets are one stall spelled more than one way,
        # so --min-frac weighs them together: a wait that lands half in
        # HT_BLOCK_TASK and half in HT_BLOCK_TIMER still dominates the thread.
        names = " or ".join(enum_name(b) for b in buckets)
        largest = th.largest_bucket()
        if largest not in buckets:
            reasons.append(("wrong bucket",
                            "largest bucket is %s (%s), expected %s"
                            % (enum_name(largest), _ns(th.bucket(largest)),
                               names)))
        got = sum(th.bucket(b) for b in buckets)
        offcpu = th.offcpu_ns()
        need = min_frac * offcpu if offcpu > 0 else 0.0
        if got <= 0 or got < need:
            reasons.append(("below --min-frac",
                            "%s is %s, needs %s (%.2f of the %s this thread "
                            "spent off-CPU)"
                            % (names, _ns(got), _ns(int(need)), min_frac,
                               _ns(offcpu))))
    if preemptor:
        if th.preemptor is None:
            reasons.append(("wrong preemptor",
                            "nothing took this thread's CPU, expected %r"
                            % preemptor))
        elif preemptor.lower() not in th.preemptor.comm.lower():
            reasons.append(("wrong preemptor",
                            "preemptor is %s, expected %r"
                            % (th.preemptor.describe(), preemptor)))
    if blocked_in is not None:
        where = th.largest.blocked_in
        if not where:
            reasons.append(("blocked-in mismatch",
                            "the longest off-CPU interval names no kernel "
                            "function, expected /%s/" % blocked_in.pattern))
        elif not blocked_in.search(where):
            reasons.append(("blocked-in mismatch",
                            "the longest off-CPU interval blocked in %r, "
                            "expected /%s/" % (where, blocked_in.pattern)))
    return reasons


def check_threads(rec: Optional[Record], comm: str, buckets: Sequence[str],
                  min_frac: float, preemptor: str,
                  blocked_in: Optional[re.Pattern]) -> List[Tuple[str, str]]:
    """(kind, detail) for each way this frame's thread detail falls short.

    Empty when some thread of the record answers every condition. Without
    --thread any thread may; with it, only the ones whose comm contains it,
    and the closest of those is the one the diagnosis describes.
    """
    if rec is None:
        return [("no record", "no record for this frame")]
    if not rec.threads:
        return [("no threads", "the record carries no per-thread detail")]
    if comm:
        cands = [t for t in rec.threads if comm.lower() in t.comm.lower()]
    else:
        cands = list(rec.threads)
    if not cands:
        return [("no such thread",
                 "no thread whose comm contains %r (threads: %s)"
                 % (comm, ", ".join(t.label() for t in rec.threads)))]
    best: Optional[List[Tuple[str, str]]] = None
    for th in cands:
        reasons = thread_reasons(th, buckets, min_frac, preemptor, blocked_in)
        if not reasons:
            return []
        if best is None or len(reasons) < len(best):
            best = [(kind, "%s: %s" % (th.label(), detail))
                    for kind, detail in reasons]
    return best or []


# --------------------------------------------------------------------------
# subcommands


def median(values: Sequence[float]) -> Optional[float]:
    """The middle value, or the mean of the two middle ones; None if empty."""
    if not values:
        return None
    ordered = sorted(values)
    mid = len(ordered) // 2
    if len(ordered) % 2:
        return float(ordered[mid])
    return (ordered[mid - 1] + ordered[mid]) / 2.0


def cmd_join(frames: Sequence[Frame], records: Sequence[Record],
             verbose: bool) -> Tuple[bool, List[str]]:
    by_id, dups = index_records(records)
    classes: List[str] = []
    for f in frames:
        name = f.injected or NO_CLASS
        if name not in classes:
            classes.append(name)

    rows = [("class", "expect", "frames", "records", "bucket-hit", "threads")]
    details: List[str] = []
    for name in classes:
        sel = [f for f in frames if (f.injected or NO_CLASS) == name]
        with_rec = [f for f in sel if f.frame_id in by_id]
        expects = sorted({f.expect for f in sel if f.expect})
        want = "/".join(enum_name(e) for e in expects) if expects else "-"
        if expects:
            hit = [f for f in with_rec
                   if f.expect and by_id[f.frame_id].largest() == f.expect]
            hits = "%d (%s)" % (len(hit), _pct(len(hit), len(with_rec)))
        else:
            hit = []
            hits = "-"
        nthreads = median([len(by_id[f.frame_id].threads) for f in with_rec])
        rows.append((name, want, str(len(sel)),
                     "%d (%s)" % (len(with_rec), _pct(len(with_rec), len(sel))),
                     hits, "-" if nthreads is None else "%g" % nthreads))
        if verbose:
            details.append("%s: %d frame(s), %d with a record, %d on bucket"
                           % (name, len(sel), len(with_rec), len(hit)))
            missing = [f for f in sel if f.frame_id not in by_id]
            if missing and name != NO_CLASS:
                details.append("  no record for frame(s): %s%s"
                               % (", ".join(str(f.frame_id)
                                            for f in missing[:DIAG_TOP]),
                                  " ..." if len(missing) > DIAG_TOP else ""))
            hit_ids = {f.frame_id for f in hit}
            wrong = [f for f in with_rec if f.frame_id not in hit_ids]
            for f in wrong[:DIAG_TOP]:
                details += ["  " + ln for ln in by_id[f.frame_id].describe()]

    widths = [max(len(r[i]) for r in rows) for i in range(len(rows[0]))]
    out = [" ".join(cell.ljust(widths[i]) for i, cell in enumerate(row)).rstrip()
           for row in rows]
    out.append("%d frame(s), %d record(s)" % (len(frames), len(records)))
    frame_ids = {f.frame_id for f in frames}
    unmatched = [r for r in records if r.frame_id not in frame_ids]
    if unmatched:
        out.append("%d record(s) for frame_ids absent from the ground truth"
                   % len(unmatched))
    if dups:
        out.append("%d duplicate record frame_id(s): %s"
                   % (len(dups), ", ".join(str(d) for d in dups[:DIAG_TOP])))
    return True, out + details


def cmd_expect(frames: Sequence[Frame], records: Sequence[Record],
               cls: str, bucket: str, min_frac: float, min_frames: int,
               resolved: Optional[str],
               chain: Sequence[str],
               need_largest: bool = True) -> Tuple[bool, List[str]]:
    by_id, _ = index_records(records)
    sel = [f for f in frames if f.injected == cls]
    good, bad = [], []
    for f in sel:
        rec = by_id.get(f.frame_id)
        reasons = check_frame(rec, bucket, min_frac, resolved, chain,
                              need_largest)
        (good if not reasons else bad).append((f, rec, reasons))

    want = "%s %s>= %.2f of the excess" % (
        enum_name(bucket),
        "as the largest bucket with " if need_largest else "carrying ",
        min_frac)
    if resolved is not None:
        want += ", resolved %s" % enum_name(resolved)
    if chain:
        want += ", chain %s" % " <- ".join(chain)
    head = ("%d/%d frame(s) injected with %s show %s"
            % (len(good), len(sel), cls, want))
    if len(good) >= min_frames and sel:
        return True, ["ok: " + head + " (need %d)" % min_frames]

    report = ["FAILED expect %s: %s, need %d" % (cls, head, min_frames)]
    if not sel:
        report.append("  the ground truth has no frame injected with %r "
                      "(classes: %s)"
                      % (cls, ", ".join(sorted({f.injected for f in frames
                                                if f.injected})) or "none"))
        return False, report
    norec = [t for t in bad if t[1] is None]
    if norec:
        report.append("  %d of the %d injected frame(s) produced no record "
                      "(frame_ids %s%s)"
                      % (len(norec), len(sel),
                         ", ".join(str(t[0].frame_id) for t in norec[:DIAG_TOP]),
                         " ..." if len(norec) > DIAG_TOP else ""))
    counts: Dict[str, int] = {}
    for _, rec, reasons in bad:
        if rec is not None:
            for kind, _detail in reasons:
                counts[kind] = counts.get(kind, 0) + 1
    for kind, n in sorted(counts.items(), key=lambda kv: -kv[1]):
        report.append("  %d frame(s): %s" % (n, kind))
    shown = 0
    for f, rec, reasons in bad:
        if rec is None or shown >= DIAG_TOP:
            continue
        shown += 1
        report += ["  " + ln for ln in rec.describe()]
        report += ["    not diagnosed: %s" % detail for _kind, detail in reasons]
    return False, report


def cmd_threads(frames: Sequence[Frame], records: Sequence[Record], cls: str,
                comm: str, buckets: Sequence[str], min_frac: float,
                preemptor: str, blocked_in: Optional[re.Pattern],
                min_frames: int) -> Tuple[bool, List[str]]:
    by_id, _ = index_records(records)
    sel = [f for f in frames if f.injected == cls]
    good, bad = [], []
    for f in sel:
        rec = by_id.get(f.frame_id)
        reasons = check_threads(rec, comm, buckets, min_frac, preemptor,
                                blocked_in)
        (good if not reasons else bad).append((f, rec, reasons))

    want = ["a thread matching %r" % comm if comm else "some thread"]
    if buckets:
        want.append("in %s with >= %.2f of its off-CPU time"
                    % (" or ".join(enum_name(b) for b in buckets), min_frac))
    if preemptor:
        want.append("preempted by %r" % preemptor)
    if blocked_in is not None:
        want.append("blocked in /%s/" % blocked_in.pattern)
    head = ("%d/%d frame(s) injected with %s carry %s"
            % (len(good), len(sel), cls, ", ".join(want)))
    if len(good) >= min_frames and sel:
        return True, ["ok: " + head + " (need %d)" % min_frames]

    report = ["FAILED threads %s: %s, need %d" % (cls, head, min_frames)]
    if not sel:
        report.append("  the ground truth has no frame injected with %r "
                      "(classes: %s)"
                      % (cls, ", ".join(sorted({f.injected for f in frames
                                                if f.injected})) or "none"))
        return False, report
    norec = [t for t in bad if t[1] is None]
    if norec:
        report.append("  %d of the %d injected frame(s) produced no record "
                      "(frame_ids %s%s)"
                      % (len(norec), len(sel),
                         ", ".join(str(t[0].frame_id) for t in norec[:DIAG_TOP]),
                         " ..." if len(norec) > DIAG_TOP else ""))
    counts: Dict[str, int] = {}
    for _, rec, reasons in bad:
        if rec is not None:
            for kind, _detail in reasons:
                counts[kind] = counts.get(kind, 0) + 1
    for kind, n in sorted(counts.items(), key=lambda kv: -kv[1]):
        report.append("  %d frame(s): %s" % (n, kind))
    shown = 0
    for f, rec, reasons in bad:
        if rec is None or shown >= DIAG_TOP:
            continue
        shown += 1
        report += ["  " + ln for ln in rec.describe(threads=True)]
        report += ["    not diagnosed: %s" % detail for _kind, detail in reasons]
    return False, report


def cmd_quiet(frames: Sequence[Frame], records: Sequence[Record],
              max_false: int, budget_ns: Optional[int]) -> Tuple[bool, List[str]]:
    by_id, _ = index_records(records)
    frame_ids = {f.frame_id for f in frames}
    clean = [f for f in frames if f.injected is None]
    noisy = [f for f in clean if f.frame_id in by_id]
    unmatched = [r for r in records if r.frame_id not in frame_ids]

    if budget_ns is None and records:
        budget_ns = sorted(r.budget_ns for r in records)[len(records) // 2]
    over = [f for f in clean
            if budget_ns and f.frame_ns and f.frame_ns > budget_ns]
    timed = [f for f in clean if f.frame_ns is not None]

    # A record for an uninjected frame that really did blow the budget is the
    # gate doing its job, not a false positive: only frames the ground truth
    # timed within budget count against it.
    over_ids = {f.frame_id for f in over}
    honest = [f for f in noisy if f.frame_id in over_ids]
    noisy = [f for f in noisy if f.frame_id not in over_ids]

    lines = ["%d of %d uninjected frame(s) produced a record for a frame that was "
             "within budget (%s), at most %d allowed"
             % (len(noisy), len(clean), _pct(len(noisy), len(clean)), max_false)]
    if honest:
        lines.append("%d further record(s) are uninjected frames that the ground "
                     "truth also puts over budget (the gate was right)"
                     % len(honest))
    if budget_ns and timed:
        lines.append("ground truth: %d of %d uninjected frame(s) were over the "
                     "%s budget (%s)"
                     % (len(over), len(timed), _ns(budget_ns),
                        _pct(len(over), len(timed))))
    else:
        lines.append("ground truth: no budget or no frame times available, "
                     "over-budget-but-uninjected rate unknown")
    if unmatched:
        lines.append("note: %d record(s) name a frame_id absent from the ground "
                     "truth (frame_ids %s)"
                     % (len(unmatched),
                        ", ".join(str(r.frame_id) for r in unmatched[:DIAG_TOP])))
    if len(noisy) <= max_false:
        return True, ["ok: " + lines[0]] + lines[1:]

    report = ["FAILED quiet: " + lines[0]] + lines[1:]
    for f in noisy[:DIAG_TOP]:
        report += ["  " + ln for ln in by_id[f.frame_id].describe()]
    if len(noisy) > DIAG_TOP:
        report.append("  ... %d more" % (len(noisy) - DIAG_TOP))
    return False, report


def cmd_present(records: Sequence[Record], min_frac: float,
                min_records: int) -> Tuple[bool, List[str]]:
    """Assert the pacing wait is named after the display, not after a syscall.

    Between the frame marker (present entry) and the present-end marker the
    root is waiting for the display, so a paced app spends the wait of its
    healthy frames in HT_BLOCK_PRESENT. A bracket that never opened leaves
    that time named after whatever the app blocked in -- HT_BLOCK_TIMER for
    a loop paced with a sleep -- which is what this check is here to catch.
    """
    good = [r for r in records if r.bucket("block_present")]
    bad = [r for r in records if not r.bucket("block_present")]
    head = ("%d of %d record(s) spend their wait in HT_BLOCK_PRESENT (%s), "
            "need %.2f of them"
            % (len(good), len(records), _pct(len(good), len(records)),
               min_frac))
    context = []
    if good:
        present = [r.bucket("block_present") for r in good]
        context.append("present time per record: median %s, largest %s"
                       % (_ns(int(median(present))), _ns(max(present))))

    if len(records) < min_records:
        return False, ["FAILED present: %d record(s), need %d before the "
                       "pacing can be judged"
                       % (len(records), min_records), head] + context
    if not records:
        return True, ["ok: no records to check: no frame went over budget, "
                      "so nothing here says where the pacing went"]
    if len(good) >= min_frac * len(records):
        return True, ["ok: " + head] + context

    report = ["FAILED present: " + head]
    counts: Dict[str, int] = {}
    for rec in bad:
        named = largest_blocked(rec.cause_ns)
        key = enum_name(named) if named else "(never blocked)"
        counts[key] = counts.get(key, 0) + 1
    tally = ", ".join("%s (%d)" % (name, n) for name, n
                      in sorted(counts.items(), key=lambda kv: -kv[1]))
    report.append("the %d record(s) without present time name their blocked "
                  "time %s instead" % (len(bad), tally))
    report += context
    for rec in bad[:DIAG_TOP]:
        report += ["  " + ln for ln in rec.describe()]
    if len(bad) > DIAG_TOP:
        report.append("  ... %d more" % (len(bad) - DIAG_TOP))
    return False, report


def thread_problems(rec: Record, tol_ns: int) -> List[str]:
    """Where the record's per-thread detail contradicts the record itself.

    Nothing to say about a record that reports no threads. Only the root's
    line is checked against the top-level buckets: the other threads ran in
    parallel, so their time is not the frame's.
    """
    if not rec.threads:
        return []
    problems = []
    roots = rec.roots()
    if len(roots) != 1:
        problems.append("%d of %d thread(s) are flagged as the root (%s), "
                        "expected exactly 1"
                        % (len(roots), len(rec.threads),
                           ", ".join(t.label() for t in roots) or "none"))
    if len(roots) != 1:
        return problems
    root = roots[0]
    off = ["%s: root %s, record %s (off by %s)"
           % (BUCKETS[i], _ns(root.cause_ns[i]), _ns(rec.cause_ns[i]),
              _ns(abs(root.cause_ns[i] - rec.cause_ns[i])))
           for i in range(PARTITION)
           if abs(root.cause_ns[i] - rec.cause_ns[i]) > tol_ns]
    if off:
        problems.append("the root thread %s and the record disagree on the "
                        "same timeline, tolerance %s: %s"
                        % (root.label(), _ns(tol_ns), "; ".join(off)))
    return problems


def oncpu_stall_problems(who: str, cause_ns: Sequence[int], frame_ns: int,
                         tol_ns: int) -> List[str]:
    """--oncpu-stall: the carve-outs weighed against the frame they came from.

    Page faults, reclaim and compaction are carved out of HT_ONCPU when the
    frame closes, so together they cannot outlast the frame, and what is left
    in HT_ONCPU cannot have gone below zero. The counters are unsigned, so an
    underflow arrives as a bucket far larger than the frame, not a negative
    one.
    """
    problems = []
    stalls = sum(cause_ns[i] for i in ONCPU_STALLS)
    if stalls - frame_ns > tol_ns:
        part = "  ".join("%s %s" % (BUCKETS[i], _ns(cause_ns[i]))
                         for i in ONCPU_STALLS if cause_ns[i])
        problems.append("%sthe on-CPU stalls sum to %s, more than the %s "
                        "frame (%s)"
                        % (who, _ns(stalls), _ns(frame_ns), part))
    if cause_ns[ONCPU] - frame_ns > tol_ns:
        problems.append("%sHT_ONCPU is %s, more than the %s frame: the "
                        "carve-out took it below zero"
                        % (who, _ns(cause_ns[ONCPU]), _ns(frame_ns)))
    return problems


def no_present_problems(rec: Record) -> List[str]:
    """--no-present: with no present-end probe the bracket cannot be open.

    HT_BLOCK_PRESENT is opened by the frame marker and closed by the present
    marker. A run without the second probe must leave the bucket empty; time
    in it means a bracket was opened that nothing could close, which is the
    failure that would otherwise quietly rename every later wait.
    """
    problems = []
    if rec.cause_ns[PRESENT]:
        problems.append("HT_BLOCK_PRESENT holds %s although the present "
                        "marker was disabled: the bracket opened with no "
                        "probe able to close it"
                        % _ns(rec.cause_ns[PRESENT]))
    for th in rec.threads:
        if th.cause_ns[PRESENT]:
            problems.append("%s: HT_BLOCK_PRESENT holds %s although the "
                            "present marker was disabled"
                            % (th.label(), _ns(th.cause_ns[PRESENT])))
    return problems


def cmd_invariant(records: Sequence[Record], tol_ns: int,
                  oncpu_stall: bool = False,
                  no_present: bool = False) -> Tuple[bool, List[str]]:
    bad: List[Tuple[Record, List[str]]] = []
    for rec in records:
        problems = []
        total = sum(rec.cause_ns[:PARTITION])
        if abs(total - rec.frame_ns) > tol_ns:
            problems.append("partition sums to %s, frame_ns is %s (off by %s, "
                            "tolerance %s)"
                            % (_ns(total), _ns(rec.frame_ns),
                               _ns(abs(total - rec.frame_ns)), _ns(tol_ns)))
        resolved = rec.cause_ns[WAIT_ONCPU] + rec.cause_ns[INHERITED]
        block = blocked_ns(rec.cause_ns)
        if resolved - block > tol_ns:
            problems.append("resolved %s (wait_oncpu %s + inherited %s) exceeds "
                            "the %s the frame spent blocked"
                            % (_ns(resolved), _ns(rec.cause_ns[WAIT_ONCPU]),
                               _ns(rec.cause_ns[INHERITED]), _ns(block)))
        if oncpu_stall:
            problems += oncpu_stall_problems("", rec.cause_ns, rec.frame_ns,
                                             tol_ns)
            for th in rec.threads:
                problems += oncpu_stall_problems("%s: " % th.label(),
                                                 th.cause_ns, rec.frame_ns,
                                                 tol_ns)
        if no_present:
            problems += no_present_problems(rec)
        problems += thread_problems(rec, tol_ns)
        if problems:
            bad.append((rec, problems))

    with_threads = sum(1 for r in records if r.threads)
    if not bad:
        if not records:
            return True, ["ok: no records to check"]
        extra = "; on-CPU stalls fit the frame" if oncpu_stall else ""
        if no_present:
            extra += "; nothing named HT_BLOCK_PRESENT"
        return True, ["ok: %d record(s) partition frame_ns within %s "
                      "(%d with per-thread detail%s)"
                      % (len(records), _ns(tol_ns), with_threads, extra)]
    report = ["FAILED invariant: %d of %d record(s) do not add up"
              % (len(bad), len(records))]
    for rec, problems in bad[:DIAG_TOP]:
        report += ["  " + ln for ln in rec.describe(threads=bool(rec.threads))]
        report += ["    %s" % p for p in problems]
    if len(bad) > DIAG_TOP:
        report.append("  ... %d more" % (len(bad) - DIAG_TOP))
    return False, report


# --------------------------------------------------------------------------
# CLI


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    j = sub.add_parser("join", help="per-class hit table, for eyeballing a run")
    j.add_argument("ground_truth")
    j.add_argument("records")
    j.add_argument("--verbose", action="store_true",
                   help="also print the frames that missed")

    e = sub.add_parser("expect", help="assert an injector class is diagnosed")
    e.add_argument("ground_truth")
    e.add_argument("records")
    e.add_argument("--class", dest="cls", required=True,
                   help="injector class, as the ground truth spells it")
    e.add_argument("--bucket", required=True,
                   help="expected largest partition bucket, e.g. HT_BLOCK_TASK")
    e.add_argument("--resolved", choices=("inherited", "wait_oncpu"),
                   help="the frame's blocked time must be dominated by this "
                        "resolution")
    e.add_argument("--chain", default="",
                   help="comma-separated hop comms, direct waker first; "
                        "substring per hop, '*' matches any hop")
    e.add_argument("--min-frac", type=float, default=0.5,
                   help="share of the frame's excess the bucket must carry "
                        "(default 0.5)")
    e.add_argument("--min-frames", type=int, default=1,
                   help="how many injected frames must pass (default 1)")
    e.add_argument("--any-size", action="store_true",
                   help="the bucket need not be the largest, only carry "
                        "--min-frac of the excess (for a cause that shares a "
                        "frame with unavoidable work, e.g. page faults)")

    t = sub.add_parser("threads",
                       help="assert the per-thread detail of a class")
    t.add_argument("ground_truth")
    t.add_argument("records")
    t.add_argument("--class", dest="cls", required=True,
                   help="injector class, as the ground truth spells it")
    t.add_argument("--thread", default="",
                   help="substring of the comm of the thread to look at "
                        "(default: any thread of the record)")
    g = t.add_mutually_exclusive_group()
    g.add_argument("--bucket",
                   help="the thread's largest partition bucket, "
                        "e.g. HT_BLOCK_TASK")
    g.add_argument("--bucket-any", dest="bucket_any", default="",
                   help="comma-separated buckets, any of which the thread's "
                        "largest may be")
    t.add_argument("--min-frac", type=float, default=0.5,
                   help="share of the thread's own off-CPU time the bucket "
                        "must carry (default 0.5)")
    t.add_argument("--preemptor", default="",
                   help="substring of the comm of the thread that took this "
                        "one's CPU")
    t.add_argument("--blocked-in", dest="blocked_in", default="",
                   help="regular expression the kernel function of the "
                        "thread's longest off-CPU interval must match")
    t.add_argument("--min-frames", type=int, default=1,
                   help="how many injected frames must pass (default 1)")

    q = sub.add_parser("quiet", help="assert the gate ignores normal frames")
    q.add_argument("ground_truth")
    q.add_argument("records")
    q.add_argument("--max-false", type=int, default=0,
                   help="uninjected frames allowed to produce a record "
                        "(default 0)")
    q.add_argument("--budget-us", type=int,
                   help="frame budget for the ground-truth over-budget rate "
                        "(default: the median budget_ns of the records)")

    pr = sub.add_parser("present",
                        help="assert the pacing wait is named after the "
                             "display")
    pr.add_argument("records")
    pr.add_argument("--min-frac", type=float, default=0.5,
                    help="share of the records that must carry "
                         "HT_BLOCK_PRESENT time (default 0.5)")
    pr.add_argument("--min-records", type=int, default=0,
                    help="records needed before the check can judge anything "
                         "(default 0: no record is no evidence either way)")

    i = sub.add_parser("invariant", help="assert the buckets add up")
    i.add_argument("records")
    i.add_argument("--tol-us", type=int, default=500,
                   help="tolerance in microseconds (default 500)")
    i.add_argument("--oncpu-stall", dest="oncpu_stall", action="store_true",
                   help="also assert the buckets carved out of HT_ONCPU "
                        "(faults, reclaim, compaction) fit inside the frame "
                        "and left HT_ONCPU itself no larger than it")
    i.add_argument("--no-present", dest="no_present", action="store_true",
                   help="assert nothing is named HT_BLOCK_PRESENT, as "
                        "nothing can be when the tool ran with the present "
                        "marker disabled")
    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        records, skipped = read_records(args.records)
        frames: List[Frame] = []
        if args.cmd not in ("invariant", "present"):
            frames, _ = read_ground_truth(args.ground_truth)
            start, end = read_window(args.records)
            traced = in_window(frames, start, end)
            if len(traced) != len(frames):
                print("(ground truth: %d of %d frame(s) fall inside the trace "
                      "window)" % (len(traced), len(frames)))
            frames = traced
        if args.cmd == "expect":
            try:
                bucket = normalize_bucket(args.bucket)
            except ValueError as e:
                parser.error(str(e))
            if args.min_frac < 0 or args.min_frac > 1:
                parser.error("--min-frac must be between 0 and 1")
            resolved = (None if args.resolved is None
                        else normalize_bucket(args.resolved))
            ok, report = cmd_expect(frames, records, args.cls, bucket,
                                    args.min_frac, args.min_frames, resolved,
                                    split_chain(args.chain),
                                    need_largest=not args.any_size)
        elif args.cmd == "threads":
            names = ([args.bucket] if args.bucket
                     else split_chain(args.bucket_any))
            try:
                buckets = [normalize_bucket(b) for b in names]
            except ValueError as e:
                parser.error(str(e))
            for b in buckets:
                if IDX[b] >= PARTITION:
                    parser.error("%s is a resolution bucket, not one a thread "
                                 "reports" % enum_name(b))
            if args.min_frac < 0 or args.min_frac > 1:
                parser.error("--min-frac must be between 0 and 1")
            blocked_in = None
            if args.blocked_in:
                try:
                    blocked_in = re.compile(args.blocked_in)
                except re.error as e:
                    parser.error("--blocked-in: %s" % e)
            ok, report = cmd_threads(frames, records, args.cls, args.thread,
                                     buckets, args.min_frac, args.preemptor,
                                     blocked_in, args.min_frames)
        elif args.cmd == "present":
            if args.min_frac < 0 or args.min_frac > 1:
                parser.error("--min-frac must be between 0 and 1")
            ok, report = cmd_present(records, args.min_frac, args.min_records)
        elif args.cmd == "join":
            ok, report = cmd_join(frames, records, args.verbose)
        elif args.cmd == "quiet":
            budget = None if args.budget_us is None else args.budget_us * 1000
            ok, report = cmd_quiet(frames, records, args.max_false, budget)
        else:
            ok, report = cmd_invariant(records, args.tol_us * 1000,
                                       args.oncpu_stall, args.no_present)
    except ParseError as e:
        print("check_hitches: %s" % e, file=sys.stderr)
        return 2
    except (OSError, ValueError) as e:
        print("check_hitches: %s" % e, file=sys.stderr)
        return 2

    for line in report:
        print(line)
    if skipped:
        print("(ignored %d non-record line(s) in %s)" % (skipped, args.records))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
