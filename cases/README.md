# hitchtrace case studies

Each case reproduces the *mechanism* behind a documented Linux gaming stutter (see `../cases.md`)
around one unmodified workload, and asks whether a hitchtrace record alone explains the hitch.

## Setup

- **`minigame/`**: a small game-shaped Vulkan program with a main thread (`mg_main`: frame limiter,
  simulation, job submission, upload, present) and a worker pool (`mg_worker*`) that draws the frame in
  software. It has **no stall injectors**. It also logs its own per-frame phase timings
  (`frames.jsonl`), which is what an engine's built-in profiler would show.
- **The environment** supplies the cause. `bgload.c` is a background application that wakes
  periodically and burns CPU. It logs every burst, and that log is the ground truth.
  `bgio.c` is its disk counterpart: it reads or writes a scratch file in bursts (case 2b).
- **`run.sh VARIANT [SECONDS] [RUN_NAME]`** runs the game on CPUs 0–3, attaches hitchtrace through the
  Vulkan layer, and writes everything to `out/RUN_NAME/`.

```sh
make && make layer layer-install && make -C cases
cases/run.sh bgcpu 60 bgcpu-confirm
python3 cases/analyze_bgcpu.py cases/out/bgcpu-confirm
tools/hitchview.py cases/out/bgcpu-confirm/hitch.jsonl    # writes hitch.html: one page, every hitch
```

`tools/hitchview.py` turns any hitchtrace JSONL file into a self-contained HTML page: every hitch of
the run on a strip, and for the selected frame its **timeline** (one lane per thread, every off-CPU
interval at its real time, an arrow from whichever thread ended a wait), where its thread's time
went, each stall group with its wakeup chain, and the other threads' totals on the same scale.

## The per-frame timeline

A hitch record used to hold only sums and chains. It now also carries the frame's raw off-CPU
intervals (`events` in the JSON): for every thread of the process, when it left the CPU, when it was
woken, when it ran again, why it waited, and who woke or preempted it.

- **How.** While a frame is open, each interval is appended to a per-frame buffer as it ends. At the
  frame mark, a frame over budget has its buffer sent right after its record; any other frame's
  buffer is discarded. Frames within budget still emit nothing. `--no-timeline` turns it off.
- **Checked against the totals.** The intervals and the per-thread totals come from different paths
  in the BPF program, so `tests/check_timeline.py` compares them: clipped to the frame, a thread's
  intervals must add up to its buckets. On the three game runs (`out/*-timeline`) and a full
  hitchbench run, every one of 2,294 and 511 thread lines agrees to within 50 µs. Five waits ended
  while their frame was being closed; the record calls those "still waiting at frame end" and the
  timeline has the finished interval.
- **Cost.** 72 bytes per interval: a median 16–22 intervals per hitch in the game (about 1.5 KB on
  top of the 10.3 KB record), 37 in hitchbench's `thousand_cuts`. Two 147 KB buffers of kernel memory.
  Interleaved A/B on the game (3 × 12 s per arm): job phase a median 9.3 ms untraced, 8.6 ms traced
  with `--no-timeline`, 9.0 ms with the timeline on, so no measurable slowdown on this VM.
- **Limits.** Threads of the traced process only: a waker in another process appears by name, with how
  long it was itself off-CPU during the wait, but has no lane. At most 2,048 intervals per frame; a
  record says how many did not fit. Time running in the kernel (page faults, reclaim) is not on the
  timeline, only in the buckets.

## Summary

| Case | What happens | Explanation in the record | Right amount | Ranked first (total / stall time) | What it cannot say |
|---|---|---|---|---|---|
| 1 `bgcpu` | a background app takes the game's cores | 99% | 100% of the worker's delay | on top (one long wait) | names only the last preemptor |
| 2 `iomain`, `iothread` | a frame waits on a disk read | 100% | 91–94% | 66–71% / 81–88% | why the disk was slow |
| 2b `iodisk` | another program reads from the same slow disk | 100% | 99% | 94% / 100% | **who else was using the disk** |
| 3 `mem`, `memthp` | walking memory that was swapped out | 99–100% | 95–96% | 33–60% / 99–100% | which pressure caused the swapping |
| 4 `slack` | the frame limiter's sleep ends late | sees the sleep | 99% | 0% | **that the sleep was late** |
| 5 `ipc` | calls to a server that cannot get its CPU | 100% | 105% | 8% / 98% | **who had the server's CPU** |
| 6 `cpuspike` | CPU work in the game (negative control) | "running", 100% | 99% | blames no stall in 91% | where the CPU time went |

"Stall time" is a ranking replayed on the same records: a wait on a thread that was itself running
does not count, and neither does a timer sleep. No case produced a false explanation in its noise
frames, with one qualification in case 5.

All results below are from the development VM (Hyper-V, 10 vCPUs, lavapipe software Vulkan). Its
timing is noisy: with nothing injected, 2–6% of frames run long, and the noise level drifts between
runs. So every comparison is made **between frames of the same run**, using the ground-truth log.
Don't compare separate runs. An interleaved A/B test (3 × 12 s each) found no measurable slowdown
from tracing: the job phase took a median 9.8 ms traced vs 10.2 ms untraced.

## Case 1: a background application takes the game's cores (`bgcpu`)

**What it models.** `cases.md` categories A and E2, and the "lock-holder preemption" and "periodic
background process" examples: a launcher, web helper or capture tool competing with the game for CPU.
The game runs with 3 workers on 4 CPUs and about 10 ms of work per 16.7 ms frame. Every 700 ms
`bgload` wakes 4 threads on the same CPUs, each burning 20 ms. A hitch is a frame over 20.8 ms
(25% over the 60 fps cap).

**Result** (`out/bgcpu-confirm`, 60 s, hitchtrace at commit 65c66fc):

| | Quiet frames | Frames during a burst |
|---|---|---|
| Frames | 3,317 | 193 |
| Over budget | 51 (2%) | 81 (42%) |
| Game's own profiler: job phase, median | 9.1 ms | 15.2 ms |

The game's own profiler shows *that* the job phase got slower, with no reason. hitchtrace recorded all
132 over-budget frames. Scoring each record against the ground truth:

| Question, answerable from one record | Burst frames (81) | Noise frames (51) |
|---|---|---|
| Is `bgload` named as the preemptor on some thread's line? | **80 (99%)** | **0 (0%)** |
| Did some worker spend ≥ half the overrun runnable but not running? | 80 (99%) | 18 (35%) |
| Does the wakeup chain's first hop say "preempted"? | 79 (98%) | 9 (18%) |
| …and explain ≥ half the overrun? | 66 (81%) | 5 (10%) |

So the record **does** contain the diagnosis: the per-thread lines say which workers lost the CPU, for
how long, and to which thread of which process, with no false accusations in the noise frames.

**But the summary lines mislead.** A typical burst-frame record:

```
hitch frame 1516  29.7 ms  (budget 20.8 ms, over by 8.8 ms, 327% kernel-observable)  root mg_main
  blocked   28.8 ms  futex 24.8 | timer 4.0 | present 0.1
  resolved   waiting on a running task 22.1 ms, inherited stalls 2.8 ms      <-- says "the game's own work"
  worst stall 24.8 ms  blocked in __futex_wait
    <- mg_worker2   preempted, runnable 2.8 ms                               <-- 2.8 of the worker's 11.2 ms
  threads
    mg_worker2   oncpu 13.6  runnable 11.2 ...  preempted by bgload0 for 2.8 ms   <-- the real story
    mg_worker0   oncpu 12.3  runnable  8.4 ...  preempted by bgload3 for 2.8 ms
    mg_worker1   oncpu 12.2  runnable  8.4 ...  preempted by bgload2 for 2.8 ms
```

Across the burst frames the chain credits a median **40%** of the waking worker's real delay, and the
root's "resolved" line reports a median 3.2 ms inherited against 14.0 ms "waiting on a running task".

**Why.** Three properties of the current implementation, all visible in `resolve()` and
`record_wakeup()` in `src/hitchtrace.bpf.c`:

1. **Only the waker's last off-CPU interval is inherited.** The scheduler alternates a worker and a
   `bgload` thread in slices of about 3 ms, so a worker is preempted several times per frame. Each
   wake link carries just the most recent interval (`w_out`..`w_in`). Per worker, total runnable time
   is a median 2× its longest single preemption.
2. **Only one waker is followed.** The main thread waits for all three workers; the chain follows
   whichever finished last.
3. **The chain does not carry the preemptor's name.** A hop says "preempted, runnable 2.8 ms" without
   saying by whom. The name appears only on the per-thread lines, and only for threads of the game's
   own process.

The "% kernel-observable" figure in the header (327% here) is also unusable as it stands: it counts
the frame limiter's sleep and the main thread's normal ~10 ms wait for its workers as stall time.

**What this means for the design.** Keeping more chains per frame would not fix point 1 on its own.
The link needs the waker's **total** off-CPU time inside the wakee's wait window, split by cause, plus
the preemptor's identity. Whether hitchtrace records one chain or many, each hop must be credited
correctly first.

### After the link-accounting fix

The fix (uncommitted at the time of the run; manifest says `65c66fc-dirty`) addresses points 1 and 3:

- Every task keeps a short history of its own off-CPU time: a ring of 32 checkpoints of cumulative
  runnable and blocked time. Intervals ending within 2 ms of a checkpoint share it, so the ring always
  reaches back at least 64 ms however finely the time is chopped up.
- At a wakeup, the link records how long the waker was off-CPU **since the wakee began to wait**
  (runnable and blocked separately), and who last preempted the waker in that window.
- A hop that was mostly kept off the CPU ends the chain and names the preemptor. Before, the chain
  went on to whoever had last woken that task, which had nothing to do with the preemption.

Same case, fresh 60 s run (`out/bgcpu-fixed`: 183 burst frames, 77 over budget), burst-frame records:

| | Before (`bgcpu-confirm`, 81 records) | After (`bgcpu-fixed`, 77 records) |
|---|---|---|
| Share of the waking worker's real delay credited by the chain, median | 40% | **100%** |
| Time the chain's first hop explains, median | 3.0 ms | **7.6 ms** |
| First hop says "preempted" and explains ≥ half the overrun | 66 (81%) | **74 (96%)** |
| First hop names `bgload` as the preemptor | not possible | **69 (90%)** |
| Root's "resolved" line: inherited vs. waiting on a running task, median | 3.2 vs 14.0 ms | **8.2 vs 9.7 ms** |
| `bgload` named on a thread line | 80 (99%) | 74 (96%) |
| `bgload` named anywhere in a *noise* record | 0 of 51 | 0 of 54 |

The remaining ~9–10 ms of "waiting on a running task" is correct: it is the workers' normal drawing
time, present in every frame. A typical record now reads:

```
hitch frame 2072  30.5 ms  (budget 20.8 ms, over by 9.7 ms)  root mg_main
  blocked   29.4 ms  futex 21.6 | timer 7.7 | present 0.1
  resolved   waiting on a running task 11.3 ms, inherited stalls 10.4 ms
  worst stall 21.6 ms  blocked in __futex_wait
    <- mg_worker1   kept off the CPU 10.4 ms, last preempted by bgload0
```

The hitchbench regression suite still passes (12 of 12 classes), and `worker_block` /
`worker_cpu` resolve as before (20.3 ms inherited vs 0.0 ms).

**Still open after the fix:**

- The chain names the **last** preemptor in the window, not the one that took the most time. In 10% of
  burst frames that was another game thread or a driver thread, because once `bgload` takes cores the
  game's own threads also start preempting each other.
- Still one waker per wait (point 2). This is inherent to a wait on several threads: the chain follows
  the one that finished last.
- The "% kernel-observable" header is unchanged and still not usable.
- `paper/` describes the old accounting (§4.4 "Resolution", the record size) and its hitchbench numbers
  come from runs made before this change.

## Stall groups: more than one chain per frame

hitchtrace used to explain **one** stall per frame: the root's longest single blocked interval. That
fails whenever a frame's time is lost in many short waits on one thing, or to preemption, which was
never a candidate. A harmless contiguous sleep then wins and the chain explains the wrong thing.

A record now carries the root's off-CPU time as **groups**: all intervals with the same cause that the
same thread ended (or an interrupt, or, for preemption, the same process). Groups are ranked by total
time and each carries the chain behind its longest interval. Seven groups get a slot; when the table
is full, the smallest is folded into an eighth, "other", so the groups always sum to the root's off-CPU
time (checked on 348 of 348 records across both workloads). Time the root spent preempted, or woken and
waiting for a CPU, forms groups too.

**Old rule against new rule, on the same records.** A record has enough to replay the old rule, so both
are scored on identical hitchbench frames: how often does the top-ranked stall have the cause the
injector produced? (`tests/rank_rules.py`, on a full `run_hitch_tests.sh` run, 12 of 12 classes passing.)

| Class | Injected frames | Old rule: longest single wait | New rule: largest group |
|---|---|---|---|
| `thousand_cuts` (40 sleeps of 0.5 ms) | 16 | 0 | **16** |
| `preempt` (root shares a CPU with hogs) | 17 | 0 | **17** |
| `io` | 18 | 13 | 13 |
| `sleep`, `poll`, `worker_block`, `worker_cpu`, `present_long`, `present_off` | 100 | 100 | 100 |
| **Total** | **151** | **113 (75%)** | **146 (97%)** |

For `thousand_cuts` the top group reads "timer, 40 times, longest 0.7 ms"; for `preempt` its first hop
names the hog process in 17 of 17. The five `io` misses are frames where the present wait really was
longer than the I/O.

The background-CPU case is unchanged by this (`out/bgcpu-groups`: chain credits 100%, names `bgload`
in 91% of burst frames, 0 of 65 noise frames), as it should be: there the root has one long wait per
frame. A record from that run:

```
hitch frame 3119  28.9 ms  (budget 20.8 ms, over by 8.1 ms)  root mg_main
  blocked   28.2 ms  futex 21.4 | timer 6.6 | present 0.2
  resolved   waiting on a running task 10.9 ms, inherited stalls 10.7 ms
  stalls     off-CPU time by cause and by who ended it, largest first
     21.4 ms  futex    blocked in __futex_wait
              <- mg_worker2   kept off the CPU 10.6 ms, last preempted by bgload0
      6.6 ms  timer    blocked in do_nanosleep
              <- [hardirq]    woken by an interrupt
      0.4 ms  in 3 smaller groups
```

**Costs and limits.**

- A record is now 10,336 bytes (was 7,056): eight groups of 464 bytes instead of one stall.
- Each group keeps the chain of its **longest** interval only. Forty waits on forty different chains
  behind the same waker would show one of them.
- On-CPU kernel time (page faults, reclaim, compaction) is still a bucket and not a group, so it is
  not in the ranking.
- The JSON keeps a `worst` object for older readers. It is now the largest group, so `worst.ns` is a
  group total; the longest single interval is `longest_ns`. `paper/scripts/hitchbench_results.py`
  still reads `worst.ns` as the latter and needs updating before it is run on new data.

**Incidental finding.** In the first, noisier run (`out/bgcpu`), the top preemptor in the *noise*
frames was `llvmpipe-N`: threads of another process doing software rendering, most likely the desktop
compositor. hitchtrace prints the preempting thread's name but not its process, although the record
carries the process id.

## Case 2: a frame waits on storage (`iomain`, `iothread`)

**What it models.** `cases.md` category C and the "synchronous asset read" example: every 30th frame
the game needs an 8 MB asset from a pack file before it can draw, as at an area transition. Two
variants, the two ways games end up waiting on storage:

- **`iomain`**: the main thread reads the asset itself, 1 MB at a time.
- **`iothread`**: the main thread asks a streaming thread (`mg_stream`) for it and waits.

The pack is read with `O_DIRECT`. A real game does not do that; it stands in for assets that are not
yet in the page cache, without needing root to drop caches. Ground truth is the game's own frame log:
which frames loaded an asset and how long the game spent on it.

**Contention needs a throttled disk.** The documented case (C1) is a *download* saturating the disk.
On this VM a background writer (`bgio`, 8 threads writing 1 MB blocks with `O_DIRECT`) made no
reliable difference: an 8 MB load took a median 7.3 ms without it and 8.7 ms during its bursts, inside
the run-to-run spread. The virtual disk sits on fast host storage. These two variants therefore
reproduce the wait itself; the contended disk is `iodisk`, below, which needs a one-time root step.

**Result** (60 s each, `HITCHTRACE_ARGS=-U`, hitchtrace at 4ca8a3b):

| | `iomain` | `iothread` |
|---|---|---|
| Frames that loaded an asset | 117 | 117 |
| Of those, over budget | 89 (76%) | 91 (78%) |
| Game's own profiler: asset phase in load frames, median | 16.2 ms | 14.4 ms |
| Other frames over budget (noise) | 76 of 3,391 | 74 of 3,380 |

Scoring each over-budget record against the ground truth:

| `iomain`: from one record | Load frames (89) | Noise frames (77) |
|---|---|---|
| The record has an I/O group whose chain ends at a device interrupt | **89 (100%)** | 0 |
| That group's time / the game's own load time, median | **91%**, in 8 waits | |
| I/O is the **top-ranked** group | 63 (71%) | 0 |
| I/O would be top under the old rule (longest single wait) | 62 (70%) | 0 |

| `iothread`: from one record | Load frames (91) | Noise frames (75) |
|---|---|---|
| Some group reads "waited for `mg_stream`, which was blocked on I/O, ended by an interrupt" | **91 (100%)** | 0 |
| The chain's first hop / the game's own load time, median | **94%** | |
| Root's "resolved" line calls ≥ half the overrun an inherited stall | 91 (100%) | 18 (24%) |
| That group is **top-ranked** | 60 (66%) | 0 |

So the record explains every one of these hitches, with the right amount, and never invents an I/O
stall in a frame that had none. With `-U` it also names the game's function:
`pread <- load_asset <- main`. A record from each variant:

```
hitch frame 809  33.2 ms  (budget 20.8 ms, over by 12.3 ms)  root mg_main          [iomain]
  blocked   30.5 ms  futex 8.4 | io 15.8 | timer 6.1 | present 0.2
  stalls     off-CPU time by cause and by who ended it, largest first
     15.8 ms  io       blocked in blk_io_schedule, 8 times, longest 11.2 ms
              <- [softirq]         woken from a softirq
      8.4 ms  futex    blocked in __futex_wait
              <- mg_worker2        ...

hitch frame 1619  31.6 ms  (budget 20.8 ms, over by 10.7 ms)  root mg_main         [iothread]
  blocked   29.4 ms  futex 23.7 | timer 5.5 | present 0.2
  resolved   waiting on a running task 10.1 ms, inherited stalls 13.8 ms
  stalls     off-CPU time by cause and by who ended it, largest first
     14.5 ms  futex    blocked in __futex_wait
              <- mg_stream         blocked 13.8 ms in blk_io_schedule
                <- [softirq]       woken from a softirq
      9.2 ms  futex    blocked in __futex_wait
              <- mg_worker0        was running (waiting for work) 0.0 ms
```

In the second record the buckets alone say "futex 23.7 ms". The groups split that into a wait on the
streamer, which was on disk, and the ordinary wait for the workers, which were running. That split is
what the chain is for.

**Finding: the ranking is polluted by the frame's normal waits.** The I/O explanation is in every
record but on top in only 66–71% of them. Every frame of this game, good or bad, has about 10 ms of
waiting for its workers and about 6 ms of frame-limiter sleep. When a load is short, those rank above
it. In the misses the I/O was a median 6.4 ms against a 9.8 ms worker wait. Ranking by total time
answers "where did the off-CPU time go", not "what was different about this frame".

Two other rules, replayed on the same records by `analyze_io.py`:

| I/O (or the wait on `mg_stream`) is top-ranked | `iomain` (89) | `iothread` (91) |
|---|---|---|
| By total time (current) | 63 (71%) | 60 (66%) |
| By stall time: a task-ended wait counts only its inherited part | 78 (88%) | 74 (81%) |
| By excess over a usual frame (per-cause median of the noise records) | 89 (100%) | not computable per cause |

Dropping "waiting on a running task" from the ranking helps but leaves the limiter sleep in the way.
Ranking by what the frame has **more of than usual** gets every one. It needs a baseline of normal
frames, which hitchtrace does not keep: normal frames emit nothing and leave nothing behind. A
per-cause baseline is not enough for `iothread`, where the wait on the streamer and the wait on the
workers are both futex waits; the baseline has to be per group. The same baseline would also fix the
"% kernel-observable" header and the share gate, which count these normal waits as stall time.

**Other observations.**

- **Grouping made no difference here** (71% against 70% under the old rule). The load is 8 waits, but
  one of them is usually long (a median longest of over half the group), so the longest single wait
  was already I/O. Grouping matters when no single wait stands out, as in `thousand_cuts`.
- **The chain ends at the device interrupt.** hitchtrace says the thread waited on storage and for how
  long. It cannot say why the storage was slow or who else was using it, which is exactly what the
  download case (C1) needs. `iodisk` below confirms it.
- **No measurable tracing overhead on the load**: interleaved A/B, 3 × 12 s per arm, 48 loads each:
  a median 7.8 ms untraced, 8.7 ms traced, 9.1 ms traced with `-U`, with a 90th percentile of 18–19 ms
  in all three. The 14–16 ms medians in the two case runs are this VM's disk being slower at the time.
- A waker delayed 0.2 ms in an 8.4 ms wait is printed as "kept off the CPU 0.2 ms". True, but the
  wording gives it more weight than the 8.2 ms it spent running.

## Case 2b: another program is reading from the same slow disk (`iodisk`)

**What it models.** `cases.md` C1: a launcher downloads or verifies files while the game streams
assets from the same disk. Every 15th frame the game asks its streaming thread for a 2 MB asset and
waits, as in `iothread`. Next to it `bgio -R` reads a large file with 4 threads, in 400 ms bursts
every 1.5 s, pinned to CPUs 6–9 so it never takes the game's cores. The two share nothing but the
disk.

The VM's disk is too fast for the reader to matter, so the disk is made slow: the game and the reader
run in one cgroup (`hitchdisk.slice`) whose reads from that disk are capped at 200 MB/s
(`IOReadBandwidthMax`). That is a slow disk with one budget for both: while the reader is idle the
game has all of it, and during a burst the reader's four threads take most of it. Ground truth is two
logs that hitchtrace never sees: the game's (which frames loaded, and how long the load took) and the
reader's (when it was reading).

The cap needs the I/O controller delegated to the user's systemd, once, as root:

```
sudo mkdir -p /etc/systemd/system/user@.service.d
printf '[Service]\nDelegate=cpu cpuset io memory pids\n' | sudo tee /etc/systemd/system/user@.service.d/delegate.conf
sudo systemctl daemon-reload && systemctl --user daemon-reexec
```

`run.sh iodisk` refuses to run without it. `DISK_BPS=none` runs the same thing with no cap.

**Result** (60 s, `HITCHTRACE_ARGS=-U`, hitchtrace at 2fef08b, 3,290 frames):

| The game's own log | Loads with the reader idle | Loads during a reader burst |
|---|---|---|
| Frames | 162 | 58 |
| Load time, median | 2.2 ms | **57.3 ms** |
| Over budget | 23 (14%) | 54 (93%) |

The same 2 MB read is 26 times slower while the other program reads. Scoring each over-budget record:

| From one record | Loads during a burst (54) | Noise (83) |
|---|---|---|
| Some group reads "waited for `mg_stream`, which was blocked on I/O, ended by an interrupt" | **54 (100%)** | 22 (quiet loads) |
| The streamer's I/O in the chain / the game's own load time, median | **99%** | 97% |
| That group is top-ranked by total | 51 (94%) | 7 |
| … by stall time | 54 (100%) | 20 |
| **Anything in the record names `bgio`** | **0 (0%)** | 0 |

The 22 noise records with an I/O group are loads made while the reader was idle. They are short (about
2 ms), correctly reported, and mostly not on top. A typical record from a burst:

```
hitch frame 2144  86.2 ms  (budget 20.8 ms, over by 65.4 ms)  root mg_main
  blocked   85.4 ms  futex 82.2 | timer 2.8 | present 0.3
  resolved   waiting on a running task 19.4 ms, inherited stalls 63.1 ms
  stalls     off-CPU time by cause and by who ended it, largest first
     63.7 ms  futex    blocked in __futex_wait
              <- mg_stream         blocked 63.1 ms in blk_io_schedule
                <- [softirq]       woken from a softirq
     18.5 ms  futex    blocked in __futex_wait
              <- mg_worker2        was running (waiting for work) 0.0 ms
    mg_stream    oncpu 0.6  blocked 66.3  futex 3.4 | io 63.0   largest 61.9 ms in blk_io_schedule
```

**What the record gives.** "This frame was 65 ms late because the streaming thread waited 63 ms for
the disk, in two waits." That is complete and correct, and it settles the first question a developer
has: the game did nothing unusual, the storage answered slowly. With `-U` the game's own stack says
which load it was.

**What it cannot give: the other program.** Nothing in any of the 54 records mentions `bgio`, and
nothing can. A wakeup chain follows who woke whom, and the streamer was woken by the disk's
completion interrupt. The reader never woke or preempted any thread of the game; it took the disk's
time by having its own requests served first. The competition happens inside the kernel's block
layer (the part that queues read and write requests for a device), which hitchtrace does not watch.
The record for a slow disk and the record for a busy disk are the same record.

A developer could still infer it: 2 MB in 63 ms is 32 MB/s, far below what the disk does when idle.
But hitchtrace does not record the size of the read, and the same load reported 2 ms a second earlier
lives in a different record, or in none if that frame was on time.

What would close the gap is block-layer data taken only while a target thread is waiting on I/O: how
long its request sat queued before the device started it, against how long the device took, and which
other processes had requests issued or completed in that window. The kernel has tracepoints for each
step (`block_rq_insert`, `block_rq_issue`, `block_rq_complete`). On this setup one more caveat
applies: the cap delays requests in the cgroup throttle, before they reach the device queue, so the
queued-versus-served split would have to count that stage as well.

**A second effect: the frames after a long load are slow too.** The game's log shows the workers' job
phase, usually 13.7 ms, taking longer in the frame that waited and in the ones after it, and the more
so the longer the wait was:

| Load took | Loads | Job phase, same frame | Next frame | The one after | Next frame over budget |
|---|---|---|---|---|---|
| under 5 ms | 151 | 13.6 ms | 14.0 | 13.4 | 1% |
| 5–20 ms | 11 | 15.4 | 15.3 | 14.2 | 0% |
| 20–45 ms | 23 | 19.8 | 18.4 | 16.7 | 26% |
| 45–70 ms | 25 | 24.1 | 20.9 | 17.0 | 60% |
| over 70 ms | 10 | 24.3 | 21.4 | 19.9 | 60% |

So one slow load costs two or three hitches, not one. The reader's bursts alone do not slow the
workers: frames with no load have the same 13.8 ms job phase inside and outside a burst. What the
records of those following frames (26 of them) say: no I/O at all, a 23 ms wait on the workers, and
the three workers on the CPU for 60 ms between them where a normal frame needs about 41 ms. They were
not queued (4 ms runnable in total). They ran the same work 50% slower. The likely reason is that the
cores sat idle for the length of the load and came back slow (lower clock speed, or the host having
parked the virtual CPUs), but that is not verified: nothing inside the VM shows it.

For hitchtrace this is the `cpuspike` blind spot again. The record says "the workers were running"
and blames no stall, which is true, but it has no way to say they ran slower than usual, and nothing
links the follow-on hitch to the load one frame earlier. In the viewer's overview strip the pattern
is visible to a person: a high dot followed closely by one or two just over the budget line.

**Other observations.**

- **Ranking is not the problem here** (94% by total). The load is long enough to outweigh the frame's
  normal waits; the three misses are loads of 11–16 ms.
- **The timeline agrees with the buckets**: `check_timeline.py` finds 1 thread line in 1,104 off by
  more than 50 µs (77 µs of run-queue time on a helper thread).
- The interrupt that ends the chain is reported the same way whether the wait was in the device or in
  the cgroup throttle, so the record does not show that a throttle was involved.

## Case 3: walking memory that was swapped out (`mem`, `memthp`)

**What it models.** `cases.md` category B, the memory-tuning folklore: a game that runs well until
memory is tight. The game keeps a 512 MB world in memory and, every 30th frame, walks 2 MB of it
(reads and writes every page), as on entering an area whose data has not been touched for a while. It
runs in a cgroup limited to 420 MB (`systemd-run --user --scope -p MemoryMax=420M`), so part of the
world is always in swap and the walk faults it back in. `memthp` asks for transparent huge pages for
the world (`MADV_HUGEPAGE`). Ground truth is the game's log: which frames walked, how long the walk
took, and the main thread's own page-fault counts from `getrusage`.

Without the limit the walk takes 0.1 ms. With it, a median 18 ms, with 64 major and 448 minor faults.

| From one record | `mem`: walk frames (81) | `memthp`: walk frames (83) | Noise (38, 39) |
|---|---|---|---|
| Has an I/O group whose kernel stack is a page fault | 80 (99%) | 83 (100%) | 0 |
| Memory (fault buckets + that I/O) is ≥ half the overrun | 81 (100%) | 83 (100%) | 0 |
| Memory time / the game's own walk time, median | 95% | 96% | |
| The group's wait count equals the game's major-fault count (±2) | 80 (99%) | 83 (100%) | |
| That group is top-ranked by total | 27 (33%) | 50 (60%) | 0 |
| … by stall time | 80 (99%) | 83 (100%) | 0 |
| … under the old rule (longest single wait) | 12 (15%) | 7 (8%) | 0 |

The record reads: 64 waits on I/O, none longer than about 0.7 ms, 13–18 ms in all, blocked in

```
io_schedule <- folio_wait_bit_common <- do_swap_page <- handle_mm_fault <- exc_page_fault
```

which is a page fault waiting for a page to come back from swap, plus about 3 ms of on-CPU time
inside major faults. The count matching the kernel's own major-fault counter, frame by frame, is a
second independent check that the grouping is right.

- **This is the case grouping was built for.** No single wait stands out, so the old rule picked the
  swap-in in 8–15% of frames. Summed, it is the largest stall in every one.
- **Huge pages made no difference here.** Same fault counts with and without them: pages come back
  from swap one small page at a time. This is one data point under one kind of pressure, not a
  verdict on the THP advice in `cases.md`.
- **The record says "swap-in", not why memory was tight.** It cannot see the limit, or who else used
  the memory. No reclaim time showed up on the main thread.

## Case 4: the frame limiter's sleep ends late (`slack`)

**What it models.** `cases.md` F2, the Linux 7.0 clockevents regression: timers delivered late, so a
frame limiter oversleeps. A preloaded library (`slack.so`) wraps `clock_nanosleep` in the unmodified
game and, for 400 ms in every 1.5 s, raises the thread's timer slack to 8 ms before the sleep. (Another
process may not set a thread's timer slack, so this has to run inside the game.) Ground truth is the
library's log of the affected sleeps, and the game's own measure of how late each sleep ended.

In affected frames the sleep ended a median 8.1 ms late and 98% went over budget.

| From one record | Affected frames (244) | Noise (29) |
|---|---|---|
| Timer group / the game's own limiter time, median | 99% | 99% |
| The timer sleep is top-ranked by total | 0 | 0 |
| The record has a field saying the sleep ended late | **0** | 0 |

**hitchtrace cannot explain this hitch.** It measures the sleep exactly (9.5 ms against a usual 6),
but a sleep that was asked to be 6 ms and took 14 looks the same as one asked to be 14. The timeline
shows a longer pink bar and nothing else. The stall-time ranking, which ignores timer sleeps, is blind
to it by construction. This is the gap `cases.md` predicted, now measured: hitchtrace has to record
how long a sleep **asked for**, and report the overshoot.

## Case 5: calls to a server that cannot get its CPU (`ipc`)

**What it models.** `cases.md` D1 and the "wineserver round-trips" example: a game thread that makes
many blocking calls per frame to another process. `ipcserver` stands in for wineserver, pinned to
CPU 5; the game's main thread makes 30 request/reply calls to it every frame. `bgload` bursts on CPU 5
only (4 threads, 25 ms every 700 ms), so the game's own cores are untouched and only the server is
delayed. Ground truth is `bgload`'s burst log.

Normally the 30 calls take 2.8 ms. During a burst, a median 9.2 ms, and 81% of those frames go over.

| From one record | Burst frames (233) | Noise (41) |
|---|---|---|
| Has a group of waits ended by `mg_server` | 233 (100%) | 41 (100%) |
| … saying the server was kept off the CPU | 232 (100%) | 27 (66%) |
| … and naming `bgload` | **0** | 0 |
| The group's inherited time / the game's extra call time, median | 105% | |
| That group is top-ranked by total | 19 (8%) | 0 |
| … by stall time | 228 (98%) | 16 (39%) |
| … under the old rule | 0 | 0 |

The chain crosses the process boundary correctly: 30 waits, ended by a thread of another process,
which was itself kept off the CPU for the right amount of time. Two things fall short.

- **It cannot say who had the server's CPU.** The server was not preempted. It was asleep, was woken
  by the game's request, and then waited in the run queue behind `bgload`. hitchtrace records who
  takes a CPU *from* a running thread, not who is occupying it when a woken thread wants it. This is
  the second gap `cases.md` predicted (the run-queue occupant).
- **The noise frames show the same group, smaller.** Every call costs the server's wake-up latency, so
  even an untroubled frame has 1–2 ms of "server kept off the CPU". In a frame that is over budget by
  1 ms for another reason, that is enough to rank first by stall time (39% of noise frames). It is
  real time, and removing it would have saved the frame, but it is not what made that frame
  different. The server has no lane on the timeline: only the game's process is logged.

## Case 6: CPU work in the game (`cpuspike`), a negative control

**What it models.** The most famous stutter in `cases.md`, shader compilation, and any spike of the
game's own: every 30th frame the main thread burns 12 ms of CPU. hitchtrace should not blame a stall.

| From one record | Spike frames (86) |
|---|---|
| Running is ≥ half the overrun | 86 (100%) |
| Extra running time / the game's own burn time, median | 99% |
| No stall worth a quarter of the overrun | 78 (91%) |

The record says "the main thread was running 12 ms more than usual", which is the right answer and
the signal to switch to a CPU profiler. It cannot say what the thread was running: stacks are taken
when a thread leaves the CPU, not while it runs. In the other 9% an unrelated stall of some size
happened in the same frame.

