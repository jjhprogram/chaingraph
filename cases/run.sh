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
#   iomain     the game loads an ASSET_MB asset every ASSET_EVERY frames on
#              its main thread (cases.md C: a frame waiting on storage)
#   iothread   the same load done by a streaming thread that the main thread
#              waits for
#   mem        the game keeps a WORLD_MB world in memory and walks WALK_MB of
#              it every ASSET_EVERY frames, inside a cgroup limited to
#              MEM_MAX, so the walk faults pages back in from swap (cases.md
#              B: memory pressure)
#   memthp     the same with transparent huge pages asked for the world
#   slack      the game's sleeps are made imprecise in bursts by a preloaded
#              library (cases.md F2: timers delivered late)
#   ipc        the game makes IPC_CALLS blocking calls per frame to a server
#              process on CPU SRV_CPU, where bgload runs too (cases.md D1: a
#              game waiting on wineserver)
#   cpuspike   the game burns BURN_MS of CPU on its main thread every
#              ASSET_EVERY frames: a negative control, not a kernel stall
#   iodisk     iothread on a disk the game shares: the game and bgio, reading
#              in bursts, run in one cgroup whose reads from the data disk
#              are limited to DISK_BPS (cases.md C1: other I/O saturating a
#              slow disk). Needs the io controller delegated to the user
#              manager; DISK_BPS=none runs it without the limit
#
# Output, in cases/out/RUN_NAME/:
#   frames.jsonl   the game's own per-frame phase timings ("engine profiler")
#   hitch.jsonl    hitchtrace records
#   hitch.txt      the same, human-readable
#   bursts.jsonl   the ground truth where the environment supplies it: when
#                  bgload was burning CPU (bgcpu, ipc), when bgio was reading
#                  (iodisk), which sleeps were made imprecise (slack). Elsewhere it is frames.jsonl: which
#                  frames loaded, walked or burned
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
ASSET_MB=${ASSET_MB:-8}
ASSET_EVERY=${ASSET_EVERY:-30}
PACK=$ROOT/build/cases/data/pack.bin
WORLD_MB=${WORLD_MB:-512}
WALK_MB=${WALK_MB:-2}
MEM_MAX=${MEM_MAX:-420M}
SLACK_NS=${SLACK_NS:-8000000}
SRV_CPU=${SRV_CPU:-5}
IPC_CALLS=${IPC_CALLS:-30}
BURN_MS=${BURN_MS:-12}
DISK_BPS=${DISK_BPS:-200M}
DISK_SLICE=hitchdisk.slice
SCRATCH=$ROOT/build/cases/data/scratch.bin
# e.g. HITCHTRACE_ARGS=-U for user stacks
read -r -a TRACE_ARGS <<<"${HITCHTRACE_ARGS:-}"

case $VARIANT in
baseline|bgcpu) ;;
iomain)   GAME_ARGS="$GAME_ARGS -s main -p $PACK -a $ASSET_MB -e $ASSET_EVERY" ;;
iothread) GAME_ARGS="$GAME_ARGS -s thread -p $PACK -a $ASSET_MB -e $ASSET_EVERY" ;;
mem)      GAME_ARGS="$GAME_ARGS -m $WORLD_MB -g $WALK_MB -e $ASSET_EVERY" ;;
memthp)   GAME_ARGS="$GAME_ARGS -m $WORLD_MB -g $WALK_MB -e $ASSET_EVERY -H" ;;
slack)    ;;
ipc)      GAME_ARGS="$GAME_ARGS -i $OUT/srv.sock -c $IPC_CALLS"
	  # more threads than the game's cores see: the server's one CPU is crowded
	  BG_THREADS=${IPC_BG_THREADS:-4}; BG_BURST_MS=${IPC_BG_BURST_MS:-25} ;;
cpuspike) GAME_ARGS="$GAME_ARGS -b $BURN_MS -e $ASSET_EVERY" ;;
iodisk)   # smaller loads, more often: alone they fit in the frame's slack
	  ASSET_MB=${IO_ASSET_MB:-2}; ASSET_EVERY=${IO_ASSET_EVERY:-15}
	  GAME_ARGS="$GAME_ARGS -s thread -p $PACK -a $ASSET_MB -e $ASSET_EVERY"
	  BG_THREADS=${IO_BG_THREADS:-4}; BG_PERIOD_MS=${IO_BG_PERIOD_MS:-1500}
	  BG_BURST_MS=${IO_BG_BURST_MS:-400} ;;
*) echo "unknown variant: $VARIANT" >&2; exit 2 ;;
esac

# A slow disk shared with something else: one slice for the game and for
# bgio, its reads from the data disk limited. The user manager can only do
# that if it was delegated the io controller.
if [ "$VARIANT" = iodisk ] && [ "$DISK_BPS" != none ]; then
	ctl=/sys/fs/cgroup/user.slice/user-$(id -u).slice/user@$(id -u).service/cgroup.controllers
	if ! grep -qw io "$ctl" 2>/dev/null; then
		echo "iodisk needs the io controller delegated to your user manager." >&2
		echo "Once, as root, then log out and back in (or reboot):" >&2
		echo "  mkdir -p /etc/systemd/system/user@.service.d" >&2
		echo "  printf '[Service]\\nDelegate=cpu cpuset io memory pids\\n' \\" >&2
		echo "      > /etc/systemd/system/user@.service.d/delegate.conf" >&2
		echo "  systemctl daemon-reload" >&2
		echo "DISK_BPS=none runs the variant without the limit." >&2
		exit 2
	fi
fi

# What the game is started through: a memory-limited cgroup, a preload.
WRAP=()
case $VARIANT in
mem*)  WRAP=(systemd-run --user --scope -q -p "MemoryMax=$MEM_MAX") ;;
iodisk) [ "$DISK_BPS" = none ] ||
	WRAP=(systemd-run --user --scope -q "--slice=$DISK_SLICE") ;;
slack) WRAP=(env "LD_PRELOAD=$ROOT/build/cases/slack.so" "SLACK_NS=$SLACK_NS"
	     "SLACK_LOG=$OUT/bursts.jsonl") ;;
esac
for f in build/hitchtrace build/cases/minigame build/cases/bgload \
	 build/cases/bgio build/cases/ipcserver build/cases/slack.so; do
	[ -e "$ROOT/$f" ] || { echo "missing $f: run make && make -C cases" >&2; exit 2; }
done
[ -f "$LAYER" ] || { echo "run 'make layer layer-install' first" >&2; exit 2; }
sudo -n "$ROOT/build/hitchtrace" --version >/dev/null 2>&1 ||
	{ echo "sudo -n build/hitchtrace must work (NOPASSWD rule)" >&2; exit 2; }

rm -rf -- "$OUT"
mkdir -p -- "$OUT"

# The asset pack: real blocks, not a sparse file, or reading it costs nothing.
case $VARIANT in
io*)
	if [ ! -f "$PACK" ]; then
		mkdir -p -- "$(dirname -- "$PACK")"
		dd if=/dev/zero of="$PACK" bs=1M count=256 oflag=direct status=none ||
			{ echo "cannot create $PACK" >&2; exit 1; }
	fi ;;
esac
if [ "$VARIANT" = iodisk ]; then
	if [ ! -f "$SCRATCH" ]; then
		dd if=/dev/zero of="$SCRATCH" bs=1M count=1024 oflag=direct status=none ||
			{ echo "cannot create $SCRATCH" >&2; exit 1; }
	fi
	if [ "$DISK_BPS" != none ]; then
		systemctl --user set-property --runtime "$DISK_SLICE" \
			"IOReadBandwidthMax=$(dirname -- "$PACK") $DISK_BPS" ||
			{ echo "cannot limit $DISK_SLICE" >&2; exit 1; }
	fi
fi

GAME_PID=
BG_PID=
SRV_PID=
cleanup() {
	[ -n "$BG_PID" ] && kill "$BG_PID" 2>/dev/null
	[ -n "$GAME_PID" ] && kill "$GAME_PID" 2>/dev/null
	[ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null
}
trap cleanup EXIT

if [ "$VARIANT" = ipc ]; then
	taskset -c "$SRV_CPU" "$ROOT/build/cases/ipcserver" "$OUT/srv.sock" \
		2>"$OUT/server.log" &
	SRV_PID=$!
	sleep 0.3
fi

# the game outlives the trace on both ends
# shellcheck disable=SC2086
HITCHTRACE=1 "${WRAP[@]}" taskset -c "$GAME_CPUS" "$ROOT/build/cases/minigame" \
	-d $((SECS + 4)) $GAME_ARGS -o "$OUT/frames.jsonl" 2>"$OUT/game.log" &
GAME_PID=$!
sleep 1.5
kill -0 "$GAME_PID" 2>/dev/null || { cat "$OUT/game.log" >&2; exit 1; }

sudo -n "$ROOT/build/hitchtrace" -p "$GAME_PID" -x "$LAYER" \
	-m hitch_frame_mark -M hitch_present_end -b "$BUDGET_US" -d "$SECS" \
	"${TRACE_ARGS[@]}" \
	-o "$OUT/hitch.jsonl" >"$OUT/hitch.txt" 2>"$OUT/hitch.err" &
TRACE_PID=$!
sleep 1

if [ "$VARIANT" = bgcpu ] || [ "$VARIANT" = ipc ]; then
	# the game's own cores, or only the one its server runs on
	BG_CPUS=$GAME_CPUS
	[ "$VARIANT" = ipc ] && BG_CPUS=$SRV_CPU
	taskset -c "$BG_CPUS" "$ROOT/build/cases/bgload" -t "$BG_THREADS" \
		-p "$BG_PERIOD_MS" -b "$BG_BURST_MS" -d $((SECS - 2)) \
		-o "$OUT/bursts.jsonl" 2>"$OUT/bgload.log" &
	BG_PID=$!
fi

if [ "$VARIANT" = iodisk ]; then
	# off the game's cores: the disk is the only thing they share
	"${WRAP[@]}" taskset -c "${IO_BG_CPUS:-6-9}" \
		"$ROOT/build/cases/bgio" -R -f "$SCRATCH" -t "$BG_THREADS" \
		-p "$BG_PERIOD_MS" -b "$BG_BURST_MS" -d $((SECS - 2)) \
		-o "$OUT/bursts.jsonl" 2>"$OUT/bgio.log" &
	BG_PID=$!
fi

wait "$TRACE_PID"
[ -n "$BG_PID" ] && wait "$BG_PID"
BG_PID=
wait "$GAME_PID"
GAME_PID=
[ -n "$SRV_PID" ] && wait "$SRV_PID" 2>/dev/null
SRV_PID=

cat >"$OUT/manifest.json" <<EOF
{"variant": "$VARIANT", "seconds": $SECS, "date": "$(date -Is)",
 "kernel": "$(uname -r)", "cpus_online": $(nproc --all),
 "game_cpus": "$GAME_CPUS", "game_args": "$GAME_ARGS", "budget_us": $BUDGET_US,
 "bgload": {"threads": $BG_THREADS, "period_ms": $BG_PERIOD_MS, "burst_ms": $BG_BURST_MS},
 "asset": {"mb": $ASSET_MB, "every_frames": $ASSET_EVERY},
 "memory": {"world_mb": $WORLD_MB, "walk_mb": $WALK_MB, "max": "$MEM_MAX"},
 "slack_ns": $SLACK_NS, "server_cpu": "$SRV_CPU", "ipc_calls": $IPC_CALLS,
 "burn_ms": $BURN_MS, "disk_bps": "$DISK_BPS",
 "hitchtrace_args": "${HITCHTRACE_ARGS:-}",
 "vulkan_device": "$(vulkaninfo --summary 2>/dev/null | sed -n 's/^\s*deviceName\s*= //p' | head -1)",
 "mesa": "$(vulkaninfo --summary 2>/dev/null | sed -n 's/^\s*driverInfo\s*= //p' | head -1)",
 "hitchtrace_commit": "$(git -C "$ROOT" describe --always --dirty)"}
EOF

tail -n 2 "$OUT/hitch.err"
echo "output in ${OUT#"$ROOT"/}"
