#!/usr/bin/env bash
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#
# End-to-end tests for the hitchtrace prototype: one run of hitchbench per
# injector class, hitchtrace attached to it, then check_hitches.py joins
# hitchbench's per-frame ground truth to hitchtrace's records.
#
#   make                              # build, as your normal user
#   sudo tests/run_hitch_tests.sh     # every injector class
#   sudo tests/run_hitch_tests.sh worker_block quiet_baseline
#
# This script never builds anything. Outputs go to tests/out/:
#   <class>.gt.jsonl      hitchbench ground truth, one object per frame
#   <class>.hitch.jsonl   hitchtrace records, one object per over-budget frame
#   <class>.bench.log     hitchbench stdout/stderr
#   <class>.stderr        hitchtrace stdout/stderr
#
# How the two binaries are driven (tests/hitchbench.c, src/hitchtrace.c):
#
#   build/hitchbench -c CLASS -d SECS -o GT.jsonl
#	frame loop injecting CLASS into every 20th frame, ground truth to
#	GT.jsonl, and on stdout once the loop is up:
#	    hitchbench pid=<pid> root_tid=<tid> binary=<exe> marker=<sym>
#		budget_us=<n> present_marker=<sym>
#	The binary, the budget and the markers are read back and handed to
#	hitchtrace, so the two agree on the budget and on the uprobe targets;
#	a report without present_marker= simply leaves hitchtrace's own
#	default in place. The baseline class runs with -i 0 (never inject)
#	instead of -c.
#   build/hitchtrace -p PID -b BUDGET_US -o OUT.jsonl -d SECS [-x BIN -m SYM]
#			[-M PRESENT_SYM]
#	traces PID for SECS seconds and appends one JSON record per
#	over-budget frame to OUT.jsonl (appends: the file is removed first).
#	-m is present entry and -M present return: between the two the root's
#	blocked time is HT_BLOCK_PRESENT, the display pacing the app. Rows
#	leave -M at hitchtrace's default unless the table says otherwise.
#
# Environment overrides:
#   TRACE_SECS   seconds hitchtrace traces (default 5, which keeps the whole
#                table near the wall-clock time it took before the poll and
#                fault classes joined it; a class still sees ~300 frames)
#   BENCH_SECS   hitchbench lifetime (default TRACE_SECS + 3; it must outlive
#                hitchtrace so the uprobe target stays mapped)
#   WARMUP_SECS  delay between hitchbench being up and hitchtrace starting
#   MIN_FRAC     share of a frame's excess the expected bucket must carry,
#                and, for the per-thread check, the share of a thread's own
#                off-CPU time its expected bucket must carry
#   MIN_FRAMES   diagnosed frames required per class
#   MAX_FALSE    uninjected frames allowed to produce a record. The gate is
#                supposed to be quiet on normal frames, but hitchbench paces
#                only 1/8 under budget, so on a busy machine some normal
#                frames really do overrun; each quiet check prints that rate
#                from the ground truth next to its own verdict.
#   PRESENT_FRAC share of a paced row's records that must spend their wait in
#                HT_BLOCK_PRESENT (check_hitches.py present)
#   TOL_US       partition tolerance for the invariant check, which also
#                weighs the on-CPU stall buckets against the frame
#                (check_hitches.py invariant --oncpu-stall)
#   BUDGET_US    frame budget, overriding what hitchbench reports
#   CLASSES      space-separated class list, overriding the table below
#   HITCHBENCH_ARGS, HITCHTRACE_ARGS   extra arguments (e.g. -i N to inject
#                more often, -U for user stacks)
#   HITCHBENCH, HITCHTRACE, CHECK_PY   tool paths
#
# Exit status: 0 all classes passed, 1 some class failed, 2 cannot run.

set -u

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
HITCHTRACE=${HITCHTRACE:-$ROOT/build/hitchtrace}
HITCHBENCH=${HITCHBENCH:-$ROOT/build/hitchbench}
CHECK_PY=${CHECK_PY:-$ROOT/tests/check_hitches.py}
OUT=$ROOT/tests/out

TRACE_SECS=${TRACE_SECS:-5}
BENCH_SECS=${BENCH_SECS:-$((TRACE_SECS + 3))}
WARMUP_SECS=${WARMUP_SECS:-1}
MIN_FRAC=${MIN_FRAC:-0.5}
MIN_FRAMES=${MIN_FRAMES:-3}
MAX_FALSE=${MAX_FALSE:-5}
PRESENT_FRAC=${PRESENT_FRAC:-0.5}
TOL_US=${TOL_US:-500}
BUDGET_US=${BUDGET_US:-}		# default: what hitchbench reports
BENCH_READY_SECS=10			# wait this long for the pid report
HT_TIMEOUT=$((TRACE_SECS + 60))		# watchdog for a hung hitchtrace

read -r -a EXTRA_BENCH_ARGS <<<"${HITCHBENCH_ARGS:-}"
read -r -a EXTRA_TRACE_ARGS <<<"${HITCHTRACE_ARGS:-}"

# One row per test:
#
#   name|bucket|resolved|chain|min-frames|thread checks|hitchbench arguments
#       |expect extras|present mode
#
# `name` is the test name, and the injector class the ground truth reports
# unless the row's hitchbench arguments name another one with -c; `bucket` is
# the partition bucket the injected stall must land in and be the largest of,
# and an empty `bucket` runs only the quiet and invariant checks. `resolved`
# is inherited/wait_oncpu (how the time the root spent blocked must be
# explained: any wait a task ended is resolved through that waker, whichever
# HT_BLOCK_* bucket the wait itself landed in), and `chain` a comma-separated
# list of hop comms, direct waker first.
# `thread checks` are arguments to `check_hitches.py threads`, which
# asserts the record's per-thread detail (struct ht_thread) rather than the
# root's own timeline; an empty field skips that check, and --class and
# --min-frames are added from this row.
# `present mode` is empty for a row traced the way hitchtrace defaults it,
# `paced` for one whose records must spend their wait in HT_BLOCK_PRESENT
# (check_hitches.py present), or `off` for one traced with the present marker
# disabled (-M ""), which must then leave that bucket empty everywhere
# (check_hitches.py invariant --no-present). The classes and their
# expected buckets are hitchbench's own table (tests/hitchbench.c,
# `build/hitchbench -l`); the bucket names are enum ht_cause from
# src/hitchtrace.h.
#
# Both worker classes have the root waiting on a condvar, so the root's own
# wait is a futex wait (HT_BLOCK_FUTEX) whichever way the worker is stalled;
# what tells them apart is the resolution, inherited for a worker that was
# itself blocked and wait_oncpu for one that was merely computing.
#
# The fault class never leaves the CPU: its frame is on-CPU from end to end,
# and what makes it a hitch is the share of that time spent in minor page
# faults, which frame close carves out of HT_ONCPU into HT_ONCPU_FAULT. The
# invariant check below weighs those carve-outs against the frame.
#
# The frame marker is present entry, so the pace sleep that follows it is the
# display holding the app back rather than the app sleeping: present_long
# makes that wait long enough to blow the budget (HT_BLOCK_PRESENT), and the
# paced baseline must show the ordinary pace sleep in the same bucket. A
# bracket that never opened would name that time after the syscall it blocked
# in -- HT_BLOCK_TIMER for a sleep-paced loop -- which is what the `present`
# check fails on. present_off is the other half: the bracket is opened by one
# probe and closed by another, so a run with no present marker must leave the
# bucket empty while still naming the sleep class HT_BLOCK_TIMER. It runs the
# sleep class under its own name, so both rows have their own outputs.
#
# The two classes with a thread check are the ones where the per-thread view
# says something the record alone does not: in worker_block the frame's stall
# belongs to hb_worker, which blocks on the feeder's pipe, and the rules name
# that wait after whatever ended it -- HT_BLOCK_TASK for the feeder's write,
# HT_BLOCK_FUTEX or HT_BLOCK_TIMER when the wake is credited to the futex the
# worker parks on or to the hrtimer that started the chain, so any of the
# three answers. In preempt the root is runnable while the pinned hogs hold
# the CPU, which only the preemptor field names.
CLASS_SPECS=(
	"sleep|HT_BLOCK_TIMER|||$MIN_FRAMES||-c sleep"
	"worker_block|HT_BLOCK_FUTEX|inherited|hb_worker,hb_feeder|$MIN_FRAMES|--thread hb_worker --bucket-any HT_BLOCK_TASK,HT_BLOCK_FUTEX,HT_BLOCK_TIMER --min-frac $MIN_FRAC|-c worker_block"
	"worker_cpu|HT_BLOCK_FUTEX|wait_oncpu|hb_worker|$MIN_FRAMES||-c worker_cpu"
	"cpu_spike|HT_ONCPU|||$MIN_FRAMES||-c cpu_spike"
	"preempt|HT_RUNNABLE|||$MIN_FRAMES|--thread hb_root --preemptor hb_hog|-c preempt"
	"thousand_cuts|HT_BLOCK_TIMER|||$MIN_FRAMES||-c thousand_cuts"
	"io|HT_BLOCK_IO|||$MIN_FRAMES||-c io"
	"poll|HT_BLOCK_POLL|||$MIN_FRAMES||-c poll"
	"fault|HT_ONCPU_FAULT|||$MIN_FRAMES||-c fault|--any-size --min-frac 0.30"
	"present_long|HT_BLOCK_PRESENT|||$MIN_FRAMES||-c present_long"
	"present_off|HT_BLOCK_TIMER|||$MIN_FRAMES||-c sleep||off"
	"quiet_baseline||||||-i 0||paced"
)

ALL_CLASSES=()
for spec in "${CLASS_SPECS[@]}"; do
	ALL_CLASSES+=("${spec%%|*}")
done

# ---------------------------------------------------------------- preflight

if [[ $(id -u) -ne 0 ]]; then
	echo "run_hitch_tests.sh: must be run as root (loading BPF programs), e.g.:" >&2
	echo "  sudo $0" >&2
	exit 2
fi

missing=0
for bin in "$HITCHTRACE" "$HITCHBENCH"; do
	if [[ ! -x $bin ]]; then
		echo "run_hitch_tests.sh: $bin is missing" >&2
		missing=1
	fi
done
if ((missing)); then
	echo "run_hitch_tests.sh: this script does not build; first run, as your normal user:" >&2
	echo "  make -C $ROOT" >&2
	exit 2
fi
if [[ ! -f $CHECK_PY ]]; then
	echo "run_hitch_tests.sh: $CHECK_PY is missing" >&2
	exit 2
fi
if ! command -v python3 >/dev/null; then
	echo "run_hitch_tests.sh: python3 is required" >&2
	exit 2
fi

if [[ -n ${CLASSES:-} ]]; then
	read -r -a SELECTED <<<"$CLASSES"
elif (($# == 0)); then
	SELECTED=("${ALL_CLASSES[@]}")
else
	SELECTED=()
	for arg in "$@"; do
		found=0
		for c in "${ALL_CLASSES[@]}"; do
			if [[ $c == "$arg" || $c == "${arg}_"* ]]; then
				SELECTED+=("$c")
				found=1
			fi
		done
		if ((!found)); then
			echo "run_hitch_tests.sh: unknown class '$arg' (classes: ${ALL_CLASSES[*]})" >&2
			exit 2
		fi
	done
fi

mkdir -p -- "$OUT" || exit 2

# ------------------------------------------------------ process bookkeeping

BG_PIDS=()		# background processes we started and still own
BENCH_PID=
TARGET_PID=
ROOT_TID=
BENCH_BINARY=		# what hitchbench reports about itself, handed on
BENCH_MARKER=		# to hitchtrace so both probe the same function
BENCH_PRESENT=		# likewise for the present-return marker, if reported
BENCH_BUDGET_US=	# and score frames against the same budget

bg_forget() {
	local pid=$1 p keep=()

	for p in "${BG_PIDS[@]}"; do
		[[ $p == "$pid" ]] || keep+=("$p")
	done
	BG_PIDS=("${keep[@]}")
}

cleanup() {
	local pid

	for pid in "${BG_PIDS[@]}"; do
		kill -TERM "$pid" 2>/dev/null
	done
	for pid in "${BG_PIDS[@]}"; do
		wait "$pid" 2>/dev/null
	done
	BG_PIDS=()
	if [[ -n ${SUDO_USER:-} && -d $OUT ]]; then
		chown -R -- "$SUDO_USER:" "$OUT" 2>/dev/null ||
			chown -R -- "$SUDO_USER" "$OUT"
	fi
}
trap cleanup EXIT
trap 'echo; echo "run_hitch_tests.sh: interrupted" >&2; exit 130' INT
trap 'exit 143' TERM

# ------------------------------------------------------- result reporting

PASS_COUNT=0
FAIL_COUNT=0
FAILED_CLASSES=()

t_begin() {
	T_NAME=$1
	T_OK=1
	T_INFO=""
	T_DIAG=""
	T_ERRFILES=()
	rm -f -- "$OUT/$T_NAME".{gt.jsonl,hitch.jsonl,bench.log,stderr}
}

# t_info LINE: context printed under the PASS/FAIL line
t_info() {
	T_INFO+="  $1"$'\n'
}

# t_fail MESSAGE [DETAIL...]: mark the class failed; details are indented
t_fail() {
	local d

	T_OK=0
	T_DIAG+="  FAILED: $1"$'\n'
	shift
	for d in "$@"; do
		[[ -n $d ]] || continue
		T_DIAG+=$(printf '%s\n' "$d" | sed 's/^/    /')$'\n'
	done
}

t_end() {
	local f n

	if ((T_OK)); then
		PASS_COUNT=$((PASS_COUNT + 1))
		echo "PASS $T_NAME"
		printf '%s' "$T_INFO"
		return
	fi
	FAIL_COUNT=$((FAIL_COUNT + 1))
	FAILED_CLASSES+=("$T_NAME")
	echo "FAIL $T_NAME"
	printf '%s' "$T_INFO"
	printf '%s' "$T_DIAG"
	for f in "${T_ERRFILES[@]}"; do
		[[ -f $f ]] || continue
		n=$(wc -l <"$f")
		echo "  --- ${f#"$ROOT"/} ($n lines$( ((n > 40)) && echo ", last 40"))"
		tail -n 40 -- "$f" | sed 's/^/  | /'
	done
}

# check SUBCOMMAND ARGS...: run check_hitches.py and record the verdict
check() {
	local out rc line

	out=$(python3 "$CHECK_PY" "$@" 2>&1)
	rc=$?
	if ((rc != 0)); then
		t_fail "check_hitches.py $1 (status $rc)" "$out"
		return
	fi
	while IFS= read -r line; do		# the verdict and its context
		t_info "$line"
	done < <(printf '%s\n' "$out" | head -n 3)
}

# report SUBCOMMAND ARGS...: run check_hitches.py for information only
report() {
	local out line

	out=$(python3 "$CHECK_PY" "$@" 2>&1) || true
	while IFS= read -r line; do
		t_info "$line"
	done <<<"$out"
}

# ------------------------------------------------------------- workload

stop_bench() {
	[[ -n $BENCH_PID ]] || return 0
	kill -TERM "$BENCH_PID" 2>/dev/null
	wait "$BENCH_PID" 2>/dev/null
	bg_forget "$BENCH_PID"
	BENCH_PID=
}

# start_bench ARG...: start hitchbench with these injector arguments and wait
# for its pid report; sets BENCH_PID, TARGET_PID, ROOT_TID and, when the
# report carries them, BENCH_BINARY, BENCH_MARKER, BENCH_PRESENT and
# BENCH_BUDGET_US.
# Returns non-zero on failure.
start_bench() {
	local log="$OUT/$T_NAME.bench.log" gt="$OUT/$T_NAME.gt.jsonl" i line
	local bench_args=("$@")

	TARGET_PID=
	ROOT_TID=
	BENCH_BINARY=
	BENCH_MARKER=
	BENCH_PRESENT=
	BENCH_BUDGET_US=
	T_ERRFILES+=("$log")
	"$HITCHBENCH" "${bench_args[@]}" -d "$BENCH_SECS" -o "$gt" \
		"${EXTRA_BENCH_ARGS[@]}" >"$log" 2>&1 &
	BENCH_PID=$!
	BG_PIDS+=("$BENCH_PID")

	for ((i = 0; i < BENCH_READY_SECS * 5; i++)); do
		line=$(grep -m 1 'hitchbench pid=' "$log" 2>/dev/null) && break
		kill -0 "$BENCH_PID" 2>/dev/null || break
		sleep 0.2
	done
	line=${line:-$(grep -m 1 'hitchbench pid=' "$log" 2>/dev/null)}
	if [[ -n $line ]]; then
		TARGET_PID=$(printf '%s\n' "$line" |
			sed -n 's/.*hitchbench pid=\([0-9][0-9]*\).*/\1/p')
		ROOT_TID=$(printf '%s\n' "$line" |
			sed -n 's/.*root_tid=\([0-9][0-9]*\).*/\1/p')
		BENCH_BINARY=$(printf '%s\n' "$line" |
			sed -n 's/.* binary=\([^ ]*\).*/\1/p')
		BENCH_MARKER=$(printf '%s\n' "$line" |
			sed -n 's/.* marker=\([^ ]*\).*/\1/p')
		# " marker=" cannot match inside "present_marker=", so the two
		# read out of the same line without stepping on each other.
		BENCH_PRESENT=$(printf '%s\n' "$line" |
			sed -n 's/.* present_marker=\([^ ]*\).*/\1/p')
		BENCH_BUDGET_US=$(printf '%s\n' "$line" |
			sed -n 's/.* budget_us=\([0-9][0-9]*\).*/\1/p')
	fi
	if [[ -z $TARGET_PID || -z $ROOT_TID ]]; then
		t_fail "hitchbench ${bench_args[*]} did not report 'hitchbench pid=... root_tid=...' within ${BENCH_READY_SECS}s" \
		       "$(tail -n 20 -- "$log" 2>/dev/null)"
		stop_bench
		return 1
	fi
	[[ -n $BUDGET_US ]] && BENCH_BUDGET_US=$BUDGET_US
	t_info "hitchbench ${bench_args[*]} -d $BENCH_SECS: pid=$TARGET_PID root_tid=$ROOT_TID budget_us=${BENCH_BUDGET_US:-default}${BENCH_PRESENT:+ present_marker=$BENCH_PRESENT}"
	sleep "$WARMUP_SECS"
	return 0
}

# finish_bench: wait for hitchbench to finish on its own; it must exit 0
finish_bench() {
	local rc

	[[ -n $BENCH_PID ]] || return 0
	wait "$BENCH_PID"
	rc=$?
	bg_forget "$BENCH_PID"
	BENCH_PID=
	if ((rc != 0)); then
		t_fail "hitchbench exited with status $rc" \
		       "$(tail -n 20 -- "$OUT/$T_NAME.bench.log" 2>/dev/null)"
	fi
}

# run_hitchtrace OUTFILE [PRESENT_MODE]: trace TARGET_PID for TRACE_SECS
# seconds; PRESENT_MODE `off` runs with the present marker disabled.
run_hitchtrace() {
	local outfile=$1 present=${2:-} errfile="$OUT/$T_NAME.stderr"
	local pid rc note="" args=()

	# -o appends, so start from an empty file
	rm -f -- "$outfile"
	[[ -n $BENCH_BUDGET_US ]] && args+=(-b "$BENCH_BUDGET_US")
	[[ -n $BENCH_BINARY ]] && args+=(-x "$BENCH_BINARY")
	[[ -n $BENCH_MARKER ]] && args+=(-m "$BENCH_MARKER")
	if [[ $present == off ]]; then
		args+=(-M "")		# no probe to close the bracket
		note=" (present marker disabled: -M '')"
	elif [[ -n $BENCH_PRESENT ]]; then
		args+=(-M "$BENCH_PRESENT")
	fi
	T_ERRFILES+=("$errfile")
	t_info "hitchtrace -p $TARGET_PID ${args[*]} -o ${outfile#"$ROOT"/} -d $TRACE_SECS$note"
	# Background + wait keeps Ctrl-C responsive; the watchdog sends SIGINT
	# (hitchtrace flushes what it has) and SIGKILL 10 s later.
	timeout -s INT -k 10 "$HT_TIMEOUT" "$HITCHTRACE" -p "$TARGET_PID" \
		"${args[@]}" -o "$outfile" -d "$TRACE_SECS" \
		"${EXTRA_TRACE_ARGS[@]}" >>"$errfile" 2>&1 &
	pid=$!
	BG_PIDS+=("$pid")
	wait "$pid"
	rc=$?
	bg_forget "$pid"

	if ((rc == 124)); then
		t_fail "hitchtrace still running after ${HT_TIMEOUT}s; interrupted by the watchdog"
	elif ((rc != 0)); then
		t_fail "hitchtrace exited with status $rc"
	fi
	return "$rc"
}

# ------------------------------------------------------------------ tests

# run_class NAME BUCKET RESOLVED CHAIN MIN_FRAMES THREAD_ARGS BENCH_ARGS
#	    EXPECT_ARGS PRESENT_MODE
run_class() {
	local cls=$1 bucket=$2 resolved=$3 chain=$4 min_frames=$5 threads=$6
	local extra_expect=${8:-} present=${9:-}
	local gt="$OUT/$cls.gt.jsonl" hitch="$OUT/$cls.hitch.jsonl"
	local bench_args thread_args extra_args args=() inv=() gt_class=$cls i

	read -r -a bench_args <<<"$7"
	# The ground truth names the injector class, which is the row name for
	# every row running its own class; a row that runs another class under
	# its own name (present_off) says which with -c.
	for ((i = 0; i + 1 < ${#bench_args[@]}; i++)); do
		[[ ${bench_args[i]} == -c ]] && gt_class=${bench_args[i + 1]}
	done
	start_bench "${bench_args[@]}" || return
	run_hitchtrace "$hitch" "$present"
	finish_bench

	if [[ ! -s $gt ]]; then
		t_fail "hitchbench wrote no ground truth to ${gt#"$ROOT"/}"
		return
	fi
	if [[ ! -f $hitch ]]; then
		t_fail "hitchtrace wrote no ${hitch#"$ROOT"/}"
		return
	fi
	if [[ ! -s $hitch ]]; then
		t_info "note: ${hitch#"$ROOT"/} is empty (no frame went over budget)"
	fi

	report join "$gt" "$hitch"
	if [[ -n $bucket ]]; then
		args=(expect "$gt" "$hitch" --class "$gt_class" --bucket "$bucket"
		      --min-frac "$MIN_FRAC" --min-frames "${min_frames:-$MIN_FRAMES}")
		[[ -n $resolved ]] && args+=(--resolved "$resolved")
		[[ -n $chain ]] && args+=(--chain "$chain")
		if [[ -n $extra_expect ]]; then
			read -r -a extra_args <<<"$extra_expect"
			args+=("${extra_args[@]}")
		fi
		check "${args[@]}"
	fi
	if [[ -n $threads ]]; then
		read -r -a thread_args <<<"$threads"
		check threads "$gt" "$hitch" --class "$gt_class" \
		      "${thread_args[@]}" \
		      --min-frames "${min_frames:-$MIN_FRAMES}"
	fi
	args=(quiet "$gt" "$hitch" --max-false "$MAX_FALSE")
	[[ -n $BENCH_BUDGET_US ]] && args+=(--budget-us "$BENCH_BUDGET_US")
	check "${args[@]}"
	# A paced row waits for the display every frame, so its records must
	# say so; they are the over-budget frames, and there may be none.
	if [[ $present == paced ]]; then
		check present "$hitch" --min-frac "$PRESENT_FRAC"
	fi
	# Nothing attached a probe able to close the bracket, so nothing may
	# have been named after present: the never-closing-bracket regression.
	[[ $present == off ]] && inv=(--no-present)
	check invariant "$hitch" --tol-us "$TOL_US" --oncpu-stall "${inv[@]}"
}

# ------------------------------------------------------------------- main

echo "hitchtrace end-to-end tests on $(uname -r); outputs in ${OUT#"$ROOT"/}/"
echo "budget gate: ${TRACE_SECS}s traced per class, >= $MIN_FRAMES diagnosed frame(s), <= $MAX_FALSE false record(s)"
for cls in "${SELECTED[@]}"; do
	for spec in "${CLASS_SPECS[@]}"; do
		[[ ${spec%%|*} == "$cls" ]] || continue
		IFS='|' read -r c_name c_bucket c_resolved c_chain c_min \
			c_threads c_args c_extra c_present <<<"$spec"
		t_begin "$c_name"
		run_class "$c_name" "$c_bucket" "$c_resolved" "$c_chain" \
			"$c_min" "$c_threads" "$c_args" "$c_extra" \
			"$c_present"
		t_end
		break
	done
done

echo
echo "$PASS_COUNT passed, $FAIL_COUNT failed"
if ((FAIL_COUNT)); then
	echo "failed: ${FAILED_CLASSES[*]}"
	exit 1
fi
exit 0
