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
- **`run.sh VARIANT [SECONDS] [RUN_NAME]`** runs the game on CPUs 0–3, attaches hitchtrace through the
  Vulkan layer, and writes everything to `out/RUN_NAME/`.

```sh
make && make layer layer-install && make -C cases
cases/run.sh bgcpu 60 bgcpu-confirm
python3 cases/analyze_bgcpu.py cases/out/bgcpu-confirm
```

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
