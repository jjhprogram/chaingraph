#!/usr/bin/env bash
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#
# End-to-end tests for chaingraph, driven by the synthetic chainload workload
# (cg_source -> cg_relay1 -> cg_relay2 -> cg_sink, connected by pipes).
#
#   make                          # build, as your normal user
#   sudo tests/run_tests.sh       # run all tests
#   sudo tests/run_tests.sh t1 t5 # run a subset (name or tN prefix)
#
# This script never builds anything. Outputs go to tests/out/:
#   <test>.folded / <test>.txt   chaingraph stdout
#   <test>.stderr                chaingraph (or flamegraph.pl) stderr
#   <test>.load                  chainload stdout/stderr
#   t1.svg                       chain graph rendered from the T1 output
#
# Environment overrides:
#   LOAD_SECS    workload lifetime per test (default 12). It must outlive
#                chaingraph, which resolves user symbols from live processes.
#   INTERVAL_MS  cg_source tick interval (default 10)
#   CHAINGRAPH, CHAINLOAD, FLAMEGRAPH   tool paths
#
# Exit status: 0 all tests passed, 1 some test failed, 2 cannot run.

set -u

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
CHAINGRAPH=${CHAINGRAPH:-$ROOT/build/chaingraph}
CHAINLOAD=${CHAINLOAD:-$ROOT/build/chainload}
FLAMEGRAPH=${FLAMEGRAPH:-$ROOT/FlameGraph/flamegraph.pl}
CHECK_PY=$ROOT/tests/check_chain.py
OUT=$ROOT/tests/out

LOAD_SECS=${LOAD_SECS:-12}
INTERVAL_MS=${INTERVAL_MS:-10}
WARMUP_SECS=1		# start tracing this long after the workload
TRACE_SECS=6
T4_TRACE_SECS=5
MIN_US=3000000		# the sink must be blocked >= 3 s of the 6 s traced
CG_TIMEOUT=$((TRACE_SECS + 60))	# watchdog for a hung chaingraph

ALL_TESTS=(t1_full_chain t2_depth_limit t3_pid_filter t4_kernel_stacks_human
	   t5_flamegraph_svg)

T1_OUT=$OUT/t1_full_chain.folded
T1_SVG=$OUT/t1.svg

# cg_sink <- cg_relay2 <- cg_relay1 <- cg_source <- [hardirq], with the stack
# frames that prove each link is what the workload did.
FULL_CHAIN_ARGS=(
	--target cg_sink
	--wakers 'cg_relay2,cg_relay1,cg_source,[hardirq]'
	--min-us "$MIN_US"
	--frame-regex '0:pipe_read|pipe_wait|anon_pipe_read'
	--frame-regex '0:cg_stage_sink'
	--frame-regex '1:pipe_write'
	--frame-regex '1:cg_stage_relay'
	--frame-regex '3:cg_stage_source'
	--frame-regex '4:hrtimer_wakeup'
)

# ---------------------------------------------------------------- preflight

if [[ $(id -u) -ne 0 ]]; then
	echo "run_tests.sh: must be run as root (loading BPF programs), e.g.:" >&2
	echo "  sudo $0" >&2
	exit 2
fi

missing=0
for bin in "$CHAINGRAPH" "$CHAINLOAD"; do
	if [[ ! -x $bin ]]; then
		echo "run_tests.sh: $bin is missing" >&2
		missing=1
	fi
done
if ((missing)); then
	echo "run_tests.sh: this script does not build; first run, as your normal user:" >&2
	echo "  make -C $ROOT" >&2
	exit 2
fi
if ! command -v python3 >/dev/null; then
	echo "run_tests.sh: python3 is required" >&2
	exit 2
fi

SELECTED=()
if (($# == 0)); then
	SELECTED=("${ALL_TESTS[@]}")
else
	for arg in "$@"; do
		found=0
		for t in "${ALL_TESTS[@]}"; do
			if [[ $t == "$arg" || $t == "${arg}_"* ]]; then
				SELECTED+=("$t")
				found=1
			fi
		done
		if ((!found)); then
			echo "run_tests.sh: unknown test '$arg' (tests: ${ALL_TESTS[*]})" >&2
			exit 2
		fi
	done
fi

mkdir -p -- "$OUT" || exit 2

# ------------------------------------------------------ process bookkeeping

BG_PIDS=()		# background processes we started and still own
LOAD_CHILDREN=()	# chainload stage pids, killed on exit if still around
LOAD_PID=
SINK_PID=

bg_forget() {
	local pid=$1 p keep=()

	for p in "${BG_PIDS[@]}"; do
		[[ $p == "$pid" ]] || keep+=("$p")
	done
	BG_PIDS=("${keep[@]}")
}

cleanup() {
	local pid comm

	for pid in "${BG_PIDS[@]}"; do
		kill -TERM "$pid" 2>/dev/null
	done
	for pid in "${LOAD_CHILDREN[@]}"; do
		comm=$(cat "/proc/$pid/comm" 2>/dev/null) || continue
		[[ $comm == cg_* ]] && kill -KILL "$pid" 2>/dev/null
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
trap 'echo; echo "run_tests.sh: interrupted" >&2; exit 130' INT
trap 'exit 143' TERM

# ------------------------------------------------------- result reporting

PASS_COUNT=0
FAIL_COUNT=0
FAILED_TESTS=()

t_begin() {
	T_NAME=$1
	T_OK=1
	T_INFO=""
	T_DIAG=""
	T_ERRFILES=()
	rm -f -- "$OUT/$T_NAME".{folded,txt,stderr,load}
}

# t_info LINE: context printed under the PASS/FAIL line
t_info() {
	T_INFO+="  $1"$'\n'
}

# t_fail MESSAGE [DETAIL...]: mark the test failed; details are indented
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
	FAILED_TESTS+=("$T_NAME")
	echo "FAIL $T_NAME"
	printf '%s' "$T_INFO"
	printf '%s' "$T_DIAG"
	for f in "${T_ERRFILES[@]}"; do
		[[ -f $f ]] || continue
		n=$(wc -l <"$f")
		echo "  --- ${f#"$ROOT"/} ($n lines$( ((n > 80)) && echo ", last 80"))"
		tail -n 80 -- "$f" | sed 's/^/  | /'
	done
}

# check SUBCOMMAND FILE ARGS...: run check_chain.py and record the verdict
check() {
	local out rc

	out=$(python3 "$CHECK_PY" "$@" 2>&1)
	rc=$?
	if ((rc == 0)); then
		t_info "$(printf '%s\n' "$out" | head -n 1)"
	else
		t_fail "check_chain.py $1 ${2#"$ROOT"/} (status $rc)" "$out"
	fi
}

# ------------------------------------------------------------- workload

stop_load() {
	[[ -n $LOAD_PID ]] || return 0
	kill -TERM "$LOAD_PID" 2>/dev/null
	wait "$LOAD_PID" 2>/dev/null
	bg_forget "$LOAD_PID"
	LOAD_PID=
}

# start_load: start chainload in the background and wait for its pid
# report; sets LOAD_PID and SINK_PID. Returns non-zero on failure.
start_load() {
	local log="$OUT/$T_NAME.load" name pid comm i ready

	SINK_PID=
	"$CHAINLOAD" -d "$LOAD_SECS" -i "$INTERVAL_MS" >"$log" 2>&1 &
	LOAD_PID=$!
	BG_PIDS+=("$LOAD_PID")

	# wait up to 5 s for the pid report and for every stage to rename itself
	for ((i = 0; i < 50; i++)); do
		ready=1
		for name in cg_source cg_relay1 cg_relay2 cg_sink; do
			pid=$(sed -n "s/^$name=\([0-9][0-9]*\)\$/\1/p" "$log")
			comm=
			[[ -n $pid ]] && comm=$(cat "/proc/$pid/comm" 2>/dev/null)
			[[ $comm == "$name" ]] || { ready=0; break; }
		done
		((ready)) && break
		kill -0 "$LOAD_PID" 2>/dev/null || break
		sleep 0.1
	done
	for name in cg_source cg_relay1 cg_relay2 cg_sink; do
		pid=$(sed -n "s/^$name=\([0-9][0-9]*\)\$/\1/p" "$log")
		if [[ -z $pid ]]; then
			t_fail "chainload did not report a $name pid" "$(cat -- "$log")"
			stop_load
			return 1
		fi
		LOAD_CHILDREN+=("$pid")
		comm=$(cat "/proc/$pid/comm" 2>/dev/null)
		if [[ $comm != "$name" ]]; then
			t_fail "chainload pid $pid has comm '$comm', expected $name" \
			       "$(cat -- "$log")"
			stop_load
			return 1
		fi
		[[ $name == cg_sink ]] && SINK_PID=$pid
	done
	t_info "chainload -d $LOAD_SECS -i $INTERVAL_MS: $(paste -sd ' ' -- "$log")"
	sleep "$WARMUP_SECS"
	return 0
}

# finish_load: wait for the workload to finish on its own; it must exit 0
finish_load() {
	local rc

	[[ -n $LOAD_PID ]] || return 0
	wait "$LOAD_PID"
	rc=$?
	bg_forget "$LOAD_PID"
	LOAD_PID=
	if ((rc != 0)); then
		t_fail "chainload exited with status $rc" "$(cat -- "$OUT/$T_NAME.load")"
	fi
}

# run_chaingraph OUTFILE ARGS...: run chaingraph (stderr to <test>.stderr)
run_chaingraph() {
	local outfile=$1 errfile="$OUT/$T_NAME.stderr" pid rc

	shift
	T_ERRFILES+=("$errfile")
	t_info "chaingraph $* > ${outfile#"$ROOT"/}"
	# Background + wait keeps Ctrl-C responsive; the watchdog sends SIGINT
	# (chaingraph prints what it has) and SIGKILL 10 s later.
	timeout -s INT -k 10 "$CG_TIMEOUT" "$CHAINGRAPH" "$@" \
		>"$outfile" 2>"$errfile" &
	pid=$!
	BG_PIDS+=("$pid")
	wait "$pid"
	rc=$?
	bg_forget "$pid"

	if ((rc == 124)); then
		t_fail "chaingraph still running after ${CG_TIMEOUT}s; interrupted by the watchdog"
	elif ((rc != 0)); then
		t_fail "chaingraph exited with status $rc"
	fi
	if [[ -n $SINK_PID && $(cat "/proc/$SINK_PID/comm" 2>/dev/null) != cg_sink ]]; then
		t_info "warning: the workload exited before chaingraph finished, so its user stacks may be unresolved; raise LOAD_SECS (now $LOAD_SECS)"
	fi
	return "$rc"
}

# ------------------------------------------------------------------ tests

# T1: the whole chain, down to the timer interrupt, with its stack frames
test_t1_full_chain() {
	start_load || return
	run_chaingraph "$T1_OUT" -f -d 4 "$TRACE_SECS"
	finish_load
	check format "$T1_OUT"
	check chain "$T1_OUT" "${FULL_CHAIN_ARGS[@]}"
	check interrupts "$T1_OUT"
}

# T2: --depth 2 keeps two wakers and marks the cut
test_t2_depth_limit() {
	local out="$OUT/$T_NAME.folded"

	start_load || return
	run_chaingraph "$out" -f -d 2 "$TRACE_SECS"
	finish_load
	check format "$out"
	check chain "$out" --target cg_sink --wakers cg_relay2,cg_relay1 \
		--truncated --min-us "$MIN_US"
}

# T3: --pid restricts targets, but wakers from other processes are kept
test_t3_pid_filter() {
	local out="$OUT/$T_NAME.folded"

	start_load || return
	run_chaingraph "$out" -f -p "$SINK_PID" "$TRACE_SECS"
	finish_load
	check only-target "$out" --target cg_sink
	check chain "$out" "${FULL_CHAIN_ARGS[@]}"
}

# T4: human-readable output with kernel stacks only
test_t4_kernel_stacks_human() {
	local out="$OUT/$T_NAME.txt" word

	start_load || return
	run_chaingraph "$out" -K -d 4 "$T4_TRACE_SECS"
	finish_load
	for word in cg_sink cg_relay2 'waker 4: \[hardirq\]'; do
		if ! grep -q -- "$word" "$out" 2>/dev/null; then
			t_fail "output does not match $word" \
			       "$(head -n 40 -- "$out" 2>/dev/null)"
		fi
	done
	# user stacks were not collected, so no user function can appear
	if grep -q -- 'cg_stage_' "$out" 2>/dev/null; then
		t_fail "user frames (cg_stage_*) appear despite -K" \
		       "$(grep -n -m 5 -- 'cg_stage_' "$out")"
	fi
}

# T5: the T1 folded output renders as a chain graph SVG
test_t5_flamegraph_svg() {
	local err="$OUT/$T_NAME.stderr" rc

	if [[ ! -s $T1_OUT ]]; then
		t_fail "${T1_OUT#"$ROOT"/} is missing or empty; run t1 first"
		return
	fi
	if [[ ! -f $FLAMEGRAPH ]]; then
		t_fail "flamegraph.pl not found at $FLAMEGRAPH"
		return
	fi
	if [[ " ${SELECTED[*]} " != *" t1_full_chain "* ]]; then
		t_info "using ${T1_OUT#"$ROOT"/} from an earlier run"
	fi
	T_ERRFILES+=("$err")
	rm -f -- "$T1_SVG"
	perl "$FLAMEGRAPH" --colors=chain --countname=us --title "chain graph" \
		<"$T1_OUT" >"$T1_SVG" 2>"$err"
	rc=$?
	t_info "flamegraph.pl --colors=chain --countname=us --title \"chain graph\" < ${T1_OUT#"$ROOT"/} > ${T1_SVG#"$ROOT"/}"
	((rc == 0)) || t_fail "flamegraph.pl exited with status $rc"
	if [[ ! -s $T1_SVG ]]; then
		t_fail "${T1_SVG#"$ROOT"/} is empty"
		return
	fi
	grep -q 'cg_sink' "$T1_SVG" || t_fail "${T1_SVG#"$ROOT"/} does not contain cg_sink"
	grep -q '</svg>' "$T1_SVG" || t_fail "${T1_SVG#"$ROOT"/} is truncated (no </svg>)"
}

# ------------------------------------------------------------------- main

echo "chaingraph end-to-end tests on $(uname -r); outputs in ${OUT#"$ROOT"/}/"
for t in "${SELECTED[@]}"; do
	t_begin "$t"
	"test_$t"
	t_end
done

echo
echo "$PASS_COUNT passed, $FAIL_COUNT failed"
if ((FAIL_COUNT)); then
	echo "failed: ${FAILED_TESTS[*]}"
	exit 1
fi
exit 0
