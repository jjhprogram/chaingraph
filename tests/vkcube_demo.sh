#!/usr/bin/env bash
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#
# Profile a real Vulkan app (vkcube) with hitchtrace, through the implicit
# layer. Run it as your normal user: it starts vkcube on your session and
# only the tracer runs under sudo.
#
#   make && make layer-install
#   tests/vkcube_demo.sh [wayland|x11] [SECONDS]
#
# Two runs per invocation, because on a software driver the answer differs:
#   1. with the present bracket   (-M hitch_present_end)
#   2. without it                 (-M "")
# lavapipe does the rasterization inside vkQueuePresentKHR, so the bracket
# calls the whole frame "the display pacing us"; without it, the same time is
# named after what it actually blocked in.
set -u

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
MODE=${1:-wayland}
SECS=${2:-8}
OUT=$ROOT/tests/out
LAYER=$ROOT/build/libVkLayer_hitchtrace.so

case $MODE in
wayland) APP=vkcube-wayland; BUDGET=16667; BUDGET_ARGS=() ;;   # paced at 60 Hz
x11)     APP=vkcube;         BUDGET=0;     BUDGET_ARGS=(--budget-mode median --median-factor 2.0 --min-budget 500) ;;
*) echo "usage: $0 [wayland|x11] [SECONDS]" >&2; exit 2 ;;
esac

command -v "$APP" >/dev/null || { echo "$APP not installed (vulkan-tools)" >&2; exit 2; }
[ -x "$ROOT/build/hitchtrace" ] || { echo "run make first" >&2; exit 2; }
[ -f "$LAYER" ] || { echo "run 'make layer layer-install' first" >&2; exit 2; }
mkdir -p "$OUT"

# one app instance, traced twice back to back: window focus (which decides
# whether a Wayland surface gets frame callbacks at all) then cannot differ
# between the two configurations.
APP_PID=
start_app() {
	HITCHTRACE=1 "$APP" > "$OUT/vkcube.app.log" 2>&1 &
	APP_PID=$!
	sleep 2
	kill -0 "$APP_PID" 2>/dev/null || {
		echo "FAIL: $APP exited; see $OUT/vkcube.app.log" >&2; return 1; }
	echo "app: $APP pid $APP_PID"
}

trace_one() {         # trace_one NAME [extra hitchtrace args...]
	local name=$1; shift
	local log="$OUT/vkcube_$name.log" js="$OUT/vkcube_$name.jsonl"

	rm -f "$js"
	echo "== $name: ${SECS}s"
	local args=(-p "$APP_PID" -x "$LAYER" -m hitch_frame_mark
		    -d "$SECS" -o "$js" "$@")
	[ "$BUDGET" -gt 0 ] && args+=(-b "$BUDGET")
	sudo "$ROOT/build/hitchtrace" "${args[@]}" > "$log" 2>&1
	sudo chown -R "${SUDO_USER:-$USER}:" "$OUT" 2>/dev/null
	echo "   $(grep -E '^[0-9]+ frames' "$log" | tail -1)"
}

start_app || exit 1
trap 'kill -TERM "$APP_PID" 2>/dev/null' EXIT

# 1. bracket on, gate on the symptom alone: benign vsync waits still emit
trace_one with_present "${BUDGET_ARGS[@]}" -M hitch_present_end
# 2. bracket on, and the excess must be kernel-observable to count
trace_one gated "${BUDGET_ARGS[@]}" -M hitch_present_end --share 0.5
# 3. no bracket: present time is named after whatever it blocked in
trace_one no_present "${BUDGET_ARGS[@]}" -M ""

kill -TERM "$APP_PID" 2>/dev/null; wait "$APP_PID" 2>/dev/null

echo
echo "compare:"
for n in with_present gated no_present; do
	echo "-- $n"
	tail -n 3 "$OUT/vkcube_$n.log" | sed 's/^/   /'
done
