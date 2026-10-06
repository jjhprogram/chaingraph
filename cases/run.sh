#!/usr/bin/env bash
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#
# Run one variant of a hitchtrace case study: the minigame, the environment
# the variant sets up around it, and hitchtrace attached through the layer.
# Run as your normal user; only the tracer runs under sudo.
#
#   make && make layer layer-install && make -C cases
#   cases/run.sh VARIANT [SECONDS] [RUN_NAME]
#
# Variants:
#   baseline   the game alone on CPUs $GAME_CPUS
#   bgcpu      + bgload on the same CPUs: a background application that
#              wakes every BG_PERIOD_MS and burns BG_BURST_MS on BG_THREADS
#              threads (cases.md A/E2: a game losing its cores to another
#              process)
#
# Output, in cases/out/RUN_NAME/:
#   frames.jsonl   the game's own per-frame phase timings ("engine profiler")
#   hitch.jsonl    hitchtrace records
#   hitch.txt      the same, human-readable
#   bursts.jsonl   when bgload was burning CPU (bgcpu only): the ground truth
#   manifest.json  what was run, on what
set -u

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
VARIANT=${1:?usage: cases/run.sh VARIANT [SECONDS] [RUN_NAME]}
SECS=${2:-30}
RUN=${3:-$VARIANT}
OUT=$ROOT/cases/out/$RUN
LAYER=$ROOT/build/libVkLayer_hitchtrace.so

GAME_CPUS=${GAME_CPUS:-0-3}
GAME_ARGS=${GAME_ARGS:--w 3 -n 120}
# the game caps at 60 fps (16.67 ms); a hitch is a frame 25% over that
BUDGET_US=${BUDGET_US:-20833}
BG_THREADS=${BG_THREADS:-4}
BG_PERIOD_MS=${BG_PERIOD_MS:-700}
BG_BURST_MS=${BG_BURST_MS:-20}

case $VARIANT in
baseline|bgcpu) ;;
*) echo "unknown variant: $VARIANT" >&2; exit 2 ;;
esac
for f in build/hitchtrace build/cases/minigame build/cases/bgload; do
	[ -x "$ROOT/$f" ] || { echo "missing $f: run make && make -C cases" >&2; exit 2; }
done
[ -f "$LAYER" ] || { echo "run 'make layer layer-install' first" >&2; exit 2; }
sudo -n "$ROOT/build/hitchtrace" --version >/dev/null 2>&1 ||
	{ echo "sudo -n build/hitchtrace must work (NOPASSWD rule)" >&2; exit 2; }

rm -rf -- "$OUT"
mkdir -p -- "$OUT"

GAME_PID=
BG_PID=
cleanup() {
	[ -n "$BG_PID" ] && kill "$BG_PID" 2>/dev/null
	[ -n "$GAME_PID" ] && kill "$GAME_PID" 2>/dev/null
}
trap cleanup EXIT

# the game outlives the trace on both ends
# shellcheck disable=SC2086
HITCHTRACE=1 taskset -c "$GAME_CPUS" "$ROOT/build/cases/minigame" \
	-d $((SECS + 4)) $GAME_ARGS -o "$OUT/frames.jsonl" 2>"$OUT/game.log" &
GAME_PID=$!
sleep 1.5
kill -0 "$GAME_PID" 2>/dev/null || { cat "$OUT/game.log" >&2; exit 1; }

sudo -n "$ROOT/build/hitchtrace" -p "$GAME_PID" -x "$LAYER" \
	-m hitch_frame_mark -M hitch_present_end -b "$BUDGET_US" -d "$SECS" \
	-o "$OUT/hitch.jsonl" >"$OUT/hitch.txt" 2>"$OUT/hitch.err" &
TRACE_PID=$!
sleep 1

if [ "$VARIANT" = bgcpu ]; then
	taskset -c "$GAME_CPUS" "$ROOT/build/cases/bgload" -t "$BG_THREADS" \
		-p "$BG_PERIOD_MS" -b "$BG_BURST_MS" -d $((SECS - 2)) \
		-o "$OUT/bursts.jsonl" 2>"$OUT/bgload.log" &
	BG_PID=$!
fi

wait "$TRACE_PID"
[ -n "$BG_PID" ] && wait "$BG_PID"
BG_PID=
wait "$GAME_PID"
GAME_PID=

cat >"$OUT/manifest.json" <<EOF
{"variant": "$VARIANT", "seconds": $SECS, "date": "$(date -Is)",
 "kernel": "$(uname -r)", "cpus_online": $(nproc --all),
 "game_cpus": "$GAME_CPUS", "game_args": "$GAME_ARGS", "budget_us": $BUDGET_US,
 "bgload": {"threads": $BG_THREADS, "period_ms": $BG_PERIOD_MS, "burst_ms": $BG_BURST_MS},
 "vulkan_device": "$(vulkaninfo --summary 2>/dev/null | sed -n 's/^\s*deviceName\s*= //p' | head -1)",
 "mesa": "$(vulkaninfo --summary 2>/dev/null | sed -n 's/^\s*driverInfo\s*= //p' | head -1)",
 "hitchtrace_commit": "$(git -C "$ROOT" describe --always --dirty)"}
EOF

tail -n 2 "$OUT/hitch.err"
echo "output in ${OUT#"$ROOT"/}"
