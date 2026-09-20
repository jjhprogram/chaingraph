# Review of `hitchtrace-implementation-plan.md`

Review date: 2026-09-21. Plan reviewed: `hitchtrace-implementation-plan.md` (425 lines, dated 2026-09-19).

Each issue below is written as **what the plan says** (quoted verbatim, with section and line), **why it breaks**, and
**what to change it to**. Fenced blocks are meant to be pasted into the plan.

## How to read this

* **Severity.** *critical* = invalidates a thesis claim or a guaranteed-scope milestone as written; *major* = will cost
  significant time or produce wrong data unless changed; *minor* = worth fixing, no measurement depends on it.
* **Evidence.** Everything checkable was checked on this machine (Ubuntu 24.04, kernel 7.0.0-31-generic, Hyper-V, 10 vCPUs,
  7 GB, no PMU, lavapipe only) or against the v7.0 kernel source and the `chaingraph` repo. Claims about Proton, DXVK,
  vkd3d-proton, Mesa, Steam and Hetzner came from web sources and are marked *(from web sources, verify)* — they decide
  design here, so confirm them before they reach the thesis text.
* **Scope of the review.** The plan's research framing (the is/is-not table, the concessions in §1, the choice of
  baselines) is sound and is not restated here. What follows is only what is wrong, unbuildable as written, or unmeasurable.

## Method

Five independent reviews (kernel/BPF feasibility, measurement validity, Proton/Vulkan ecosystem, baselines and tooling,
reuse and schedule) produced 77 findings. Every finding of *critical* or *major* severity was then handed to a separate
reviewer whose task was to refute it, with the plan, the sources and this machine in hand. Findings that survived are
below, merged where several reviews found the same defect; severities are the post-refutation ones. A final pass
re-grepped every quote against the plan and re-ran every machine claim.

## Two things to settle before any of this lands

1. **One naming pass.** Several issues rename or split buckets (`present_wait` → `display_wait`, `preempt` split into
   `preempt`/`yield`, `gpu_wait` split into fence-wait and submit back-pressure). Apply the renames everywhere at once —
   §2.3, §2.4, §2.5, §2.8, App. C and the W5/W9 DoDs all name buckets.
2. **Week numbers.** Issue 35 re-sequences the schedule (the evaluation box moves to the front). Every other issue cites
   the plan's *original* week numbers. Re-key them by role — *layer + L0*, *baselines*, *bring-up*, *M0*, *M2*, *M3* —
   rather than by number.

## Index

| # | Issue | Severity |
|---|---|---|
| 1 | The predicate, `oncpu` and M0 are built on a per-tgid sum of off-CPU time | critical |
| 2 | Waiting on another thread's CPU work is counted as a kernel stall, and the fix is in stretch scope | critical |
| 3 | Charging a stall at switch-in to the current epoch misplaces exactly the stalls behind bad frames | major |
| 4 | M0/M2 statistics are censored by the default gate, and the denominators and baselines do not exist | major |
| 5 | App. C, the W5 DoD and M1 need a frame join and a per-level criterion | major |
| 6 | Cause buckets overlap, so §2.8's totals double-count and the gate is inflated | major |
| 7 | `present_wait` catches almost nothing — vsync and frame-latency back-pressure land in `futex`/`other_block` | critical |
| 8 | Frame window, reset epoch, ftrace baseline and delayacct baseline are four different intervals | major |
| 9 | "Present caller = render thread" is wrong under Proton — the presenting thread is DXVK's or vkd3d's | major |
| 10 | MangoHud is a second implementation of the same metric, not ground truth | minor |
| 11 | The loader-export uprobe counts zero everywhere — W0's DoD cannot pass, and the per-title check is already decided | minor |
| 12 | pressure-vessel changes the layer path, the maps paths and the bitness | minor |
| 13 | `reclaim` hooks global reclaim only, so the bucket reads ~0 under `MemoryMax` — its own injector | major |
| 14 | `irq_state` from `irq_handler_entry` never sees timer or Hyper-V interrupts, so timer wakeups are blamed on a random thread | major |
| 15 | `gpu_wait` hooks the one fence path RADV does not use, and the obvious replacement cannot be attached | major |
| 16 | Flag-setting syscall hooks: system-wide cost and wrong buckets | major |
| 17 | The Wine sync backend is uncontrolled, and `futex_waitv` is not a 6.7 feature | major |
| 18 | The per-frame `uaddr` histogram cannot detect convoys under Wine | minor |
| 19 | Preempt/yield classification and the W1 bpftrace scripts | minor |
| 20 | Waker matching: the edge ring, the `out ≤ ts ≤ in` rule, and the hop rule | major |
| 21 | `stall_ring` as a hash of arrays cannot be built from BPF; the tid hash and tid registry are unnecessary | major |
| 22 | The L3 window dump runs on the present thread and does not fit the ringbuf/verifier rules | major |
| 23 | Preemptor identity is the first occupant, and runqueue waits get no occupant at all | major |
| 24 | Claim 3 rests on stretch work while guaranteed scope already pays for the edge ring | critical |
| 25 | The `perf --off-cpu` baseline cannot run, and its sweep cannot be expressed | major |
| 26 | The ftrace hist trigger silently moves the whole instance to the `global` clock | major |
| 27 | Snapshot semantics make the M3 drain-time and bytes-per-hitch numbers wrong | major |
| 28 | systing is marked optional, but it is the comparator §1 promises to answer | major |
| 29 | The uprobe offset instructions and the `perf probe` SDT syntax are wrong | minor |
| 30 | The delayacct baseline reads the wrong field and is handicapped by construction | minor |
| 31 | The §3.3 package line aborts `apt` on day one | minor |
| 32 | The Deck-class cgroup recipe never reaches the game | major |
| 33 | The Deck emulation is not faithful: CPU count, SMT layout and GPU memory escape the cgroup | major |
| 34 | Steam-side confounders and version drift are uncontrolled | major |
| 35 | The 12-week schedule does not fit, and the riskiest dependency is first touched in W6 | major |
| 36 | Novelty claims overreach, and direct prior art for online chains is missing | major |
| 37 | dm-delay at Deck-class latencies adds a 1 kHz kthread, and the day-one checklist has wrong blockers | minor |
| 38 | Depth-8 kernel stacks are mostly tracing and scheduler frames, and stack ids go stale | minor |
| 39 | `bpf_task_under_cgroup` is not callable from the USDT uprobes or syscall tracepoints | minor |
| 40 | The §9 feature list is incomplete, the PMU statement is wrong, and the dev VM violates §3.1 | minor |
| 41 | M1's ladder uses an L2 level the plan never defines | minor |
| 42 | libbpf refuses USDT attach on 32-bit ELF | minor |
| 43 | Decide the daemon language now: C, extending `chaingraph.c` | minor |

---

## A. What the tool measures

The definition of the metric, the gate and the frame accounting. Nothing else in the plan is worth building until these are settled.

### 1. The predicate, `oncpu` and M0 are built on a per-tgid sum of off-CPU time (critical)

**Plan says** (§2.2/§2.3/§2.5, lines 90, 111, 130):

> - **Cause-gated predicate (v2)**: `frame_ns > budget && offcpu_ns >= share * (frame_ns - budget)`

> | `oncpu` | remainder | `frame_ns − Σ(off-CPU)` | …

> evaluate predicate against `targets[tgid]` totals (kept per tgid by atomic adds at switch-in to avoid iterating threads)

**Why it breaks**

- A tgid sum has no upper bound at `frame_ns`: N threads contribute up to N×`frame_ns` per window, and under DXVK `dxvk-cs`, `dxvk-submit`, `dxvk-queue`, `dxvk-frame` plus Mesa's WSI thread are parked most of every frame *(from web sources, verify)*.
- In your own `tests/out/t4_kernel_stacks_human.txt` (a 5 s run of `chaingraph -K -d 4`), the `llvmpipe-*` targets total **47.6 s of off-CPU over 11,812 sleeps** — 9.5× wall, ≈159 ms per 16.67 ms frame. lavapipe is the only Vulkan driver that enumerates a device on the dev VM, so hitchbench hits this in W2, not W7. The numerator is pinned to (thread count) × frame time whenever threads idle: *idleness* dominates it.
- All guaranteed scope: `share=0.5` is satisfied by parked-thread time alone on any frame whose tgid has ≳2 threads idle for the window (4.2 ms on the §2.8 frame), so M2/W8's share=0 vs 0.5 comparison degenerates; M0's ratio exceeds 1 wherever that holds, leaving claim 4 without a denominator; `oncpu` goes large negative.
- Parallel sums also inflate M2: sub-threshold stalls on different threads overlap and do not add to frame latency.

**Change to**

```diff
- **Threads of interest**: all threads of the game tgid … [totals summed per tgid]
- oncpu = frame_ns − Σ(off-CPU)
+ **Root thread**: the thread executing the frame_end USDT (caller of vkQueuePresentKHR).
+   Under DXVK dxvk-submit, under vkd3d-proton the command-queue worker — NOT the game's
+   render thread. Recorded per title in W6.
+ **Frame timeline**: the root's [frame_start, frame_end) split into on-CPU / runnable /
+   blocked, each interval clipped to the window.
+   Invariant: oncpu + Σ(buckets) == frame_ns on the root (unit-tested in walker/).
+ **cp_off**: the root's blocked+runnable time, each blocked interval resolved through its
+   waker chain (issue 2). 0 <= cp_off <= frame_ns.
+ Per-thread and per-tgid sums are CONTEXT ONLY: never in the predicate, never in M0/M2.
```

- `oncpu` and the gate numerator are defined once in issue 6. M0's metric (§5) becomes `(cp_off − cp_off_base) / (frame_ns − budget)`, clamped to [0,1]. In §2.8, Worker3's 6.9 ms preemption becomes the *resolved part* of RenderThread's futex block inside `chains`.
- Knock-on: §1 claims 1 and 4, §2.2, §2.3 `oncpu` row, §2.5, §2.8, §5 M0+M2, §4 W7/W8, §10 item 2.

---

### 2. Waiting on another thread's CPU work is counted as a kernel stall, and the fix is in stretch scope (critical)

**Plan says** (§2.3/§2.6/§4, lines 101, 138, 200):

> | `futex` | `sys_enter/exit_futex` … set/clear per-thread flag; capture `uaddr` | blocked while flag set | …

> If `W` was running continuously → terminate: cause is `W`'s own execution

> Guaranteed scope: **L0 + L1 + harness + baselines + M0 + M2 + M3**. Stretch: **L3 v1 + M1**

**Why it breaks**

- §2.3 makes `futex` and `poll` stall buckets unconditionally; only `present_wait` and `timer` are benign. A futex wait whose waker ran user code throughout is *the game's own CPU work*, which §1 excludes from scope — yet a CPU-bound hitch lengthens exactly these waits, passes the cause gate and lands in M0 as a kernel stall.
- The only rule that stops at a busy waker is §2.6 step 3: L3, W9, stretch. L1 (W5) records `{tid, comm, gap_ns}` for one hop and never whether the waker was on-CPU, so guaranteed-scope M0 cannot separate a stall from a CPU-bound frame at all.
- Under Proton the root *is* such a waiter: `dxvk-submit` waits for `dxvk-cs`, which waits for the app thread — 3+ hops before game code *(from web sources, verify)*, so `futex` dominates nearly every frame. Cost is not the obstacle: one task-storage update per wakeup, which chaingraph already does at ~1 µs *with* stack walks.

**Change to**

- Resolve waiter→waker in L1 by extending chaingraph's online propagation (`record_wakeup()`, `src/chaingraph.bpf.c:280`, already `wakee.links[1..] = waker.links[0..]` in O(depth)) instead of waiting for the L3 edge ring. The waker state it needs — `wake_ts`, `last_wake_in`, `last_cause`, `last_stall_ns` — is defined once on `chain_link` in issue 24; drop the stack ids on the hitchtrace build.
- Split the root's blocked interval `[out, ts]` at the waker's switch-in, resolving *time*, not one cause:

```
irq/idle waker (LINK_HARDIRQ|LINK_NMI|LINK_IRQEXIT|LINK_IDLE)
      -> whole interval terminal: iowait / timer / irq  (chaingraph's cut computes this)
inherited    = overlap([out, ts], W's last off-CPU interval) -> W's bucket; recurse links[]
oncpu_stall  = W's fault/reclaim/compaction ns since its switch-in -> that bucket
wait_oncpu   = remainder of [W.last_in, ts] -> NOT a stall; "wait_oncpu:<W comm>"
unresolved   = [out, W.last_out) when W.last_out > out -> reported separately
own_runqueue = [ts, in] -> runqueue
```

- Prerequisite: chaingraph times only voluntary sleeps (`chaingraph.bpf.c:454`: `!preempt && prev_state != TASK_RUNNING`), so preempt/runqueue intervals must be timed before a waker's preemption can be inherited; `max_depth` (default 4, `chaingraph.bpf.c:53`) must be ≥ 4 for the DXVK path.
- M0 numerator = own + inherited runqueue, preempt, iowait, fault, reclaim, compaction; `wait_oncpu`, `gpu_wait`, `display_wait`, `timer` and `unresolved` reported separately, M0 published as a range with `unresolved` counted both ways.
- Add negative control `worker_cpu` to W4 — the root waits on a *busy* worker, M0 must report ≈0 — and redefine `thousand_cuts`'s ground truth as holder-stalled vs holder-running.
- Knock-on: §2.3 futex/poll rows, §2.6 (L3 becomes the offline check), §4 W2–W3 and W5, §5 M0/M1, §7 row "present caller = render thread" (line 323).

---

### 3. Charging a stall at switch-in to the current epoch misplaces exactly the stalls behind bad frames (major)

**Plan says** (§2.3/§2.4/§2.5, lines 95, 120, 130):

> Classification is decided **at switch-in** of a thread of interest

> **Lazy reset**: on update, if `entry.epoch != targets[tgid].epoch` zero it first.

> Then bump `epoch` (resets everything lazily) and `frame_id`. Normal frames: bump epoch only.

**Why it breaks**

- No interval is clipped to the window: a 1 s condvar sleep is charged in full to the frame in which it ends, and a stall still open at `frame_end` contributes nothing to the frame it delayed. (chaingraph has the same shape: `delta = now - ts->offcpu_ts` added whole at switch-in, `chaingraph.bpf.c:488`.)
- With pipelining the causal stall usually lands *outside* the late frame's window: DXVK's `SyncFrameLatency` waits on `m_frameId - GetActualFrameLatency()` *(from web sources, verify)*, so a 20 ms game-thread stall can close during frame k−1 — a good frame whose data "bump epoch only" discards — while late frame k shows only the root's futex wait.
- A shared per-tgid total cannot be reset at all: read-then-zero loses adds made on other CPUs in between, and lazy check-and-zero lets concurrent writers wipe each other.

**Change to**

- Report **OVERLAP** (clipped to the window) and **CAUSAL** (on the root's wake chain, possibly closed earlier). M0 and the predicate use CAUSAL; OVERLAP is per-thread context.

```
- thread_acc: tid -> {epoch, ns[N_CAUSES], …}; lazy reset on epoch change
+ thread_acc: tid -> {out_ns, cum_ns[N_CAUSES], …}   /* cumulative, NEVER reset */
+ targets:    tgid -> { …, ring[F] of {frame_start, frame_end, snapshot of cum_ns} },
+             F = max_frame_latency + 2 (>= 8)
+ at switch-in: split [out, in] across the slots it overlaps (bpf_loop over <= F slots)
+ at frame_end(k): close every still-blocked thread —
+       charge min(now, frame_end) - max(out, frame_start); set out = now
+ verdict for frame k is taken at frame_end(k + D), D = max_frame_latency + 1;
+       per-frame values are differences of cumulative counters
+       -> straddling stalls are split, open stalls are counted, no lost updates
```

- Bump the accounting boundary at the probe issue 8 picks (`present_enter`, i.e. today's `frame_end` USDT), not at both. Present and acquire time is kept out of the in-window buckets by bracketing it (issue 7's `display_wait`), not by moving the epoch boundary — the two are alternatives and §2.5 has room for one.
- **W1 gate, one 30-line BPF object:** does a `SEC("uprobe/...")` program load and verify while calling `bpf_iter_task_new`/`bpf_iter_task_next` over `BPF_TASK_ITER_PROC_THREADS` and `bpf_task_storage_get` on the yielded tasks? If it loads, issue 21's design stands and the tid-keyed snapshot map in issues 3 and 24 is dropped. If the verifier rejects it, keep the tid-keyed map plus `bpf_loop`, and issue 21's §2.5 change applies only to the `tid[256]` registry. Do not write the daemon against both — `bpf_task_from_pid` is no escape, its kfunc set is not registered for the USDT uprobe's `BPF_PROG_TYPE_KPROBE`.
- Record the frame latency in effect in the manifest, size `D` from it, and add a hitchbench injector with separate producer and presenter threads so W5 exercises pipelined attribution.
- Knock-on: §2.4, §2.5, §4 W2–W3 DoD (the "exactly one record" test needs the deferred verdict), §4 W5, §8 manifest.

---

### 4. M0/M2 statistics are censored by the default gate, and the denominators and baselines do not exist (major)

**Plan says** (§2.2/§2.5/§4, lines 90, 130, 241):

> `share=0` reproduces symptom-only gating; `share=0.5` is the default. Both modes are compared in M2.

> Normal frames: bump epoch only. **Zero bytes cross to user space for normal frames.**

> - Output: distribution of off-CPU share of excess per over-budget frame; breakdown by bucket; game-initiated vs external split.

**Why it breaks**

- Nothing in §5 or W7 says M0 runs with `share=0`. At the default every emitted frame already has share ≥ 0.5, so M0's question answers itself — the distribution is truncated at exactly the value being measured. W8's "fraction of excess in the single largest stall" needs all over-budget frames too.
- No per-bucket baseline from good frames exists, since normal frames emit nothing, so a recurring per-frame wait (a job-system join, the DXVK latency wait) counts in full as "excess"; the over-budget frame count is also missing whenever `share>0`.
- A fixed 16.667 ms budget is wrong for M0's conditions: `AllowedCPUs=0-3` + 12 G on a 780M with a title like Cyberpunk makes nearly every frame over budget, and the excess is steady display-bound time surfacing as `futex` (DXVK's `SyncFrameLatency` blocks on a condition variable) *(from web sources, verify)*.

**Change to**

```
+ §5/W7/W8: M0 and the real-title half of M2 run in "study mode" with share=0, so every
+   over-budget frame is emitted. share=0.5 is used only for the M3 volume/perturbation
+   runs. The mode is recorded in every run manifest.
+ §2.2 budget for M0: per title and condition — the title's FPS cap or the refresh rate;
+   uncapped -> rolling 19-frame median x 1.5, floor (excess >= 2 ms). Fixed-60 is a
+   secondary view. (This issue owns the budget; the hitch margin is issue 7's.)
+ §2.4 always-on in-kernel aggregates, read once at run end (as chaingraph reads its
+   `chains` hash), so output stays O(hitches) and claim 2 survives:
+     per tgid : frames_total, frames_over_budget, frames_emitted
+     per bucket: sum, count, log2 histogram — normal and over-budget frames kept apart
+     log2 histogram of (cp_off - cp_off_base)/(frame_ns - budget) over ALL over-budget frames
```

- The normal-frame histogram also supplies `cp_off_base` in issue 1, so this is one map, not two features. Frames are strongly autocorrelated (shader-compile and streaming bursts): report prevalence with CIs across the ≥ 5 runs or by block bootstrap, never a per-frame binomial CI.
- Knock-on: §1 claim 2 (name the aggregates as constant-size, or "output proportional to hitches" carries an asterisk), §5 M0/M2 rows, §4 W7/W8, §8 manifest, §10 items 1–2.

---

### 5. App. C, the W5 DoD and M1 need a frame join and a per-level criterion (major)

**Plan says** (§4/App. C, lines 232, 425):

> - DoD: for each hitchbench injector, the record's `largest.cause`, `preemptor`, or `waker` names the injected mechanism in ≥ 95% of injected frames.

> A record diagnoses an injected stall if it names, for the over-budget frame, (a) the injected mechanism's bucket as the largest contributor … L-level is the minimum level at which both (a) and (b) hold.

**Why it breaks**

- Criterion (a) names no thread and no aggregation. The only frame-level aggregate is `totals_ns`, dominated by parked workers (issue 1) — so `sync_read`, `reclaim` and `holder_preempt` can never win the argmax, while futex injectors pass for the wrong reason. And since L-level is the minimum level at which both hold, a failure of (a) fails *every* level.
- The join is undefined: the injector logs its own frame ids, the BPF `frame_id` is a counter bumped at `frame_end`, and `hitchtrace attach <pid>` starts it at 0 mid-run. With switch-in charging (issue 3) a cross-thread stall lands in frame N or N+1 with no stated tolerance.
- The denominator is wrong: "≥ 95% of injected frames" scores as misses every injection that did not push its frame over budget and every over-budget frame the `share` gate suppressed — `bg_periodic` (5 ms against 16.7 ms) frequently is not a hitch at all.
- `L2` is used in M1 and in the ladder but defined nowhere (see issue 41, which defines the ladder): App. C's "minimum level at which both (a) and (b) hold" is not computable until it is.

**Change to**

```
+ layer: frame_begin/frame_end carry the layer's own per-swapchain present counter
+        (and VkPresentIdKHR.presentId when the app chains one, as a cross-check).
+ BPF:   the record already carries frame_start_ns / t_end_ns (bpf_ktime_get_ns == MONOTONIC).
+ hitchbench: each injection logs [t_start, t_end] with clock_gettime(CLOCK_MONOTONIC).
+ primary join = interval overlap, NOT frame ids; tolerance +/-1 frame for stalls on
+   another thread that cross a boundary, up to frames-in-flight for gpu_heavy.
```

- Rewrite App. C criterion (a) per level, on the path that level exposes — L0: dominant bucket on the root; L1: + the one-hop waker/preemptor of the root's largest stall; L2: + the chain; L3: the edge-window walk — with the injected bucket accounting for ≥ X% of the frame's excess *above the normal-frame baseline* from issue 4's aggregates. Never an argmax over `totals_ns`.
- Score only injected frames that were over budget and emitted; report injected-but-under-budget, over-budget-but-gated-out and emitted-but-misattributed separately, and size W5 injectors at ≥ 2× budget.
- The per-configuration W9 DoD chains are in issue 37; the `kworker` hop goes in all of them. Evict the page cache with `posix_fadvise(DONTNEED)` or `O_DIRECT`, recorded in the manifest.
- Knock-on: §4 W5 DoD and W9 DoD, §5 M1 row, App. C.

---

### 6. Cause buckets overlap, so §2.8's totals double-count and the gate is inflated (major)

**Plan says** (§2.3, lines 95, 103–104):

> Classification is decided **at switch-in** of a thread of interest, from flags set by other hooks plus the `sched_switch` state.

> | `pagefault_minor` / `pagefault_major` | `fentry/fexit handle_mm_fault` set/clear flag; `VM_FAULT_MAJOR` in return | on-CPU time inside fault + any blocking inside it | …
> | `reclaim` | `vmscan/mm_vmscan_direct_reclaim_begin/end` | wall time between begin/end, on thread | …

**Why it breaks**

- §2.8 is the proof: `9.8+7.8+4.1+0.9+1.8+0.6+0.1 = 25.1 ms`, exactly `frame_ns`. So `pagefault_minor` (1.8 ms, mostly on-CPU first-touch) and `reclaim` (0.6 ms, mostly on-CPU LRU scanning) sit on the off-CPU side of the gate and are subtracted from `oncpu`; a frame that is CPU-bound in fault handling passes a `share=0.5` gate.
- Three double-counts on this kernel. (1) A fault that blocks waits in `folio_wait_bit_common()` → `io_schedule()` (mm/filemap.c:1323), so `in_iowait=1` and the same nanoseconds match `pagefault` *and* `iowait`. (2) `/boot/config-7.0.0-31-generic` has `CONFIG_PREEMPTION=y`, `CONFIG_PREEMPT_LAZY=y`, so a `prev_state == TASK_RUNNING` switch inside a fault also matches `preempt`. (3) `reclaim`'s wall time contains `reclaim_throttle()`'s `schedule_timeout` (mm/vmscan.c:587), charged again by `sched_switch`, and direct reclaim is reached from the allocation slowpath under `handle_mm_fault`.
- "at switch-in" and "`VM_FAULT_MAJOR` in return" are mutually exclusive: the thread switches back in *inside* `handle_mm_fault`, before fexit. One user fault calls it up to three times (arch/x86/mm/fault.c:1334, 1344, 1409) and the retry usually returns without MAJOR; the kernel's rule is `mm_account_fault()`: `major = (ret & VM_FAULT_MAJOR) || (flags & FAULT_FLAG_TRIED)` after an early return on RETRY. `runqueue` (`in − ts`) likewise lies inside the blocked interval and is never subtracted from it.
- Cost check: `/proc/vmstat` on this idle VM gives ~500–2,600 pgfault/s (lifetime average ~2,400/s) against ~5k wakeups/s — the fault pair is *not* the hottest hook here, it is quieter than `sched_waking`. Add one line next to A3 in W1 measuring `pgfault/s` on a loaded Proton title before committing to the `handle_mm_fault` pair, which fires for every process before any tgid filter; budget it in M3 either way.

**Change to**

**§2.3 `oncpu` and §2.2's gate, defined once** (issues 1, 6, 7 and 8 all edit these two lines):

```
oncpu   = Σ RUNNING cells on the root timeline — measured, never `frame_ns − Σ`
          (split oncpu.user / oncpu.fault / oncpu.mm)
cp_off  = root-timeline blocked + runnable time resolved through the waker chain,
          excluding display_wait and intentional-sleep timer
gate    = frame_ns > budget + margin
          && (cp_off − cp_off_base) + oncpu.mm >= share * (frame_ns − budget)
invariant: oncpu + cp_off + display_wait + timer == frame_ns on the root
```

M0 publishes both numerators — strict off-CPU, and kernel-observable including `oncpu.mm` — and §1 claim 4 says which is the headline. Exclusive per-thread cells on task storage carry it, one nanosecond charged once:

```c
/* BPF_MAP_TYPE_TASK_STORAGE (as chaingraph src/chaingraph.bpf.c:56-72; freed on exit) */
struct task_state {
    u8  state;                    /* RUNNING | RUNNABLE | BLOCKED            */
    u16 ctx_mask;                 /* fault, memstall, compaction, fence, futex,
                                     poll, sleep, display — depth counters   */
    u8  iowait;                   /* attribute of a BLOCKED cell             */
    u64 last_ts, cell[3][N_CTX];  /* [state][innermost(ctx_mask)]            */
};
/* Credit now-last_ts at EVERY transition: switch-out -> RUNNABLE if the tp's `preempt`
   arg is set else BLOCKED; sched_waking BLOCKED -> RUNNABLE (splits runqueue out);
   switch-in RUNNABLE -> RUNNING; plus each ctx hook. innermost() is a fixed priority:
   compaction > memstall > fault > fence > futex/poll/sleep/display                    */
```

Rewritten §2.3 rows (paste over lines 102–103 and 111):

```text
| `iowait`      | `prev->in_iowait` at switch-out | *attribute* of a BLOCKED cell, never its own bucket | BLOCKED x fault(iowait) = a major fault's I/O |
| `pagefault_*` | fentry/fexit `handle_mm_fault` (depth counter) | BLOCKED/RUNNABLE x fault are off-CPU; RUNNING x fault is a sub-bucket of `oncpu` | major/minor per *call* at fexit |
| `oncpu`       | Σ RUNNING cells | computed directly, never `frame_ns − Σ` | split into oncpu.user, oncpu.fault, oncpu.mm |
```

- Flag pairs use per-kind **depth counters attached fexit-before-fentry** (`src/chaingraph.c:1006–1030`), so a hook starting between the two attaches cannot leave a flag stuck set.
- Gate on `RUNNING × {memstall, compaction}` as `oncpu.mm`; keep `RUNNING × fault` a separately reported cell and a configurable gate term. Reclaim is mostly on-CPU — excluding it makes exactly the memory-pressure hitches M0 exists to expose fail the cause gate.
- New W2–W3 DoD item: for every thread and frame, `Σ cells == thread's in-frame wall time` within 1 µs.
- Knock-on: §2.3 (all rows get a state × context definition), §2.5, §2.8 (totals must no longer sum to `frame_ns`; add `oncpu.fault`/`oncpu.mm`), §5 M0/M2, §1 claim 4, W2–W3 DoD.

## B. The frame model and the Vulkan layer

Where a frame begins and ends under Proton, and what the layer can actually observe.

### 7. `present_wait` catches almost nothing — vsync and frame-latency back-pressure land in `futex`/`other_block` (critical)

**Plan says** (§2.2 lines 88–89; §2.3 line 107):

> Time spent inside `vkQueuePresentKHR` (swapchain acquire, vsync wait) is accounted to a separate `present_wait` bucket of frame N+1's window, so a capped-FPS game does not register present waits as stalls.

> - **Budget**: `HITCHTRACE_BUDGET_US` (default 16667). …

> | `present_wait` | USDT `present_begin/end` | blocked inside | Benign under FPS caps |

**Why it breaks**

- Acquire is a *different entry point*, and the layer does not intercept it. Mesa's X11 WSI `x11_queue_present` only queues; the FIFO throttle blocks in `x11_acquire_next_image` → `wsi_queue_pull`, a condvar, i.e. a futex. Wayland blocks *inside* present only on the legacy pre-`fifo-v1` path — which is what sway 1.9 / wlroots 0.17 would be, if §3.1's Sway option is taken; the dev VM runs GNOME Shell on Wayland with no sway installed, so re-check the compositor half against whichever headless stack §3.1 settles on and record it in §8. (from web sources, verify)
- Under DXVK the back-pressure is `Presenter::acquireNextImage` on `m_surfaceCond` plus `SyncFrameLatency` → Wine `NtWaitForAlertByThreadId` → `futex_wait`; under vkd3d-proton a frame-latency semaphore → fsync `futex_waitv`, or ntsync (`CONFIG_NTSYNC=m` here → an ioctl, so `other_block`, not even `futex`). None of it is inside the present bracket. (from web sources, verify)
- Worked case (FIFO, 16.67 ms): 6 ms work + 10.7 ms acquire wait; a 14 ms on-CPU spike takes work to 20 ms, the frame quantizes to the next vblank at 33.3 ms and the acquire wait becomes 13.3 ms — excess 16.6 ms, futex 13.3 ms, so `share=0.5` passes and M0 books 80% "kernel-observable stall share" for a frame whose only cause is CPU work. Generally, under FIFO the acquire wait absorbs the rounding up to the next vblank, so it grows *with* the excess and the measured share converges on 1 whatever made the frame late. This hits every Proton title and any native title on a FIFO swapchain; §3.2's title list is not fixed, so W6's per-title checklist records which ones.
- `gpu_wait` also misses explicit sync (DRM syncobj waits) — see issue 15. The consequence *here* is that WSI acquire under explicit sync is invisible to the fence hook, so the layer bracket below is needed either way.
- The dev VM cannot expose this and hides a second bug: `wsi_common_queue_present` does `if (wsi->sw) WaitForFences(...)`, so under lavapipe `present_wait` holds the entire software render — labelled "benign". Measured here (scratch FIFO test, lavapipe under Xwayland, 5000 frames): mean present interval **0.119 ms**; acquire and present never blocked. **Commit that probe as `hitchbench/fifo_probe` and re-run it in W0** — it is the only evidence for moving vsync validation to Hetzner and is not reproducible from the repo today.
- 16667 µs is shorter than the real period on both stacks: `amdgpu.virtual_display` uses `drm_cvt_mode(...,60)` → 16.68–16.72 ms; Xorg dummy's fake vblank alternates ~16/17 ms. Every FIFO-locked frame is over budget with milliseconds of acquire futex in it, so the W2–W3 "10k normal frames produce zero records" DoD cannot pass.

**Change to**

```md
- `display_wait` (was `present_wait`): blocked time inside the layer's
  present / acquire / present-wait brackets, OR on any thread whose wakeup at
  switch-in came from a display-pacing thread. Excluded from the cause gate and
  from the M0 numerator. Not assumed benign — still carries its kernel stack.
- `gpu_wait`: fence/syncobj waits (issue 15) plus waits woken by dxvk-queue /
  vkd3d_fence. This is GPU back-pressure: never benign.
```

- Layer brackets (§2.1, W2–W3) — all go through the device dispatch chain, so the layer sees them: `present_enter/present_return` (`vkQueuePresentKHR`), `acquire_enter/acquire_return` (`vkAcquireNextImageKHR`, `…2KHR`), `pwait_enter/pwait_return` (`vkWaitForPresentKHR`, `…2KHR`), `gpuwait_enter/gpuwait_return` (`vkWaitForFences`, `vkWaitSemaphores`). These names replace the plan's `present_begin`/`present_end`.
- Brackets alone do not fix the game thread's futex: classify by *who woke it*, one hop, reusing chaingraph. In `record_wakeup` (`src/chaingraph.bpf.c:280`, `tp_btf/sched_waking` at :419) tag `links[0]` `LINK_PACING`/`LINK_GPU` when the waker holds the layer's in-WSI flag or is a registered tid; at switch-in (:488) a blocked interval whose wake carries that flag becomes `display_wait`/`gpu_wait` instead of `futex`. Comm is truncated to 15 chars — match `"dxvk-submit"`, `"vkd3d-swapchain"`, `"WSI swapchain q"`, `"Xwayland"`; GPU side `"dxvk-queue"`, `"vkd3d_fence"`.
- Budget — the *margin*, not the budget, is this issue's contribution:

```
budget = the title's FPS cap or the measured refresh period; uncapped -> median present
         interval over the first 256 frames          # issue 4 owns this definition
hitch  = frame_ns > budget + margin,  margin = max(2 ms, 0.25 * budget)
```

  A derived budget on a FIFO stack sits within a millisecond of the true period, so without a margin every FIFO-locked frame is over budget. §2.2 states one formula; §10 item 1 closes.
- Gate on the presenting thread's wake chain (`dxvk-submit ← dxvk-cs ← game thread`), not Σ over the tgid; `oncpu` and the gate numerator are defined once in issue 6.
- Run M0 uncapped (`MESA_VK_WSI_PRESENT_MODE=immediate`, `dxgi.syncInterval=0`, `VKD3D_SWAPCHAIN_PRESENT_MODE=IMMEDIATE`) and capped, reported separately; capped only once `display_wait` works. §8 gains: WSI stack, translation layer + commit, effective present mode, swapchain image count, frame-latency settings, sync backend, measured refresh period.
- Add a hitchbench mode copying DXVK's topology (present thread acquiring immediately after present + a futex frame-latency fence on the game thread) and validate vsync classification on Hetzner, not on the VM.
- Knock-on: §1 claim 4 and §5 M0 (prevalence biased upward until `display_wait` splits out); §2.2, §2.3, §2.5, §2.8; W2–W3 probe set and FIFO DoD; §8; §10 item 1.

---

### 8. Frame window, reset epoch, ftrace baseline and delayacct baseline are four different intervals (major)

**Plan says** (§2.2 line 88; §2.5 line 130; §6.3 line 304):

> **Frame N** = `[frame_begin(N-1), frame_end(N))` where `frame_end` fires at present *entry* and `frame_begin` at present *return*.

> Then bump `epoch` (resets everything lazily) and `frame_id`.

> diffs per-thread `cpu_run_virtual/blkio/swapin/freepages/thrashing/compact/wpcopy/irq` delays at each `frame_begin`

**Why it breaks**

- Four windows, all called "frame": accumulators are entry→entry (epoch bumps at `frame_end`), `frame_ns` is return→entry, §6.3's delayacct diff is return→return, MangoHud is entry→entry. M2/M3 therefore compare tools scoped to different intervals.
- `present_wait` is charged to frame N+1 from time lying *outside* N+1's window, so per-thread Σ buckets can exceed `frame_ns` (`oncpu` and the gate numerator are defined once in issue 6).
- A block occurring entirely inside present(N) is in no frame's `frame_ns`: MangoHud shows the hitch, the predicate never fires. On Wayland legacy FIFO and on lavapipe's `if (wsi->sw)` path that is exactly where the wait lives, so W6's "agree within tolerance" DoD fails systematically, not occasionally.
- Aligning the epoch to the window is not sufficient, because stalls are charged *whole, at switch-in, in the epoch where they end*. chaingraph does this today: `src/chaingraph.bpf.c:462` sets `ts->offcpu_ts` at switch-out, :488–489 charge `now - ts->offcpu_ts` at switch-in. A stall that began three frames earlier is billed entirely to this frame; a stall still in progress at frame end — the worker the render thread is waiting on — is absent from the record that exists to explain it.
- W6's comparison is unrunnable as written: normal frames emit nothing, so there is no per-frame series to compare against MangoHud's CSV.

**Change to**

```md
§2.2  now: Frame N = [present_enter(N-1), present_enter(N))
§2.5  now: bump epoch and set frame_start_ns at present_enter — same probe, same timestamp
§2.8  emit frame_ns, present_ns, acquire_ns, and
      cpu_frame_ns = frame_ns - present_ns(N-1) - acquire_ns
§6.1  one uprobe on present_enter carrying the layer-computed delta as an argument:
        echo 'p:hitch/present_enter <so>:0x<OFF> delta=+0(%si):u64' >> uprobe_events
        echo 'snapshot if delta > <budget_us>' > events/hitch/present_enter/trigger
      (drops the synthetic event and the frame_begin/frame_end hist pairing entirely)
§6.3  diff delayacct at present_enter, not at frame_begin
```

- Clip stalls to the window and close the open ones before emitting:

```c
/* switch-in; was: delta = now - ts->offcpu_ts; */
u64 start = frame_start_ns(tgid);              /* epoch start = present_enter */
delta = now - (ts->offcpu_ts > start ? ts->offcpu_ts : start);

/* symptom frames only, inside the bpf_loop over the tid registry §2.5 already runs */
if (ts->offcpu_ts)
        acc[cause] += frame_end_ns - max(ts->offcpu_ts, start);
```

- W6: keep a cheap in-BPF present-to-present histogram plus an over-budget count per tgid (normal frames still emit zero bytes) and compare that distribution and count against MangoHud's CSV, matching individual records by frame time.
- Knock-on: §2.5, §2.8, §6.1, §6.3; W2–W3 (epoch/probe code and the zero-record DoD); W6 DoD; App. C. Supersedes issue 3's "bump at `frame_begin`".

---

### 9. "Present caller = render thread" is wrong under Proton — the presenting thread is DXVK's or vkd3d's (major)

**Plan says** (§7 line 323):

> | Wine thread names don't propagate | Records harder to read | Fall back to tid + role inference (present caller = render thread); verify early |

**Why it breaks**

- DXVK presents on `dxvk-submit`, two hand-offs from the game's `Present()` (game thread → `dxvk-cs` → `dxvk-submit`); vkd3d-proton presents on `vkd3d_queue` via a queued callback. The USDT fires on an API-translation thread that already has a name, so the inference labels the wrong thread and adds nothing. (from web sources, verify)
- The row's premise is also wrong: Wine copies Windows thread names into `comm` (`set_native_thread_name`), and DXVK and vkd3d-proton name their own threads. The residual gap is only threads the game never names — and every comm is truncated to 15 characters (`"WSI swapchain q"`, `"vkd3d-swapchain"`).
- It is not uniform, so it must be determined rather than assumed: Windows games calling Vulkan directly go through win32u's `vkQueuePresentKHR` on the caller's own thread, as do native games.
- Frame *counts* stay 1:1; what shifts is timing — frame windows open and close late by the `dxvk-cs` + `dxvk-submit` latency. That lag is the thing to measure.
- Both shortcuts fail: `VkPresentIdKHR` is attached only under present-id (DXVK) or only for FIFO + present-wait (vkd3d-proton) and is a sequence number with no timing meaning; "one wakeup back from `vkd3d_queue`" breaks on exactly the heavy frames that matter, because that thread only sleeps when its queue is empty.

**Change to**

```md
| Wine thread names don't propagate | Largely moot | Wine copies Windows thread names
to comm (set_native_thread_name); DXVK/vkd3d name their own threads. Match on the
15-char truncated comm. Do NOT infer "present caller = render thread" — under DXVK
and vkd3d-proton it is an API-translation thread. |
```

- Identify the frame thread from the wake chain at the present probe, reusing chaingraph's per-task chain (link pids must be recorded unconditionally: `src/chaingraph.bpf.c:397` currently does `l->pid = per_thread ? waker->pid : 0`). `frame_thread` = first link whose comm is not `dxvk-*` / `vkd3d*` / `WSI*` — DXVK 2 links back, vkd3d-proton 1, winevulkan/native the presenting thread itself. Majority-vote over the first 256 frames (a chain goes stale when the translation thread never sleeps) and record `present_thread` and `frame_thread` in the manifest.
- Measure the hand-off lag per frame (game thread's last wakeup of `dxvk-cs`/`vkd3d_queue` → present probe), report its distribution per title, and when a frame is over budget with significant lag include the frame thread's stalls from the previous window. Use the present ID only to detect skipped/retried presents.
- Get ground truth for the translated path: a D3D11 hitchbench variant on DXVK Native (`DXVK_WSI_DRIVER=SDL3`) with the same injectors, run under Wine+DXVK on Hetzner. Native-only hitchbench exercises none of the `dxvk-cs`/`dxvk-submit`/`dxvk-frame` topology the M0 titles have, so M1/M2 accuracy measured there says nothing about them.
- Manifest: Proton version, DXVK and vkd3d-proton commits, `DXVK_CONFIG`/dxvk.conf, `dxgi.maxFrameLatency`, frame-rate limit, sync interval, present mode.
- Knock-on: §2.6 and W9 DoD (chain roots written in game-thread terms); W5 DoD (≥95% naming the injected mechanism, measured on native hitchbench only); W4/W6 (add the DXVK-Native arm); §7; §8.

---

### 10. MangoHud is a second implementation of the same metric, not ground truth (minor)

**Plan says** (§3.2 line 187; §4 W6 DoD line 237):

> MangoHud frametime log as ground truth for frame times.

> DoD: `hitchtrace` runs against each title for 30 minutes with < X% CPU (record X) and produces plausible records; MangoHud and hitchtrace frame times agree within tolerance.

**Why it breaks**

- MangoHud computes `frametime_ns = now - sw_stats.last_present_time` in `update_hud_info`, reached from `before_present` at the top of `overlay_QueuePresentKHR` — the same present calls, on the same thread, one layer away in the same chain. Agreement cannot validate the frame definition; it is the same metric implemented twice. (from web sources, verify)
- The two do not even time the same interval: MangoHud is entry→entry, §2.2's `frame_ns` is return→entry, and they drift apart precisely when present blocks (issue 8).
- Implicit layer order is undefined (the loader reads manifests in readdir order), so MangoHud's own per-present work (ImGui, `vkQueueSubmit`) lands either inside hitchtrace's frame window or inside its present bracket, and nothing pins which.
- MangoHud starts a `hw_info_updater` thread inside the game process; §2.2's "all threads of the game tgid" folds its blocked time into the gate totals.
- Not problems, so don't spend time on them: the fps limiter is inert at default (`fps_limit = {0}`), and the tempting replacements are worse here — headless Sway does no KMS page flips, `amdgpu.virtual_display` vblank is a software timer, and DXVK's HUD is also measured at present.

**Change to**

```md
§3.2: MangoHud frametime log as an independent present-capture cross-check
      (a second user-space implementation of present-to-present), NOT ground truth.
§4 W6 DoD: per-swapchain present COUNTS match MangoHud exactly, and the
      present-to-present distributions agree within tolerance. A count mismatch
      means a missed USDT hit, a layer not exported by pressure-vessel, or
      multiple swapchains — which is what this check is actually for.
```

- Config, recorded in the manifest: `no_display=1`, `autostart_log=1` (no keyboard on a headless box), `fps_limit=0`, and the *same* MangoHud config in every M3 arm including the no-tool arm.
- Pin and log layer order (explicit layer via `VK_INSTANCE_LAYERS` or a loader settings file); save `VK_LOADER_DEBUG=layer` output in the manifest and re-check it inside pressure-vessel. Tag `hw_info_updater` out of threads of interest.
- In the thesis, state that "frame" means present-to-present, identical to PresentMon's `MsBetweenPresents`, and concede it as standard — §1 already concedes the frame marker to Nsight. Do not add DRM vblank tracing as ground truth on a virtual display.
- Knock-on: W6 DoD; §8.

---

### 11. The loader-export uprobe counts zero everywhere — W0's DoD cannot pass, and the per-title check is already decided (minor)

**Plan says** (§4 W0 line 204; §4 W6 line 236; §7 line 315):

> DoD: `bpftrace -e 'uprobe:/usr/lib/x86_64-linux-gnu/libvulkan.so.1:vkQueuePresentKHR { @[comm]=count(); }'` counts vkcube presents.

> - Loader-bypass check on each title (does the loader-symbol uprobe fire? the layer USDT does?). Record per title.

> … Implicit layer with USDT (layers sit in the device chain regardless); ICD-symbol uprobe as fallback; W1/W6 check per title; report as a finding |

**Why it breaks**

- vkcube itself bypasses the probed symbol: Ubuntu 24.04's vulkan-tools 1.3.275 builds cube with volk and `VK_NO_PROTOTYPES` *(from web sources, verify — `vulkan-tools` is not installed on this VM)*, so present goes through a device dispatch pointer from `vkGetDeviceProcAddr`. The mechanism is verified locally with a test program doing what volk does: the pointer resolves into `/usr/lib/x86_64-linux-gnu/libvulkan_lvp.so` at file offset `0x64180`, with no symbol name, while the loader's own export sits at `libvulkan.so.1` offset `0x39280` (confirmed with `nm -D`) and is never entered. The W0 DoD counts 0 presents.
- For Proton it is not a per-title question: winevulkan fills device function pointers through `p_vkGetDeviceProcAddr` and win32u presents through `device->p_vkQueuePresentKHR`, so every Proton title — DXVK, vkd3d-proton *or* native Vulkan — skips the loader export, and W6's per-title check always returns the same answer. (from web sources, verify)
- The stated fallback has nothing to attach to: `nm -D --defined-only libvulkan_radeon.so` yields four symbols — the three `vk_icd*` entry points plus one stray C++ local — and none is `vkQueuePresentKHR`; plain `nm` reports no symbols and the file carries only a `.gnu_debuglink`. `libvulkan_lvp.so` does export 229 dynamic symbols (gallium and SPIRV-Tools internals), but likewise no `vk*` entry point beyond the three `vk_icd*` ones, and it is equally stripped with a `.gnu_debuglink`. Neither ICD gives the fallback a symbol to attach to. A dbgsym exists for this Mesa (`mesa-vulkan-drivers-dbgsym_25.2.8-0ubuntu0.24.04.2`); kisak-mesa on Hetzner is unverified.
- pressure-vessel captures `libvulkan.so.1` with `if-exists:if-same-abi`, so inside the container the loader may be the runtime's copy rather than the host file the uprobe is on. (from web sources, verify)

**Change to**

- Replace the W0 DoD with a probe on the function vkcube actually calls; the offset is resolved at runtime, so no symbols are needed:

```bash
apt install vulkan-tools      # vkcube is not installed on the VM
# 40-line helper: create instance + device, then
#   p = vkGetDeviceProcAddr(dev, "vkQueuePresentKHR");
#   dladdr(p, &info); off = (char *)p - (char *)info.dli_fbase;
# lavapipe 25.2.8 on this VM: off = 0x64180 in libvulkan_lvp.so
bpftrace -e 'uprobe:/usr/lib/x86_64-linux-gnu/libvulkan_lvp.so:0x64180 { @[comm]=count(); }'
# DoD: the count equals vkcube's own frame count, from vkcube's pid only
```

  Offsets are per Mesa build: re-resolve on each machine and record in the manifest. (Alternative: install `mesa-vulkan-drivers-dbgsym` and probe by name.) Install `vulkan-tools` and re-run this against vkcube itself before rewriting the DoD.

- State the bypass as a known fact instead of a finding to be discovered:

```md
§7:  Loader bypass is universal for volk-based apps (vkcube, many engines) and for
     ALL Proton titles (winevulkan resolves device functions through
     p_vkGetDeviceProcAddr; win32u presents through device->p_vkQueuePresentKHR).
     Mitigation: the implicit layer is the only frame source under Proton.
     Fallback: "ICD dispatch-entry uprobe at a runtime-resolved file offset
     (or by name with mesa-vulkan-drivers-dbgsym)" — not "ICD-symbol uprobe".
A1:  keep for native, non-volk apps only. Optional bypass demonstration:
       uprobe:libvulkan.so.1:vkGetDeviceProcAddr /str(arg1)=="vkQueuePresentKHR"/
     — it shows the lookup happening; it does not count presents, so it is not a gate.
W1 item 1 / W6: delete "loader-bypass check per title". The only open question is
     whether the layer's USDT fires inside pressure-vessel (issue 42 adds process
     bitness to that check).
```

- Move the layer smoke test to the first gate of W2 and budget ~200–250 lines for the minimal implicit layer (loader/layer interface negotiation, instance and device chain creation, per-device dispatch map) — §2.1's "Zero logic beyond that" describes the layer's behaviour, not its size.
- Knock-on: W0 DoD, W1 item 1, W2–W3 (layer smoke test becomes the first gate), W6; §7; App. A (A1).

---

### 12. pressure-vessel changes the layer path, the maps paths and the bitness (minor)

**Plan says** (§2.7 line 147; §4 W2–W3 line 218):

> Emit module+offset with a `/proc/<pid>/maps` snapshot; symbolize offline.

> - Implicit layer with `init`, `frame_begin`, `frame_end`, `present_begin`, `present_end` USDTs; manifest with `enable_environment`; …

**Why it breaks**

- The good news first: host implicit layers are imported by default, the manifest is rewritten under `/overrides`, and the `.so` is a symlink into `/run/host` — the same inode — so a uprobe attached via the host path does fire. Everything *downstream* of the path breaks. (from web sources, verify)
- `/proc/<pid>/maps` inside the container shows container paths (`/run/host/usr/...`, or runtime files that are different files from the host's same-named ones). Symbolizing offline by path silently opens the wrong ELF — a wrong-symbol result, not an error.
- `VK_LAYER_PATH` / `VK_ADD_LAYER_PATH` are unset inside the container when `--runtime` and `--graphics-provider` are active, so a layer located by environment variable during development is simply absent under Proton.
- The loader requires `disable_environment` for implicit layers; the plan's manifest has only `enable_environment`.
- Proton 11 still runs 32-bit games through i386-unix unless `PROTON_USE_WOW64=1`; a 64-bit-only layer produces zero frames for those titles, with no error.
- The Steam-Headless Docker recipe adds a PID namespace, so a pid passed as a USDT *argument* is a container pid, not the daemon's.

**Change to**

```md
layer manifest: add  "disable_environment": { "HITCHTRACE_DISABLE": "1" }
layer install:  a standard host path (/usr/share/vulkan/implicit_layer.d) — never
                VK_LAYER_PATH; build i386 and x86_64, or state 64-bit titles only
                and check PROTON_USE_WOW64. No C++ runtime dependency in the layer.
USDT attach:    once, system-wide, by HOST path (offsets from readelf -n on the
                host .so). Take the tgid from bpf_get_current_pid_tgid(), never
                from a USDT argument.
symbolization:  record build ids at flush and resolve files through
                /proc/<pid>/map_files/ or /proc/<pid>/root — not by map path.
```

- Reuse `/home/asdf/ebpf/src/syms.c`, which already does the container-aware resolution: `open_map_file()` (line 1242), `proc_root_differs()` (line 1058), `elf_build_id()` (line 901, over `notes_build_id()` at 879), `elf_debuglink()` (line 933). Save `VK_LOADER_DEBUG=layer` output captured *inside* the container in the run manifest.
- Knock-on: W6 (the per-title check becomes "does the layer's USDT fire inside pressure-vessel" — issue 11); §8 (manifest fields; `layer/` gains an i386 build).

## C. Hooks and cause classification

Whether each bucket's hook exists, fires for the right events, and can be attached on this kernel.

### 13. `reclaim` hooks global reclaim only, so the bucket reads ~0 under `MemoryMax` — its own injector (major)

**Plan says** (§2.3 line 104; §4 W4 line 225):

> | `reclaim` | `vmscan/mm_vmscan_direct_reclaim_begin/end` | wall time between begin/end, on thread | In-thread; no chain |

> `reclaim` (memory hog inside a `MemoryMax` cgroup)

**Why it breaks**

- `trace_mm_vmscan_direct_reclaim_begin` fires only in `try_to_free_pages()` (mm/vmscan.c:6566) — node-wide. A memcg limit goes `try_charge_memcg()` → `try_to_free_mem_cgroup_pages()` (mm/memcontrol.c:2413), which fires `mm_vmscan_memcg_reclaim_begin` (vmscan.c:6651) instead. Both typedefs are in this kernel's BTF; the plan hooks one.
- `MemoryMax=12G` on a 64 GB box never puts the *node* under pressure, so the bucket reads ~0 for the W4 injector and for the M0 Deck-class condition — the two places it exists for. That fails W5's "≥ 95% of injected frames" DoD and silently zeroes an M0 row.
- The bug is invisible rather than loud: with `memory.max` the charge usually happens inside an anon fault → `pagefault_minor`, and memcg reclaim's `cond_resched()` switches out in `TASK_RUNNING` → `preempt`.
- Your own baseline beats you: delayacct's freepages counter starts in `do_try_to_free_pages()` (vmscan.c:6352), which global *and* memcg reclaim share, so §6.3's "80% with zero kernel code" comparator diagnoses the injector while `reclaim` shows nothing.
- PSI is the definition to cite and is attachable here: `psi_memstall_enter`/`psi_memstall_leave` are BTF FUNCs, `task_struct.in_memstall` is in BTF, `/proc/pressure` exists.

**Change to**

```text
# WAS (§2.3 line 104):
| `reclaim` | `vmscan/mm_vmscan_direct_reclaim_begin/end` | wall time between begin/end, on thread | In-thread; no chain |

# NOW:
| `memstall` | `fentry/psi_memstall_enter` + `fexit/psi_memstall_leave`; open the interval only when
  `current->in_memstall` was 0 (the kernel's nesting flag makes inner calls no-ops) | RUNNING x memstall
  -> oncpu.mm; BLOCKED/RUNNABLE x memstall -> off-CPU (issue 6's cell table) | == the kernel's PSI *memory*
  definition; covers global direct reclaim, memcg `memory.max` and `memory.high` reclaim + its penalty
  sleep, direct compaction, thrashing refault waits, node_reclaim |
| `compaction` | `compaction/mm_compaction_begin/end` (filter tgid) | sub-label of memstall, not a separate
  time base | detail signal only |
```

- In-thread callers covered: page_alloc.c:4176/4445, memcontrol.c:2103/2347/2413, filemap.c:1258/1413, vmscan.c:7634. kswapd (vmscan.c:6967) and kcompactd (compaction.c:3191) are not the game thread — do not hook them.
- Read `prev->in_memstall` with `BPF_CORE_READ_BITFIELD_PROBED` at switch-out, ahead of `in_iowait`, the fault flags and `preempt` (`src/chaingraph.bpf.c:432`), and subtract memstall time from the enclosing `handle_mm_fault` interval so it is not also `pagefault_minor`. Wall time alone is what issue 6 forbids.
- Not `in_thrashing`: `delayacct_thrashing_start()` returns early without `delayacct_key` (include/linux/delayacct.h:172–179) and `kernel.task_delayacct` is 0 here. `in_memstall` already covers refault waits.
- New DoD: the `reclaim` injector's frames show non-zero memstall inside the `MemoryMax` cgroup, cross-checked against the delayacct freepages delta.
- Knock-on: §2.3 rows 104–105, §4 W2–W3 hook list (line 219), W4/W5 DoDs, §5 M0 — label the Deck-class numbers *memcg-limit* reclaim; a 12 G cgroup on a 64 GB host has no kswapd background reclaim, so add one `mem=16G` run if Deck-like global reclaim matters.

---

### 14. `irq_state` from `irq_handler_entry` never sees timer or Hyper-V interrupts, so timer wakeups are blamed on a random thread (major)

**Plan says** (§2.4 line 123; §2.3 line 108; §2.6 step 4, line 139):

> | `irq_state` | percpu array | cpu → {in_hardirq, in_softirq} | 1 | Maintained from `irq_handler_entry/exit`, `softirq_entry/exit`; read at `sched_waking` |

> | `timer` | `nanosleep`/`clock_nanosleep` syscall flag, or wake edge with `irq` flag and thread in poll/sleep | blocked | Intentional sleep (frame limiter) — benign terminal |

> 4. Edge flagged `irq`/`softirq` → terminal: `IRQ`/`softirq` (disk completion, network, timer). Distinguish `timer` via the wakee's sleep/poll flag.

**Why it breaks**

- `trace_irq_handler_entry/exit` fire only for genirq descriptors (kernel/irq/handle.c:201/212). x86 system vectors bypass genirq entirely: `sysvec_apic_timer_interrupt` emits only `local_timer_entry`, `sysvec_hyperv_stimer0`/`_callback` emit no tracepoint (arch/x86/kernel/cpu/mshyperv.c:153–201), and `vmbus_irq` is forced to −1 so VMBus never uses `request_percpu_irq` (drivers/hv/vmbus_drv.c:2994). All still raise the HARDIRQ bits via `irq_enter_rcu`.
- `/proc/interrupts` on this VM: after ~32 h of uptime the only genirq-managed lines are IRQ 8 = 0 and IRQ 9 = 18, and LOC = 0 and NMI = 0, against HVS (stimer0) 54.0M, CAL 37.9M, HYP (VMBus) 710k and RES 115k. Every interrupt this box actually takes is an x86 system vector, and `irq_handler_entry` sees essentially none of them. (Monotonic counters — re-read them when the thesis quotes figures.)
- Sleeper hrtimers expire in hardirq on non-RT kernels (`is_hard` forced only under `PREEMPT_RT`, hrtimer.c:2045/2074; `CONFIG_PREEMPT_RT` unset here), covering `nanosleep`, poll/select timeouts and futex timeouts. `sched_waking`'s `current` is then whatever was interrupted — `swapper/N`, or a sibling game thread. Step 4 never fires, the `timer` clause never fires, and step 3 recurses into the interrupted thread's unrelated stall.
- Measured: chaingraph on this VM counted `irq-wakeups=3080` and `irq-exit-wakeups=644` of `wakeups=32757` (`tests/out/t1_full_chain.stderr`) — ~11% of edges would carry a fabricated waker; `t1_full_chain.folded` has 1619 stack lines through `sysvec_hyperv_stimer0`.
- Softirqs are fine (`softirq_entry/exit` fire inside `handle_softirqs` wherever softirqs run), so W9's `sync_read … ← IRQ` chain is reachable. Timers and IPIs are the hole.
- W5's DoD is an OR over `largest.cause`/`preemptor`/`waker`, so it passes with a garbage waker field.

**Change to** — drop the map and the four tracepoints; read the preempt count in the `sched_waking` program, as already written and tested in your tree (flag spellings are the tree's `LINK_*`, `src/chaingraph.h:35-41`).

```text
/* NOW, in sched_waking — src/chaingraph.bpf.c:134 preempt_count() (weak __preempt_count
   ksym, pcpu_hot fallback for v6.2-6.14), used exactly as src/chaingraph.bpf.c:309-320: */
pc = preempt_count();
if      (pc & NMI_MASK)      flags |= LINK_NMI;       /* terminal */
else if (pc & HARDIRQ_MASK)  flags |= LINK_HARDIRQ;   /* terminal */
else { if (pc & SOFTIRQ_OFFSET) flags |= LINK_SOFTIRQ;
       if (irq_exit_depth())    flags |= LINK_IRQEXIT; }
if (waker->pid == 0)         flags |= LINK_IDLE;      /* terminal, never "swapper" */
/* Any terminal edge: waker_tid = waker_tgid = 0. NEVER record `current` as the waker. */

# §2.3 line 108 WAS:
| `timer` | `nanosleep`/`clock_nanosleep` syscall flag, or wake edge with `irq` flag and thread in poll/sleep | blocked | Intentional sleep (frame limiter) — benign terminal |
# NOW:
| `timer` | `fentry/hrtimer_wakeup`: tag `container_of(timer, struct hrtimer_sleeper, timer)->task` with
  wake_cause=TIMER in task storage; same for `fentry/process_timeout`. Read at switch-in. | blocked | O(1),
  works at L0/L1. Sub-label: *intentional sleep* (nanosleep/clock_nanosleep, or pselect6/select with
  nfds=0 = Wine's non-alertable Sleep) = benign; *timed-out wait* = not automatically benign |
| `yield` | `fentry/fexit do_sched_yield` flag in task storage, read at the next `sched_switch` (issue 19;
  `do_sched_yield` is a FUNC in this kernel's BTF) | — | split out of `preempt`: Wine's `NtDelayExecution`
  yields before sleeping. Do **not** derive it from `!preempt && prev_state == TASK_RUNNING` — under
  `PREEMPT_LAZY` (this box boots "Dynamic Preempt: lazy") that signature is also ordinary
  return-to-user preemption. |
```

- Covers every source with zero per-interrupt hooks — device IRQs, LAPIC timer, stimer0, VMBus, IPIs, irq_work — on the VM and on Hetzner. Optional: keep task-context softirqs (loopback TCP) while cutting irq-exit ones by reusing the `irq_exit_rcu` fentry/fexit counter at `src/chaingraph.bpf.c:170–207`, attaching fexit before fentry (`src/chaingraph.c:1006–1030`) and resyncing to 0 in `sched_switch` (`:444–447`); ≈1.5k interrupts/s here, so budget it in M3, not as free.
- Hook `pselect6` and `ppoll` as well as `select`/`poll` (glibc 2.39's `select()` issues `pselect6`). Under Proton, DXVK/Wine frame-limiter sleeps land in `poll`/`futex`, not `timer` *(from web sources, verify)*. Verify `kfunc:hrtimer_wakeup` attachability in W1.
- New tests: (a) a walker test where a timer wakeup lands on a busy CPU whose interrupted task has a recent stall — assert no recursion into it; (b) hitchbench injectors reproducing the Proton pattern natively (`sched_yield` + `pselect6(0,…,&tv)`, `futex_waitv` with timeout, limiter thread waking render through a futex), run on the VM (stimer0) and Hetzner (LOC).
- Knock-on: §2.4 (`irq_state` row deleted; `edge_ring` flags gain nmi/irqexit/idle), §2.3 (`timer`; the `preempt` row is issue 24's), §2.6 steps 3–4 (terminal = "flagged or waker_tid==0"), §4 line 219, W5/W9 DoDs.

---

### 15. `gpu_wait` hooks the one fence path RADV does not use, and the obvious replacement cannot be attached (major)

**Plan says** (§2.3 line 106; §4 W2–W3 line 219):

> | `gpu_wait` | `fentry/fexit dma_fence_wait_timeout`, `dma_fence_default_wait` | blocked inside | "CPU waited on GPU" — the CPU/GPU-bound triage signal; not *why* the GPU was slow |

> hooks for futex, iowait bitfield, `handle_mm_fault` fentry/fexit, reclaim, compaction, `dma_fence_wait_timeout`, sleep/poll syscalls

**Why it breaks**

- RADV's `vkWaitForFences`/`vkWaitSemaphores`/`vkQueueWaitIdle` go through Mesa's common runtime to DRM syncobj ioctls (`vk_drm_syncobj.c:318–349`) *(from web sources, verify)*. The kernel side, `drm_syncobj_array_wait_timeout` (drm_syncobj.c:1033–1176), runs its own `set_current_state`/`schedule_timeout` loop (line 1172) and never calls `dma_fence_wait_timeout`; `dma_fence_wait_any_timeout` (dma-fence.c:887–936) is a third loop the hook misses. Those waits land in `other_block` with `prev_state == S`, `in_iowait == 0`.
- The obvious replacement will not attach: `/proc/kallsyms` has only `t drm_syncobj_array_wait_timeout.constprop.0` while BTF carries a plain FUNC, and `bpf_check_attach_target` resolves via `kallsyms_lookup_name()` — exact `strcmp`, no suffix stripping (kernel/kallsyms.c:227–240) — failing with "The address of function %s cannot be found" (verifier.c:25222). Attachable global symbols here: `T drm_syncobj_wait_ioctl`, `T drm_syncobj_timeline_wait_ioctl`, `T dma_fence_wait_any_timeout`, `T dma_fence_signal_timestamp_locked`.
- Under DXVK the wait is not on the frame-critical thread: `vkWaitSemaphores` runs on `dxvk-queue`, while the game thread blocks in `SyncFrameLatency` → `NtWaitForAlertByThreadId` → `futex_wait`; vkd3d-proton uses a fence worker the same way *(from web sources, verify)*. A GPU-bound Proton frame is recorded as `futex`, inflating M0's kernel-observable share. The W2–W3 cut line ("drop `gpu_wait` to `other_block`") does not fix a mislabel.
- A per-tgid `gpu_wait` total is not a triage signal either: `dxvk-queue` sits in a fence wait for most of every GPU-busy frame, hitch or not.
- Nothing on the dev VM exercises the path: lavapipe's `lvp_pipe_sync` waits with `cnd_wait`, i.e. futex *(from web sources, verify)*; `/dev/dri` here has only `card1` (hyperv_drm), and neither it nor vkms/vgem sets `DRIVER_SYNCOBJ`, so W4's DoD cannot be met for `gpu_heavy`.
- `dma_fence_wait_timeout` is still worth keeping, but not as "CPU waited on GPU": `amdgpu_ctx_wait_prev_fence` blocks the *submitting* thread past `amdgpu_sched_jobs` (32) in-flight submissions. `dma_fence_default_wait` is nested inside it — hooking both is redundant.

**Change to** — tag on the **waker** side, so classification is independent of which syscall the waiter used.

```text
# WAS (§2.3 line 106): | `gpu_wait` | `fentry/fexit dma_fence_wait_timeout`, `dma_fence_default_wait` | ...
# NOW:
| `gpu_wait` | per-CPU nesting counter via `fentry/fexit dma_fence_signal_timestamp_locked` (T symbol + BTF
  here; every dma_fence_signal* variant funnels through it — same trick as chaingraph's irq_exit_rcu
  counter, src/chaingraph.bpf.c:170-207, fexit attached before fentry). At sched_waking, if the counter
  is set, mark the wakee "fence-woken"; a blocked interval ending in such a wake becomes `gpu_wait` and
  the chain terminal reads "GPU fence". | covers syncobj ioctls, sync_file/dma-buf poll,
  dma_fence_default_wait and dma_fence_wait_any_timeout, whatever the waiter called |
| `submit_wait` | `fentry/fexit drm_syncobj_wait_ioctl`, `drm_syncobj_timeline_wait_ioctl` (global T
  symbols; read `flags` from the kernel-copied `data` arg). WAIT_AVAILABLE = waiting for another
  thread's submit, not for the GPU; Mesa always sets WAIT_FOR_SUBMIT, so these include CPU-side
  submit latency. | separate bucket, not gpu_wait |
| `submit_backpressure` | keep `fentry/fexit dma_fence_wait_timeout` (drop `dma_fence_default_wait`,
  nested inside it) | amdgpu_ctx_wait_prev_fence at >32 in-flight submissions, CS_WAIT, eviction waits |
```

- Never point fentry at a symbol that exists only as `.constprop.0`/`.isra.0`. Add a W2 load test checking **every** attach target against an exact `/proc/kallsyms` match, not just BTF — a BTF-only check (what `src/chaingraph.c` main does today) passes here and then fails the whole object load. Load optional hooks so one failure drops one hook, and log failed attaches in the manifest.
- Issue 16's switch-out `orig_ax`/ioctl-cmd classifier subsumes the syncobj *entry-point* hooks if verifier or attach cost forces a cut; the waker-side `dma_fence_signal_timestamp_locked` counter stays regardless, because it is what makes classification independent of the waiter's syscall.
- Proton triage needs one more hop and need not wait for L3: extend the L1 one-hop waker to carry the waker's last-stall cause plus the fence-woken bit, so the record reads `game futex ← dxvk-queue [gpu_wait ← GPU fence]`. `src/chaingraph.bpf.c:284–380` already copies the waker's links at `sched_waking`; add a cause byte to `chain_link` (defined once in issue 24). Mandatory for M0 even if the `gpu_wait` bucket is cut.
- WSI acquire needs a layer hook, not a BPF filter: with Wayland explicit sync, `vkAcquireNextImageKHR` blocks in `SYNCOBJ_TIMELINE_WAIT` outside the present window *(from web sources, verify)*. Add issue 7's `acquire_enter`/`acquire_return` brackets to §2.1's layer and account that time to `display_wait` (issue 7 owns the rename from `present_wait`).
- Validation split: on the VM test only that the hooks **attach**, plus a `sw_sync` unit test of the fence tag (`CONFIG_SW_SYNC`, debugfs `sync/sw_sync`). Move functional `gpu_wait`/`gpu_heavy` validation to the Hetzner bring-up week (old W6 — re-key per issue 35), with a new DoD before M0: a RADV `vkWaitForFences` microtest yields `gpu_wait > 0` and near-zero `other_block`.
- Knock-on: §2.3 (row 106 → three rows), §2.1 (layer brackets), §4 line 219 and its cut line (line 222 no longer describes the failure mode), W4/W5 DoDs, §5 M0 (report `gpu_wait` as a GPU-bound indicator, outside the kernel-stall numerator), Appendix C (under Proton `gpu_heavy` is diagnosed at L1 via the waker's cause; hitchbench needs a DXVK-like mode where a helper thread waits on the fence and wakes the render thread through a condvar).

---

### 16. Flag-setting syscall hooks: system-wide cost and wrong buckets (major)

**Plan says** (§2.3 lines 95, 109; §2.4 line 125; §4 line 219):

> Classification is decided **at switch-in** of a thread of interest, from flags set by other hooks plus the `sched_switch` state.

> | `poll` | `poll/ppoll/epoll_wait/select` flag | blocked | Sockets/pipes → wineserver hops (chain) |

> | `fd_names` | hash | (tgid, fd) → name[64] | 8192 | From `sys_enter_openat` filename (v2 file attribution) |

**Why it breaks**

- Any syscall tracepoint slows every syscall on the machine: `syscall_regfunc()` walks `for_each_process_thread` and sets `SYSCALL_TRACEPOINT` on every task (kernel/tracepoint.c:762–776). `syscalls:sys_enter_futex` alone pushes Steam, the compositor and wineserver through the slow entry/exit path for the whole run. `perf --off-cpu` and delayacct do not pay it, so it is an avoidable handicap in your own M3 table.
- 32-bit processes get no classification at all: `CONFIG_IA32_EMULATION=y` plus `ARCH_TRACE_IGNORE_COMPAT_SYSCALLS` (arch/x86/include/asm/ftrace.h:155) makes `trace_get_syscall_nr()` return −1 for compat syscalls (trace_syscalls.c:68) on the `perf_syscall_enter` path BPF uses. Proton still ships an i386 Wine, so every 32-bit PE title is silently unclassified *(from web sources, verify)*.
- `gpu_wait` misses the path RADV/DXVK actually take (issue 15); the switch-out classifier below is how a hook-free design recovers it.
- `timer` and `poll` are mislabelled under Proton: non-alertable `NtDelayExecution` is `select(0, NULL, NULL, NULL, &tv)`, and this glibc's `select()` issues **pselect6** (nr 270), absent from the plan's list — a frame limiter lands in `other_block`. Wineserver RPC blocks in `read()` on a pipe, not poll, so "Sockets/pipes → wineserver hops" never fires *(from web sources, verify)*.
- `fd_names` from `sys_enter_openat` fails twice: the fd is only known at exit, and under Proton the **wineserver** performs the `open()` and passes the fd over SCM_RIGHTS *(from web sources, verify)*.
- The information is already free at switch-out, where the plan captures a depth-8 kernel stack: on this kernel those read `__schedule, schedule, futex_do_wait, __futex_wait, futex_wait, do_futex` (`tests/out/t4_kernel_stacks_human.txt:45-47`), and `src/syms.c` `ksyms_lookup()` already symbolizes them.

**Change to**

```text
Hook set (replaces the W2-W3 BPF list, line 219)
  keep   tp_btf/sched_switch, sched_waking, sched_wakeup_new                    (issue 20)
  keep   fentry/fexit irq_exit_rcu + the __preempt_count ksym  (replaces §2.4 irq_state, issue 14)
  keep   prev->in_iowait; fentry/fexit handle_mm_fault; psi_memstall_enter/leave (issue 13)
         -- i.e. only the causes that occur while ON-CPU, tgid-filtered
  keep   fentry/fexit dma_fence_wait_timeout as issue 15's `submit_backpressure`, and issue 15's
         waker-side dma_fence_signal_timestamp_locked counter -- the switch-out orig_ax/ioctl-cmd
         rule below is the zero-hook *cross-check* for syncobj waits, not a replacement for them
  drop   fentry/fexit dma_fence_default_wait (nested inside dma_fence_wait_timeout)
  drop   sys_enter/exit_futex, nanosleep/clock_nanosleep, poll/ppoll/epoll/select flags
  drop   sys_enter_openat (fd_names); irq_handler_entry/exit + softirq_entry/exit

Blocked-cause classification, at SWITCH-OUT, in the block that already takes the stack id:
  1. handle_mm_fault flag -> pagefault_minor/major. The flag wins over orig_ax: a fault taken in
     kernel mode inside a syscall still shows that syscall's orig_ax (a user-mode fault has -1).
  2. nr = bpf_task_pt_regs(prev)->orig_ax  (bpf_task_pt_regs is in bpf_base_func_proto under
     CAP_PERFMON, so tp_btf/sched_switch may call it):
       202 futex, 449 futex_waitv, 455 futex_wait      -> futex / nt_wait (issue 17)
       35 nanosleep, 230 clock_nanosleep               -> timer
       23 select, 270 pselect6, 7 poll, 271 ppoll, 232 epoll_wait, 281/441 epoll_pwait(2) -> poll
         ... but nfds == 0 (regs->di)                  -> timer   /* Wine Sleep(), DXVK limiters */
       16 ioctl, cmd = regs->si: 0xc0284e82/83 NTSYNC_IOC_WAIT_ANY/ALL -> nt_wait;
                                 DRM_IOCTL_SYNCOBJ_(TIMELINE_)WAIT     -> gpu_wait
       0/17/19 read/pread64/readv on a FIFO (task->files->fdt->fd[di]->f_inode) -> wine_rpc
     Second table when prev->thread_info.status & TS_COMPAT (0x0002): i386 240 futex, 422
     futex_time64, 142 _newselect, 413 pselect6_time64, 168 poll, 414 ppoll_time64, 162 nanosleep,
     407 clock_nanosleep_time64, 54 ioctl, 3 read, 449 futex_waitv.
  3. Otherwise: switch-out stack id -> cause hash; on a miss match ~8 frames against kallsyms
     ranges the daemon writes into .rodata (futex_do_wait, do_nanosleep, ep_poll/do_sys_poll,
     drm_syncobj_array_wait_timeout, ntsync_wait_any/all, pipe_read, folio_wait_bit_common),
     then cache it. Refresh module ranges on module load (ntsync, amdgpu).

fd_names: resolve the fd at the stall, not at open -- regs->di -> task->files ->
f_path.dentry->d_name + (s_dev, i_ino); for faults bpf_find_vma -> vm_file; or have the daemon
readlink /proc/<tid>/fd/<fd> when it emits the record.
```

- Keep classification in BPF, not the daemon: the cause-gated predicate must exclude benign `timer`/`display_wait` sleeps in kernel, and the K=64 stall ring overflows precisely on the many-small-stalls frames M2's `thousand_cuts` produces, so daemon-side classification loses exactly the data M2 needs.
- Knock-on: §2.3 (`futex`/`timer`/`poll` trigger columns), §2.4 (`irq_state`, `fd_names` rows), §4 line 219 and its cut line (`gpu_wait`/`poll`/`timer` become table entries, not hooks to drop), §5 M3 (state which tools pay the global syscall-tracepoint cost).

---

### 17. The Wine sync backend is uncontrolled, and `futex_waitv` is not a 6.7 feature (major)

**Plan says** (§2.3 line 101; §2.8 line 160):

> | `futex` | `sys_enter/exit_futex` (+ `futex_wait`, `futex_waitv` on ≥6.7) set/clear per-thread flag; capture `uaddr` | blocked while flag set | Wine fsync; `uaddr` histogram per frame detects convoys |

> "largest":{"cause":"futex","ns":7000000,"kstack":["futex_wait_queue","futex_wait","do_futex","__x64_sys_futex"]},

**Why it breaks**

- Proton 11 is ntsync-first: `server/inproc_sync.c get_inproc_device_fd()` opens `/dev/ntsync` unless `PROTON_NO_NTSYNC`, and NT-object waits become `ioctl(NTSYNC_IOC_WAIT_ANY/ALL)` → `ntsync_wait_any/all` → `schedule_hrtimeout_range_clock` (drivers/misc/ntsync.c:851/944) with `in_iowait` clear. No planned flag fires: Win32 event/semaphore/mutex waits — the dominant Wine wait class — land in `other_block` *(from web sources, verify)*.
- Which backend runs is unrecorded host state. Here `CONFIG_NTSYNC=m`, module not loaded, no `MODULE_ALIAS`/`modules.devname` entry and nothing in `modules-load.d`, so `/dev/ntsync` is absent and Proton falls back to fsync — the plan's "Wine fsync" is right *by accident*. One `modprobe ntsync` moves that time to `other_block`. SteamOS 3.7.20+ loads ntsync by default; Proton 10 titles have none. Bucket shares are then incomparable across runs, titles and machines *(from web sources, verify)*.
- Version error: `futex_waitv` is syscall 449 and dates from **5.16**; 6.7 added `futex_wake`/`futex_wait`/`futex_requeue` (454/455/456 in the local `asm/unistd_64.h`). Harmless on 7.0, but a citable slip.
- Not affected: total off-CPU time, so M0's headline share and the gate hold. What moves is the per-cause breakdown (M0, guaranteed scope) and Appendix C(a) for Proton titles.

**Change to**

```text
§2.3 — replace the futex row with two rows whose meaning is backend-independent:
| `futex`   | switch-out in __futex_wait / futex_do_wait (nr 202/455)       | user-space locks: Wine
             SRW/CS/CV/WaitOnAddress and native futexes |
| `nt_wait` | futex_waitv (449) from a Wine process, or ioctl 0xc0284e82/3  | Win32 event/semaphore/mutex
             waits; same bucket under fsync and ntsync |
  Detect ntsync by ioctl cmd (issue 16 step 2), NOT by fentry on ntsync_wait_any/all: that needs the
  module loaded at attach time and its module BTF.
  Version note: futex_waitv is 5.16; 6.7 added futex_wait/futex_wake/futex_requeue.
  §2.8 example kstack: `futex_wait_queue` does not exist on 7.0; use the real stack from
  tests/out/t4_kernel_stacks_human.txt -- see issue 38, which replaces the whole array.

§8 manifest — add, and hold fixed for M0:
  proton_version (one build in compatibilitytools.d, forced per title); dxvk_version;
  vkd3d_proton_version; steam_linux_runtime VERSIONS.txt; steam_client_version
  sync_backend: "ntsync" | "fsync" | "server"  <- from the wineserver stderr line
      "ntsync: up and running." / "fsync: up and running." /
      "wineserver: using server-side synchronization."
      (PROTON_LOG=1 enables a long WINEDEBUG list: read this on a calibration launch, never on a run)
  ntsync_loaded (lsmod), /dev/ntsync present, PROTON_NO_NTSYNC, PROTON_NO_FSYNC, WINEFSYNC,
  appid on Proton's built-in nofsync list?

setup/provision.sh: /etc/modules-load.d/ntsync.conf on BOTH machines; pin ntsync for M0 (matches
SteamOS >= 3.7.20 + Proton 11). This is the single pin issue 34 defers to; set the opposing variable
explicitly in every launch script so the default cannot drift. Run a one-title ntsync-vs-fsync A/B as
a side result -- not a fourth M0 condition (M0 is already ~56 h; a fourth adds ~19 h).
```

- W2–W3 DoD: hitchbench is native and exercises only plain `futex`. Add two injectors issuing `futex_waitv` and `NTSYNC_IOC_WAIT_ANY` through the uapi, each producing exactly one record in the right bucket.
- Knock-on: §2.3, §2.8, §8 manifest, §5 M0 controls, §4 W2–W3 DoD, §9 (BPF-feature version row).

---

### 18. The per-frame `uaddr` histogram cannot detect convoys under Wine (minor)

**Plan says** (§2.3 line 101):

> `sys_enter/exit_futex` (+ `futex_wait`, `futex_waitv` on ≥6.7) set/clear per-thread flag; capture `uaddr` … Wine fsync; `uaddr` histogram per frame detects convoys

**Why it breaks**

- Win32 user-mode locks never sleep on the lock address: `SRWLOCK`, `CONDITION_VARIABLE`, critical sections and `WaitOnAddress` all go `RtlWaitOnAddress` → `NtWaitForAlertByThreadId` → `futex_wait` on the **waiter's own** `tid_alert_entry`. A convoy of N threads on one SRWLOCK appears as N distinct uaddrs with one waiter each; DXVK and vkd3d-proton are PE builds wrapping these *(from web sources, verify)*.
- For fsync objects the syscall is `futex_waitv`, whose first argument is a pointer to a **stack array** of waiters, so "capture `uaddr`" captures a stack pointer. Under ntsync there is no uaddr at all (issue 17).
- Minor: no §1 claim or DoD depends on convoy detection and futex *time* is still correct. Only the record's hint field misleads, and only for Proton titles.

**Change to**

```text
§2.3 futex row, Notes column:
  was: Wine fsync; `uaddr` histogram per frame detects convoys
  now: uaddr identifies a lock only for native targets (hitchbench's own job system). Under Wine,
       CS/SRW/CV/WaitOnAddress sleep on the waiter's own tid-alert futex (NtWaitForAlertByThreadId),
       so uaddr identifies the waiter, not the lock; convoys come from the L1 waker and the chain.
If an object key is wanted for fsync kernel objects: fexit on futex_wait_multiple (a global symbol on
7.0, kernel/futex/waitwake.c); the return value is the index of the woken futex, so key on
vs[ret].w.uaddr -- not vs[0], and not the futex_waitv syscall argument. Skip the trailing APC futex,
present only for alertable waits.
```

---

### 19. Preempt/yield classification and the W1 bpftrace scripts (minor)

**Plan says** (§2.3 line 99; Appendix A lines 396, 405):

> | `preempt` | `sched_switch` with `prev_state == TASK_RUNNING` | out→in = runnable-not-running; record `next` as preemptor (tgid, comm)

> @offcpu[args->next_comm, @st[args->next_pid] == 0 ? "R(preempt)" : "blocked"] = sum($d);

> tracepoint:sched:sched_switch /curtask->tgid == $1 && args->prev_state != 0/ { @reason[kstack(3)] = count(); }

**Why it breaks**

- The rule must **not** be replaced by the tp_btf `preempt` argument alone. Most preemption of a game thread in user code is not `SM_PREEMPT`: returning to user with NEED_RESCHED calls plain `schedule()` from `exit_to_user_mode_loop` (kernel/entry/common.c:52–54), so `preempt = false`, `prev_state = RUNNING`. This box boots "Dynamic Preempt: lazy" (`CONFIG_PREEMPT_LAZY=y`), so that is the usual form — classifying by `preempt` alone files most real preemptions as yields. (Issue 24 owns the resulting `preempt` row: runnable-wait = `preempt || prev_state == TASK_RUNNING`, which also catches a task preempted between `set_current_state()` and `schedule()`.)
- But `sched_yield()` has the same signature (`do_sched_yield` → `schedule()`, SM_NONE), and Wine calls it in `NtYieldExecution` **and before every non-alertable `Sleep()`** *(from web sources, verify)*. Game-initiated spin time is then booked as `preempt` with a bogus preemptor, moving M0's game-initiated/external split to the wrong column. Hence the yield flag must come from `fentry/fexit do_sched_yield` (a FUNC in this kernel's BTF), never from `!preempt && prev_state == RUNNING` — issue 14 owns that taxonomy row.
- Appendix A uses the classic tracepoint, which returns `TASK_REPORT_MAX` (0x100) when `preempt` is true (include/trace/events/sched.h:190–203): A2 files in-kernel preemptions as "blocked" and A4 collects their stacks as blocked reasons. K is unaffected (A2's `@stalls` counts every switch-in), so only the R/blocked split must be re-run.

**Change to**

```text
Appendix A — mask the classic tracepoint's state (0x100 = TASK_REPORT_MAX):
  A2: @offcpu[args->next_comm, (@st[args->next_pid] & 0xff) == 0 ? "R(preempt)" : "blocked"]
  A4: /curtask->tgid == $1 && (args->prev_state & 0xff) != 0/
  Add a per-tid tracepoint:syscalls:sys_enter_sched_yield counter so W1 measures the yield share on a
  Proton title.
Never look for a wake edge on a runnable-wait interval: the task was never dequeued.
Rows: `preempt` (with issue 23's occupant fields) is issue 24's; `yield` is issue 14's.
```

- Knock-on: §4 W1 items 2 and 4 (re-run the R-vs-blocked split and the blocked-reason note with the mask; the stalls-per-frame number that sets K does not change), and the new `yield` bucket must appear in §2.3, §2.8 `totals_ns` and M0's game-initiated/external split.

## D. Chains, maps and emission

How wakers are matched, what the maps can hold, and what emission costs on the game's own thread.

### 20. Waker matching: the edge ring, the `out ≤ ts ≤ in` rule, and the hop rule (major)

**Plan says** (§2.3 line 100; §2.6 lines 136, 138):

> | `runqueue` | wakeup edge inside a blocked interval | `in_ts − wake_ts` | Falls out of time-interval matching |

> 1. Find the edge with `wakee == tid` and `out ≤ ts ≤ in` (bounded scan; time-interval matching, **not** "last waker" — a thread wakes many times per frame).

> 3. Hop rule: look at the waker `W` immediately before `ts`. If `W` had just resumed from a stall ending ≤ `ts` (its own stall ring) → recurse into that stall. If `W` was running continuously → terminate: cause is `W`'s own execution …

**Why it breaks**

- `ts < out` is normal, not an edge case. `trace_sched_waking` fires under `p->pi_lock` right after `ttwu_state_match` (`core.c:4135`); the wakee's `trace_sched_switch` comes later (`core.c:6908`), after `pick_next_task` drops the rq lock in `sched_balance_newidle` (`fair.c:12982/13034`) — exactly the path taken when a game thread blocks on an otherwise idle CPU. `out ≤ ts ≤ in` then matches nothing: `runqueue` reads 0, the whole interval is billed to the blocked bucket, with no waker and no chain. chaingraph already hit and documented this (`src/chaingraph.bpf.c:475-483`, README:126).
- The stated reason for interval matching does not hold per sleep. After one successful `ttwu_state_match` the state is `TASK_WAKING`/`RUNNING`, so exactly one `sched_waking` ends a given sleep and it lies in `(prev_in, in]`. "Latest waker per sleep, consumed at switch-in" is correct and O(1); only a per-*frame* last waker would be wrong.
- `runqueue` (L0) and the W5 one-hop waker are guaranteed scope with no data source in the guaranteed weeks: the W2–W3 hook list (line 219) has no `sched_waking`, and the edge ring is wired in W9 (stretch). Matching at `frame_end` means dumping edges at L1, which erases the L1/L3 volume split M1 and M3 rest on.
- The hop rule needs *W's* stall ring, and stall rings exist only for threads of interest (§2.3, §2.4), so every chain terminates at the first kworker, wineserver, pipewire or compositor thread and wrongly reports "W's own execution".

**Change to**

```
§2.3 runqueue row
  now: | `runqueue` | wake stored on the wakee at sched_waking, consumed at switch-in | `in − max(wake_ts, out)` | O(1); no edge scan |
       blocked-cause bucket gets max(0, wake_ts − out); the two sum to in − out exactly.
       woken == 0 -> count a no_waker stat; do not search for an edge.

§2.6 steps 1-3 (replace)
  1. The wake that ends [out, in] is the one recorded on the task since its previous switch-in
     — NOT "the edge with out ≤ ts ≤ in". tp_btf/sched_waking + sched_wakeup_new write
     {ts, waker tid/tgid/comm, hardirq/softirq/irq-exit/idle flags} into the wakee's
     BPF_MAP_TYPE_TASK_STORAGE and set woken=1; tp_btf/sched_switch consumes it at switch-in
     (chaingraph.bpf.c:472-483, record_wakeup at :280). Flags come from preempt_count() plus
     the irq_exit_rcu nesting counter (:126-142, 170-186), not from an
     irq_handler_entry/softirq_entry map: timer sysvecs never fire irq_handler_entry, and
     softirqs run inside irq_exit_rcu with the HARDIRQ bits already cleared.
  2. blocked = max(0, ts − out);  runqueue = in − max(ts, out)
  3. Hop rule: split, don't choose, using the per-link boundary fields defined once on
     `chain_link` in issue 24:
       if (W.last_wake_in <= T.out) -> terminal: "W executing" / "W preempted by X"
       else [T.out, W.last_wake_in] -> recurse into W's stall (clip with the stored bounds);
            [W.last_wake_in, ts] -> W running;   [ts, in] -> runqueue
     Cut on `last_wake_in`, never `last_switch_in`: a brief preemption of W after T.out would
     otherwise postdate T.out and keep W's stale chain (chaingraph README:195-197).
  Wakers outside the target need ~350 B of chain state, not a stall ring: keep chaingraph's
  task_states for all tasks with the depth-1 early filter (chaingraph.bpf.c:299).
```

- Move `tp_btf/sched_waking` + `sched_wakeup_new` from W9 into the W2–W3 hook list (line 219); W5's waker and gap then depend on no ring. Keep the edge ring only as the v1 validation oracle.
- W1: count wakings whose `ts` precedes the wakee's next S switch-out, to size the race on the futex job system; add walker cases for `ts < out`, an orphan waking before `out`, and a DELAY_DEQUEUE "S" interval that was runnable throughout.
- The per-configuration W9 DoD chains for `sync_read` are in issue 37; the `kworker` hop goes in all of them.
- Knock-on: §2.3 `runqueue` row, §2.4 `edge_ring`/`irq_state`, §4 W2–W3 / W5 / W9, §5 M1, §1 claim 3 (wording, see issue 22).

### 21. `stall_ring` as a hash of arrays cannot be built from BPF; the tid hash and tid registry are unnecessary (major)

**Plan says** (§2.4 lines 120, 121; §2.5 line 130):

> | `thread_acc` | hash | tid → {epoch, ns[N_CAUSES], largest{cause,ns,stackid}, preemptor{tgid,comm,ns}, flags} | 4096 | …

> | `stall_ring` | hash of arrays | tid → ring[K] of {out_ns, in_ns, cause, stackid, preemptor_tid, flags} | K=64 (tune from W1 data) | Single writer per thread (switch-in), so no locking. …

> then per-thread details by iterating a bounded `tgid → tid[256]` registry with `bpf_loop`

**Why it breaks**

- BPF cannot populate a map-of-maps: `kernel/bpf/verifier.c:10240-10243` allows only `BPF_FUNC_map_lookup_elem` on `HASH_OF_MAPS`/`ARRAY_OF_MAPS`, so the daemon would have to create one inner ring per thread, racing thread creation — and the stall ring is W5, guaranteed scope.
- The tid-keyed hash has no exit path. Nothing deletes entries, so a 45-minute session with Wine/engine thread churn fills the 4096 cap, and a reused tid in another tgid matches a stale epoch.
- The `tid[256]` registry cannot enrol threads that already exist: it is filled at layer `init` plus `sched_process_fork`, but under Proton most threads exist before `vkCreateInstance` runs.
- "so no locking" ignores the reader: the `frame_end` uprobe reads other threads' rings from another CPU while they are written. chaingraph solves this with an odd/even seq counter (`task_state.seq`, `LINK_TORN`, `chaingraph.bpf.c:345/402`).
- §7's mitigation `bpf_for_each_map_elem` cannot iterate task storage (no `map_for_each_callback`); the open-coded task iterator is the tool.

**Change to**

*Reset scheme: cumulative counters with per-frame differences (issue 3). `epoch` disappears. The `seq` counter below and issue 22's double-buffering guard torn reads of `ring[]`/`largest` only — they are not a zeroing mechanism.*

```
§2.4 — delete the thread_acc and stall_ring rows; one map instead:

struct thread_acc {                    /* BPF_MAP_TYPE_TASK_STORAGE, BPF_F_NO_PREALLOC */
    __u64 ns[N_CAUSES];
    struct { __u32 cause; __u64 ns; __s32 stackid;
             struct chain_link links[6]; } largest;   /* chain copied in at switch-in */
    struct { __u32 tgid; char comm[16]; __u64 ns; } preemptor;
    __u32 seq;            /* odd while ring[]/largest is rewritten (chaingraph.bpf.c:345) */
    __u32 head, truncated;
    struct stall ring[K];  /* K=64 -> ~2.7 kB; cap is ~64 kB minus a 128 B header,
                              include/linux/bpf_local_storage.h:113 */
};
Create with BPF_LOCAL_STORAGE_GET_F_CREATE for threads of interest only, at USDT `init` or at
the thread's first switch-out — not from the sched_switch fast path, where a 2.7 kB allocation
under the rq lock is expensive.

§2.5 — drop the `tgid -> tid[256]` registry. At frame_end:
    bpf_iter_task_new(&it, bpf_get_current_task_btf(), BPF_TASK_ITER_PROC_THREADS)
    /* wineserver / other tgids: BPF_TASK_ITER_ALL_PROCS + bpf_task_under_cgroup */
bpf_iter_task_new/next/destroy are in common_btf_ids (kernel/bpf/helpers.c:4618) registered for
BPF_PROG_TYPE_UNSPEC, KF_RCU_PROTECTED (accepted in non-sleepable programs), and present in
this machine's BTF. Threads that exit mid-frame drop out of per-thread detail but stay in the
per-tgid totals; flush them at sched_process_exit if that matters.

§2.2 — `children tracked via sched_process_fork` then has no purpose: task storage is created
on demand and freed with the task. Keep sched_wakeup_new only for the chain (issue 20).
```

- Whether a uprobe may call the task iterator and `bpf_task_storage_get` at all is settled by the W1 verifier gate in issue 24. If the verifier rejects it, keep a tid-keyed snapshot map plus `bpf_loop`, and this issue's §2.5 change applies only to the `tid[256]` registry.
- Knock-on: §2.2, §2.4, §2.5, §4 W2–W3 ("`bpf_loop` over the tid registry") and W5, §7 ("Concurrency in rings", "Verifier friction").

### 22. The L3 window dump runs on the present thread and does not fit the ringbuf/verifier rules (major)

**Plan says** (§2.4 line 126; §2.5 line 130):

> | `events` | ringbuf | — | 8 MB | Records only for bad frames |

> If over budget → reserve ringbuf record, write header + per-tgid totals, then per-thread details … L3 additionally copies edge slots newer than `frame_start − 2 frames` and the relevant stall rings. Then bump `epoch`

**Why it breaks**

- `bpf_ringbuf_reserve` needs a verifier-constant size (`verifier.c:10103-10108`, `ARG_CONST_ALLOC_SIZE_OR_ZERO`), so a variable-length window cannot be one reserve; and `ringbuf.c:451` refuses a reserve while `prod − cons > mask`, so a near-MB record into an 8 MB ring drops on hitch clusters — shader-compile storms, the case you most want.
- Cost scales with wakeup rate × hitch length, not with threads: reading other CPUs' slots needs `bpf_map_lookup_percpu_elem` per slot. At the ~5k `sched_waking`/s measured on this VM a 3-frame window is ~250 edges (~8 KB); a 300 ms hitch at 100k wakeups/s is ~33k edges (~1 MB) — milliseconds inside a uprobe.
- It runs on the present thread inside a held lock. DXVK calls `vkQueuePresentKHR` on `dxvk-submit` while holding `m_mutexQueue`, and vkd3d-proton wraps it in `vkd3d_queue_acquire/release`, so every queued GPU submission waits behind the dump — the tool can cause the next hitch. (from web sources, verify)
- The cost is invisible in your own numbers and lands on the tail: under §2.2 the dump falls between `frame_end(N)` and `frame_begin(N)`, inside no `frame_ns` — but MangoHud's present-to-present time (the W6 agreement check and M3's Δp99/1%-low ground truth) does include it.
- Edge slots carry no seq, so a torn edge is undetectable.

**Change to**

*Reset scheme: cumulative counters with per-frame differences (issue 3). `epoch` disappears; the double-buffering below guards torn reads of `ring[]`/`largest` only — it is not a zeroing mechanism.*

```
§2.5 — replace the L3 clause:
  now: snapshot the cumulative counters first, then emit fixed-size records only — per-frame
       values are differences, so anything arriving during emission lands in the next frame
       rather than being lost;
         1 header + one constant-size bpf_ringbuf_reserve per thread inside bpf_loop.
       Header gains: per-CPU edge heads, window [t0, t1], and handler_ns
       (bpf_ktime_get_ns at handler entry and exit).
  The L3 chain comes from the links already copied into `largest` at switch-in (issue 20),
  so emission is O(threads x depth) and independent of the wakeup rate.

If a full edge window is still wanted as M1's "full trace" rung:
  edge_ring: plain BPF_MAP_TYPE_ARRAY with BPF_F_MMAPABLE, ncpu*S slots, index
  cpu*S + (head & (S-1)), seq and ts written LAST. (BPF_F_MMAPABLE is rejected on
  PERCPU_ARRAY: kernel/bpf/arraymap.c:66-68. Each CPU's partition still has a single writer:
  tp_btf programs do not re-enter on the same CPU.)
  frame_end then emits only a ~64 B notice {tgid, frame_id, t_start, t_end}; the daemon copies
  the window from the mmap and re-reads the heads afterwards to set edge_ring_overflow.
  Retention = S / per-CPU wakeup rate. Pin the daemon outside the game's AllowedCPUs.
```

- M3: report `handler_ns` per bad frame, and run the L3 arm at the 1/s hitch rate where hitch frames (≈1.7% at 60 fps) dominate p99.
- Knock-on: §1 claim 3 — "at always-on cost" becomes "online-propagated, frame-gated chains", and the per-wakeup propagation cost is what M3 must measure; §2.6's v1/v2 volume graph (v1 no longer exists as specified); §4 W9; §5 M1/M3; §7 ("L3 v1 volume ≈ ftrace snapshot" stops being a risk).

### 23. Preemptor identity is the first occupant, and runqueue waits get no occupant at all (major)

**Plan says** (§2.3 line 99; §4 line 232; Appendix C line 425):

> | `preempt` | `sched_switch` with `prev_state == TASK_RUNNING` | out→in = runnable-not-running; record `next` as preemptor (tgid, comm); keep top-1 by duration per thread per frame | The *cause* is the preemptor, not a waker |

> - DoD: for each hitchbench injector, the record's `largest.cause`, `preemptor`, or `waker` names the injected mechanism in ≥ 95% of injected frames.

> (b) where applicable, the correct counterpart: preemptor comm (`holder_preempt`, `bg_periodic`)

**Why it breaks**

- `next` is the first task picked, not the task that held the CPU. Under EEVDF with `RUN_TO_PARITY` and `PREEMPT_SHORT` (`kernel/sched/features.h`), `check_preempt_wakeup_fair` leaves the current task running for the rest of its protected slice — base 0.7 ms × (1 + ilog2(ncpus)) ≈ 2.8 ms on this 10-vCPU box — so a 10–50 µs kworker that happens to be picked first is named as the preemptor of a wait dominated by the hog.
- Those short kthreads are common: sampled from `/proc/*/task/*/schedstat` over 10 s on this idle VM, `rcu_preempt` alone runs ~46×/s, and the unbound `kworker/uNN:*` pool adds tens more at 10–50 µs each (the workqueue is a comm *suffix* — `kworker/u40:0+events_unbound` — not a task of its own). The busier the box, the worse it gets: on this VM the application threads already run 240–440 times a second each. It becomes the normal case under `AllowedCPUs=0-3` and in the M0 real-game runs.
- "Top-1 by duration" ranks by the *victim's* wait, which favours exactly the kworker-first waits, while `preemptor.ns` stores the victim's wait rather than the occupant's runtime.
- Runqueue waits have no occupant field anywhere (§2.8 has only `waker.gap_ns`, and `waker` is who woke the thread, not who held the CPU), and the edge ring holds wakeups only. So Appendix C(b) cannot be met for `bg_periodic`, which mostly delays an already-sleeping thread, and M0's game-initiated vs external split cannot be computed for preempt/runqueue time at all.

**Change to**

```
§2.3 preempt and runqueue rows — same occupant fields on both:
  first_occ {tgid, comm}  preempt : `next` at switch-out
                          runqueue: bpf_per_cpu_ptr(&runqueues, task_cpu(p))->curr read at
                                    tp_btf/sched_wakeup -- NOT sched_waking, which fires
                                    before select_task_rq/set_task_cpu (core.c:4135 vs
                                    4230-4239). `runqueues` is a VAR in this kernel's BTF;
                                    sched_wakeup fires from ttwu_do_wakeup (core.c:3631).
  last_occ  {tgid, comm}  = `prev` in the sched_switch that gives the victim the CPU back
                            (free; names the hog or daemon in the kworker-first case)
  top_occ   {tgid, comm, run_ns} = max by summed run_ns over [start, in] on the wait CPU
  migrated  flag from tp_btf/sched_migrate_task (core.c:3283)
  Keep choosing WHICH stall to report by victim wait; name the occupant by run_ns.

Own-vs-foreign split (M0 needs it): two per-CPU counters bumped at every sched_switch --
  target_ns (registered tgids) and foreign_ns (everything else, idle excluded).
  Snapshot (cpu, values) into the victim's task storage at preempt switch-out / sched_wakeup;
  at sched_migrate_task close the old CPU's segment with bpf_map_lookup_percpu_elem and reopen
  on the new one; diff at switch-in.

top_occ source: per-CPU ring of 64 {ts, pid, tgid, run_ns} written at every sched_switch,
  scanned with bpf_loop only for waits above ~500 us, with a wrapped flag. Its per-switch
  write cost belongs in M3.

§2.8 record: add "occupant" beside "waker" for preempt and runqueue stalls.
Appendix C(b): for `bg_periodic` and `holder_preempt`, treat preempt+runqueue together as CPU
  contention and accept first/last/top occupant as the counterpart, not only `preemptor`.
```

- hitchbench must specify how the injected load contends (CPU saturation, which CPUs the daemon may use, which cgroup); otherwise `select_idle_sibling` puts `bg_periodic` on an idle CPU and it injects nothing.
- Knock-on: §2.3 (both rows), §2.4 (`thread_acc.preemptor`), §2.8, §4 W5 DoD (the ≥95% target depends on this for the contention injectors), §5 M0, Appendix C.

### 24. Claim 3 rests on stretch work while guaranteed scope already pays for the edge ring (critical)

**Plan says** (§1 line 30; §4 lines 200, 231, 253):

> 3. On-demand chain walking over a bounded edge window recovers cross-thread/process blocking chains for outlier frames at always-on cost; this addresses the "latency outliers" case Gregg's chain-graph work explicitly left open. (M1, L3)

> Guaranteed scope: **L0 + L1 + harness + baselines + M0 + M2 + M3**. Stretch: **L3 v1 + M1**, then in-kernel walk.

> - Cut line: if W8 slipped, L3 is design-only in the thesis (§2.6 + cost analysis) and W9 goes to M3.

**Why it breaks**

- The scope is self-contradictory: a "Claimed (and measured)" claim cites only stretch deliverables and the W9 cut line permits L3 to be design-only — yet guaranteed W5/L1 already needs "one-hop waker with gap (runqueue) from the edge ring" (line 231), guaranteed W10/M3 lists an `L3-v1` configuration (line 256), and M2's result-either-way says "If aggregate hitches are rare, thesis narrows to L3" (line 276). You pay for the edge ring in guaranteed scope and may get no claim for it.
- The justification for a window is wrong on 7.0: `trace_sched_waking` fires only after `ttwu_state_match` succeeds (`kernel/sched/core.c:4116-4119`), so each blocked interval gets exactly one wakeup, and consuming it at switch-in *is* per-interval matching — which is what `src/chaingraph.bpf.c` `on_switch()` already does (lines 481–483).
- Your own repo already does the expensive part online and bounded: `record_wakeup()` (`chaingraph.bpf.c:280-417`) sets `wakee.links[0] = waker`, `links[1..] = waker.links[0..]`, depth ≤ 8, seq-guarded against torn reads, terminating at hardirq/NMI/irq-exit softirq/idle, with `sched_wakeup_new` covering fork edges. Measured on this VM: `tests/out/t1_full_chain.stderr` → `wakeups=32757` over 6 s (~5.5k/s), `torn=0`; `tests/run_tests.sh` t1 asserts `cg_sink ← cg_relay2 ← cg_relay1 ← cg_source ← [hardirq]` with stack regexes. The plan never mentions it.
- It is not free: `struct chain_link` (`src/chaingraph.h:55-63`, 40 B) has no timestamps, chains land in an aggregate hash rather than per-stall records, nothing records preemption (so §2.8's `Worker3 … preempted 6.9 ms by steamwebhelper` hop cannot be produced), and the README caveat — a long-running waker still carries the chain that last woke it (README:195-197) — is exactly the case §2.6's hop rule 3 terminates on.

**Change to**

- Replace the edge ring, the window dump and `walker/` with online propagation on task storage, and move L3 into guaranteed scope. This is the review's single definition of `chain_link`; issues 2 and 20 cite it rather than redefining it.

```c
/* replaces §2.4 edge_ring + §2.5 window copy + §2.1 walker/ */
struct task_state {            /* task storage, ALL tasks; chaingraph has this map */
    __u64 wake_in_ns;          /* switch-in that CONSUMED the wakeup (bpf.c:481-483) */
    __u64 preempt_ns;          /* since wake_in_ns */
    __u32 preemptor_tgid; char preemptor_comm[16];
    struct { __u32 cause; __u64 out, wake_ts, in; } last_stall;
    struct chain_link links[8];
};
struct chain_link {            /* 40 B today -> ~88 B; depth-8 snapshot ~700 B */
    __s32 kstack_id, ustack_id; __u32 pid, tgid, flags; char comm[16];  /* existing */
    __u64 wake_ts;        /* when this waker woke the task below it */
    __u64 last_wake_in;   /* switch-in that ended the waker's last VOLUNTARY sleep */
    __u32 oncpu_ns;       /* wake_ts - last_wake_in - preempt_ns (freshness) */
    __u32 last_cause, last_stall_ns, runq_ns;
    __u32 preempt_ns, preemptor_tgid; char preemptor_comm[16];   /* issue 23 */
};
/* Build the snapshot in a per-CPU scratch map (like key_scratch), not on the 512 B BPF
   stack; snapshot at the target's switch-in, top-N stalls per thread per frame. */
```

**W1 gate, one 30-line BPF object:** does a `SEC("uprobe/...")` program load and verify while calling `bpf_iter_task_new`/`bpf_iter_task_next` over `BPF_TASK_ITER_PROC_THREADS` and `bpf_task_storage_get` on the yielded tasks? If it loads, issue 21's design stands and the tid-keyed snapshot map in issues 3 and 21 is dropped. If the verifier rejects it, keep the tid-keyed map plus `bpf_loop`, and issue 21's §2.5 change applies only to the `tid[256]` registry. Do not write the daemon against both. (`bpf_task_from_pid` is not the way in: the `frame_end` uprobe is `BPF_PROG_TYPE_KPROBE` and that helper is registered only for TRACING, SCHED_CLS, XDP, STRUCT_OPS, SYSCALL, CGROUP_SKB — `helpers.c:4711-4716`.)

- Freshness replaces the in-kernel hop rule, and the preempt trigger changes:

```diff
-3. Hop rule: ... If `W` was running continuously → terminate: cause is `W`'s own execution
+3. Copy stale links; apply freshness OFFLINE as a sweepable M1 parameter:
+   terminate at W when link.oncpu_ns > X (or > f × stall_ns).
+   Measure oncpu from W's WAKE-IN, not its last switch-in: v7.0 __schedule calls
+   trace_sched_switch on preemption too, so last_in moves on every preemption and a
+   stale chain would look fresh. Detect A→B→A at render time.
```

```diff
-| `preempt` | `sched_switch` with `prev_state == TASK_RUNNING` |
+| `preempt` | `tp_btf/sched_switch`, runnable-wait = `preempt || prev_state == TASK_RUNNING`,
+             minus the `yield` flag (issue 19) |
```

  Reason: v7.0 passes the raw `prev->__state` to `trace_sched_switch` even when `preempt` is true, so a task preempted between `set_current_state()` and `schedule()` reports a sleeping state and is misbucketed — but the `preempt` argument alone is not sufficient either, because under `PREEMPT_LAZY` return-to-user preemption arrives with `preempt == false`. The disjunction catches both; see issue 19 for the yield split.

- Keep a ≤ 8-frame kernel stack only on interrupt-context links — timer sysvecs never fire `irq_handler_entry`, so §2.4's `irq_state` cannot tell `hrtimer_wakeup` from block completion from net RX. Drop user stacks and process-context waker stacks in always-on mode.
- Move the chain DoDs from W9 into W5 and assert terminal type plus kernel-stack regexes like `tests/run_tests.sh` t1. The per-configuration `sync_read` DoD chains are in issue 37; the `kworker` hop goes in all of them.
- Knock-on: §0 (the "bounded in-kernel window of wakeup edges … walked **only for bad frames**" sentence is no longer the design — the cost is per wakeup, the saving is at emission); §1 claim 3; §2.1 (`walker/` becomes an offline validator over `trace-cmd`/`perf sched record`, serving as M1's "full trace" rung); §2.4/§2.5/§2.6 (the v1-vs-v2 volume graph disappears — replace with "bytes per hitch: online chain vs ftrace snapshot" in M3); §4 W5/W9/W11 and the guaranteed/stretch line; §7 rows "`sched_waking` system-wide BPF cost" (now two storage lookups per switch plus one per wakeup — M3 must measure it at the game's real wakeup rate) and "L3 v1 volume ≈ ftrace snapshot" (delete); §10 item 3.

## E. Baselines and scripts

The comparators the thesis rests on, and the commands as written.

### 25. The `perf --off-cpu` baseline cannot run, and its sweep cannot be expressed (major)

**Plan says** (§6.2, line 301; §9, line 363 — "Confirm perf version on the bench box"):

> `perf record --off-cpu --off-cpu-thresh <us> -p <pid>` (needs perf ≥ 6.16 for `--off-cpu-thresh`; verify). Sweep thresholds 100/500/2000 µs for M2.

**Why it breaks**

- The packaged perf has no BPF skeletons, so `--off-cpu` is a silent no-op: `bpf_skeletons: [ OFF ]` in `--build-options`, a run warns ``option `off-cpu' is being ignored`` and records cpu-clock samples (`builtin-record.c` v7.0:4107 `set_nobuild(..., true)`; `parse-options.c:126-146` only warns). A version check passes at 7.0.14, §3.3 installs the same binary on Hetzner, and W4's DoD ("produces output") accepts that perf.data — M2 and M3 get plausible wrong numbers.
- Units are ms, not µs (`builtin-record.c:3242-3259`, `strtoull` × `NSEC_PER_MSEC`): `500us` is rejected; `100/500/2000` silently means 100 ms / 500 ms / 2 s.
- `can_record()` (`off_cpu.bpf.c:176-178`) accepts only TASK_(UN)INTERRUPTIBLE, so R-state preemption and on-CPU faults are invisible at *every* threshold — `holder_preempt`, `bg_periodic`, the fault half of `thousand_cuts`. At d ≈ 0.5 ms against a 1 ms floor the M2 curve is a step function.

**Change to**

```sh
# replacement §6.2 perf off-CPU with threshold
make -C tools/perf BUILD_BPF_SKEL=1 prefix=/opt/perf-skel install   # provision.sh, both machines
perf version --build-options | grep -q 'bpf_skeletons: \[ on' || exit 1      # provisioning gate
/opt/perf-skel/bin/perf record --off-cpu --off-cpu-thresh <ms> -k CLOCK_MONOTONIC \
    -a -G <game-scope-cgroup>         # cgroup scope per §2.2, so wineserver is covered too
# was: perf record --off-cpu --off-cpu-thresh <us> -p <pid>; sweep 100/500/2000 µs.
# Whole ms only: sweep T = 0, 1, 2, 4, 8 ms.  T=0 streams every S/D interval as a timestamped
# sample; sub-ms thresholds come from post-processing that log, not from perf.
# DoD: `perf evlist` lists offcpu-time and `perf script` shows the injected ~30 ms stall;
#      the script aborts if perf's stderr contains "is being ignored".
```

- Capture is per frame, never from the aggregates: clip each sample's `[t−period, t]` to each frame window, sum, apply hitchtrace's gate, discard the session-end totals (`off_cpu.bpf.c:304-315` stamps them `~0ull<<32`). Report perf's S/D-only blindness as its own M2 result, not as an aggregation effect.
- Knock-on: §2.1, §3.3 (build deps, and `linux-tools`' perf is unusable here), W4/W8/W10, M2's "capture rate vs threshold", §9 → "Confirm build options and threshold units".

---

### 26. The ftrace hist trigger silently moves the whole instance to the `global` clock (major)

**Plan says** (§6.1, lines 291–292):

> ```
> echo 'hist:keys=common_pid:ts0=common_timestamp.usecs' > events/hitch/frame_begin/trigger
> echo 'hist:keys=common_pid:lat=common_timestamp.usecs-$ts0:onmatch(hitch.frame_begin).frame_lat($lat,common_pid)' > events/hitch/frame_end/trigger
> ```

**Why it breaks**

- `common_timestamp` without `clock=` switches the entire trace array to `"global"` (`trace_events_hist.c` v7.0:1591-1592, :6621-6628), and every reserved event pays it via `rb_time_stamp()` — all system-wide `sched_switch`/`sched_waking`, not just two 60 Hz uprobes. `trace_clock_global()` (`trace_clock.c:94-140`) is `raw_local_irq_save` plus `arch_spin_trylock` and a write to one shared cacheline, per event, across 10 vCPUs. It inflates the recorder's CPU% in M3 in hitchtrace's favour — the bias §7's "`sched_waking` system-wide BPF cost exceeds ftrace's write cost" row exists to test.
- Nothing restores it (`hist_unregister_trigger` is not a `tracing_set_clock` caller) and the recipe writes to the top-level instance, so later trace-cmd/gpuvis captures stay on `global`; a clock change also resets both buffers (`trace.c:7045/7050`), so re-arming mid-run wipes the window.
- `global` is not CLOCK_MONOTONIC, so snapshots cannot be joined to `bpf_ktime_get_ns` or MangoHud for M2. `clock=mono` is `ktime_get_mono_fast_ns` (`trace.c:1344`) — the function behind `bpf_ktime_get_ns` — and is cross-CPU coherent.

**Change to**

```sh
mkdir /sys/kernel/tracing/instances/fr      # confine the clock change and the snapshot
cd /sys/kernel/tracing/instances/fr         # uprobe_events + synthetic_events stay at top level
echo 'hist:keys=common_pid:ts0=common_timestamp.usecs:clock=mono' > events/hitch/frame_begin/trigger
echo 'hist:keys=common_pid:lat=common_timestamp.usecs-$ts0:clock=mono:onmatch(hitch.frame_begin).frame_lat($lat,common_pid)' > events/hitch/frame_end/trigger
cat trace_clock >> $MANIFEST                # the run manifest has no clock field today
# was: the same two triggers, no clock=, in the top-level instance
```

- Both triggers need the same `clock=` (a mismatch makes the second override the first and resets the buffers); same `clock=mono` for M2's per-event ftrace comparator; delete the instance to clean up, which trigger removal does not do. In W5, verify `onmatch` and the snapshot trigger fire inside an instance.
- Knock-on: W10's flight-recorder row; §8 manifest gains a trace-clock field.

---

### 27. Snapshot semantics make the M3 drain-time and bytes-per-hitch numbers wrong (major)

**Plan says** (§6.1, lines 293–294, 298):

> ```
> echo 1 > snapshot                                   # allocate the snapshot buffer once
> echo 'snapshot if lat > 16600' > events/synthetic/frame_lat/trigger
> ```
> Drain `snapshot` from user space after each trigger; record drain time and whether a second trigger overwrote an unread snapshot.

**Why it breaks**

- A snapshot is a swap, not a copy, and the new live buffer is never cleared: `update_max_tr()` (`trace.c:1607-1641`) is `swap(tr->array_buffer.buffer, tr->snapshot_buffer.buffer)`. Per-CPU buffers wrap independently, so idle CPUs carry the same pages into snapshot after snapshot — duplicated bytes at *any* hitch rate, straight into bytes-per-hitch. An unlimited trigger can also swap under the reader, which `__tracing_open()` (`trace.c:3940-3986`) binds to one `ring_buffer` at open.
- `cat snapshot` formats every event to text in the kernel via `seq_read`, on the game machine: that is the "drain time" being reported, and it perturbs the run it measures. The binary, splice-capable path is `per_cpu/cpuN/snapshot_raw`.
- The window is undefined (`buffer_size_kb` unset, no `trace_buf_size` → the 1408 KB/CPU default, `trace.c:688`) and untied to hitchtrace's edge-ring window, so M3's volume comparison is not like-for-like. 16600 ≠ the 16667 µs budget, and `echo 1 > snapshot` is redundant and does alloc+*swap*.

**Change to**

```sh
# arm (inside instances/fr, after the hist triggers)
echo <N> > buffer_size_kb     # smallest size whose *minimum* per-CPU coverage >= the L3 window
                              # at the W1 rate, then sweep it: bytes/hitch is window x rate
echo 'snapshot:1 if lat > 16667' > events/synthetic/frame_lat/trigger   # one-shot; same budget
# was: `echo 1 > snapshot` + `echo 'snapshot if lat > 16600' > ...` (drop the echo 1 line)
echo 1 > events/sched/sched_switch/enable; echo 1 > events/sched/sched_waking/enable
echo 1 > tracing_on
# drain loop, replacing "Drain `snapshot` ... after each trigger".  Metadata once per run; never
# `trace-cmd extract -s` per hitch (each file embeds all of kallsyms, 11,190,387 B here).
#   wait   inotify IN_MODIFY on tracing_max_latency (__update_max_tr -> latency_fsnotify();
#          the latency-collector pattern.  inotify coalesces, so it cannot count drops.)
#   read   splice per_cpu/cpu*/snapshot_raw    # binary, consuming; never `cat snapshot`
#   clear  echo 2 > snapshot                   # so stale pages cannot reappear later
#   re-arm echo '!snapshot' > .../trigger, then re-echo the snapshot:1 line
```

- Definitions: bytes/hitch = payload pages only; drain time = trigger → re-armed; "dropped" = `frame_lat` events timestamped between a firing and its re-arm, counted from a second instance (`instances/hitchcount`, filter `lat > 16667`, consume `trace_pipe`; `onmatch` calls every registered probe). Pin the drainer off the game's `AllowedCPUs`; report memory as 2 × `buffer_size_kb` × nCPU.
- Name the event set: `handle_mm_fault` and `dma_fence_wait_timeout` have no tracepoints, so "the same tracepoints hitchtrace uses" is inexpressible. Like-for-like with L3 v1 is `sched_switch` + `sched_waking`; for M1/M2 add `exceptions:page_fault_user`, fprobe `f:` entry/`%return` on those two functions (`CONFIG_FPROBE_EVENTS=y`), `vmscan`/`compaction` begin/end, and futex/poll/nanosleep `raw_syscalls`.
- Knock-on: W10's metrics and M3's "snapshot drain/drop" now have definitions.

---

### 28. systing is marked optional, but it is the comparator §1 promises to answer (major)

**Plan says** (§6.4, lines 306–307; §1, line 41):

> `systing --pid <pid>` for the Linux-desktop Perfetto pipeline; Perfetto `RING_BUFFER` + `STOP_TRACING` trigger fired from the layer if time allows.

> **[R]** Expect: "systing/Perfetto + a missed-frame trigger gives the same result." Answer with M2/M3 numbers, not architecture arguments.

**Why it breaks**

- The plan contradicts itself: §2.1, §6.4 and W4 mark systing optional, W10 (line 256) lists it as an unconditional M3 row, and the only configuration defined is untriggered `systing --pid` — the strawman the [R] item exists to prevent.
- systing already implements the triggered flight recorder with no layer code: `--continuous SECONDS` plus `stop_triggers.thresholds: [{start, end, duration_us}]` on `usdt:` events, attached via libbpf `attach_usdt_with_opts` (`src/systing_core.rs:5163`). The planned layer-fired `RING_BUFFER`/`STOP_TRACING` work is unnecessary. (from web sources, verify)
- It is the sharpest M3 contrast for claim 2: in continuous mode systing streams every event to user space and bounds a *user-space* ring by time (`configure_recorder` → `set_ringbuf_duration`), so its always-on kernel→user movement grows with elapsed time — the opposite of §2.5's "Zero bytes cross to user space for normal frames". ftrace's in-kernel ring is the other design; you want both. (from web sources, verify)

**Change to**

```sh
# replacement §6.4 systing — triggered flight recorder (guaranteed; W4 baseline, W10 M3 row)
cargo install --locked --git https://github.com/josefbacik/systing.git --rev <sha>  # sha in manifest
# cfg.json: events usdt:<layer.so>:hitchtrace:frame_begin / :frame_end
#   stop_triggers.thresholds: [{start: frame_begin, end: frame_end, duration_us: <budget>}]
#   thresholds key on tgidpid, not scope: "process" — both probes must fire on one thread.
#   --continuous cannot be combined with `-- <cmd>` (main.rs:593): attach to a running game.
#   --sw-event on the PMU-less VM, else the default 1000 Hz stacks inflate systing's overhead.
systing --continuous <window_s> --cgroup <game cgroup> --trace-event-config cfg.json \
        --only-recorder sched --only-recorder irq --only-recorder sleep-stacks [--sw-event]
# was: `systing --pid <pid>` (untriggered) + "Perfetto RING_BUFFER + STOP_TRACING trigger fired
#      from the layer if time allows" — delete the latter; systing needs no layer code.
```

- The trigger is one-shot (1 s stop delay, detach, drain, symbolize, write), so wrap it in a relaunch loop and measure blind time from trigger to re-armed recording, reporting hitches missed at 0.01/0.1/1 per s — the systing analogue of §6.1's dropped snapshots. In M3, report its always-on kernel→user event rate and missed-event counters beside bytes/hour and CPU%; that answers the [R] item.
- Knock-on: §1's [R] item, §2.1 (drop "optional"), W4 (systing joins "scripted and validated"), W10, and the W6 per-title checklist gains "`frame_begin` and `frame_end` fire on the same thread".

---

### 29. The uprobe offset instructions and the `perf probe` SDT syntax are wrong (minor)

**Plan says** (§6.1, line 287):

> ```
> # uprobes on the layer's two USDT sites (get offsets via `readelf -n libVkLayer_hitchtrace.so` or `perf probe -x ... %hitchtrace:frame_begin`)
> ```

**Why it breaks**

- `%hitchtrace:frame_begin` is not valid perf-probe syntax: it is `%[sdt_PROVIDER:]SDTEVENT`, i.e. `%sdt_hitchtrace:frame_begin`, after a `buildid-cache` scan. Such events land in group `sdt_hitchtrace`, so `events/hitch/...` and `onmatch(hitch.frame_begin)` do not resolve.
- `readelf -n` prints `Location` as a virtual address, not a file offset; the two coincide only under GNU ld's default layout (a bfd-linked test .so here has text PT_LOAD offset 0x1000, vaddr 0x1000). An lld layout differs and the uprobe lands on the wrong instruction.
- With USDT semaphores, a plain ftrace uprobe never fires unless given as `PATH:OFFSET(REF_CTR_OFFSET)` — parsed by `trace_uprobe.c` v7.0:549-626, undocumented in `uprobetracer.rst`.

**Change to**

```sh
# generate the exact uprobe_events lines (offset and ref_ctr included) instead of computing them
perf buildid-cache --add /usr/lib/…/libVkLayer_hitchtrace.so
perf probe -x /usr/lib/…/libVkLayer_hitchtrace.so -D %sdt_hitchtrace:frame_begin
perf probe -x /usr/lib/…/libVkLayer_hitchtrace.so -D %sdt_hitchtrace:frame_end
# rewrite the group from sdt_hitchtrace/ to hitch/ before echoing it into <instance>/uprobe_events,
# so events/hitch/... and onmatch(hitch.frame_begin) still resolve.
# was: "get offsets via `readelf -n …` or `perf probe -x ... %hitchtrace:frame_begin`"
```

- Alternative: build the layer's USDTs without semaphores and say so in §2.1, so the ftrace baseline stays a one-liner.

---

### 30. The delayacct baseline reads the wrong field and is handicapped by construction (minor)

**Plan says** (§6.3, line 304; Appendix A, line 408):

> a small netlink (TASKSTATS) reader that diffs per-thread `cpu_run_virtual/blkio/swapin/freepages/thrashing/compact/wpcopy/irq` delays at each `frame_begin`

> ```
> getdelays -d -t <TID>
> ```

**Why it breaks**

- `cpu_run_virtual_total` is not a delay: `delayacct_add_tsk()` fills it from `tsk->se.sum_exec_runtime`, i.e. on-CPU runtime. The run-queue wait is `cpu_delay_total` (`sched_info.run_delay`). As specified, the "80% with zero kernel code" comparator reports on-CPU time as a stall; `irq_delay_total` is always 0 here (`# CONFIG_IRQ_TIME_ACCOUNTING is not set`).
- `getdelays` is not packaged, and v7.0 `tools/accounting/getdelays.c` fails against the installed `taskstats.h` (v14, linux-libc-dev 6.8): `'struct taskstats' has no member named 'cpu_delay_max'`. It builds against the v17 header under `/usr/src/linux-hwe-7.0-headers-7.0.0-31/include/uapi`; unprivileged it fails, `TASKSTATS_CMD_GET` being `GENL_ADMIN_PERM`.
- Per-thread polling at every boundary is costly and skewed: ~100 Proton threads × 60 fps ≈ 6k netlink round trips/s (~1–3% of a core), walked serially, smearing one "snapshot" over hundreds of µs around a USDT-marked boundary.
- Enable order is a silent-zero trap: `delayacct_tsk_init()` allocates `tsk->delays` only `if (delayacct_on)` at fork, so threads older than the sysctl report 0 — exactly what A5's `-d -t <TID>` on a running thread does.

**Change to**

```sh
# replacement §6.3 delayacct poll
sysctl kernel.task_delayacct=1   # BEFORE launching Steam/pressure-vessel/wineserver/the game;
                                 # 0 for every other M3 arm; record the value in each manifest.
# one TASKSTATS_CMD_ATTR_TGID query per frame boundary, diffing summed *_delay_total:
#   cpu_delay_total  blkio_delay_total  swapin_delay_total  freepages_delay_total
#   thrashing_delay_total  compact_delay_total  wpcopy_delay_total  (irq_delay_total is 0 here)
# was: per-thread diffs of `cpu_run_virtual/blkio/…` — cpu_run_virtual_total is on-CPU runtime,
#      not a delay; the run-queue wait is cpu_delay_total.
# per-thread PID queries only for a short fixed list (present-calling thread + named workers);
# record USDT-signal -> reply latency as this baseline's skew; pin the poller off AllowedCPUs.
# build: gcc -I/usr/src/linux-hwe-7.0-headers-7.0.0-31/include/uapi tools/accounting/getdelays.c
#        (a reader restricted to *_delay_total also compiles against the stock v14 header)
```

- A5 becomes `getdelays -d -c <cmd>`; field 2 of `/proc/<tid>/schedstat` is a privilege-free run-queue check even with `task_delayacct=0`. Do not promote v17's `*_delay_max` to an M2 comparator: it is a lifetime high-water mark that is never reset.
- Score information content separately from poller cost: read `task->delays` and `sched_info` from BPF at `frame_end` (frame-synchronous, no skew) and report the netlink poller's CPU cost as its own number — otherwise M3 measures your polling loop.
- Knock-on: §3.3 (sysctl moves to per-run, with launch ordering), W1 item 5, W10 (delayacct off in all other arms), Appendix A A5.

---

### 31. The §3.3 package line aborts `apt` on day one (minor)

**Plan says** (§3.3, line 192):

> - Packages: `clang llvm libbpf-dev bpftool bpftrace linux-tools-$(uname -r) pahole trace-cmd gpuvis mangohud vulkan-tools vulkan-validationlayers libvulkan-dev systemtap-sdt-dev (sys/sdt.h) cmake ninja python3-duckdb`.

**Why it breaks**

- `apt-get install -s` aborts the whole transaction on four names: `bpftool` has no installation candidate (virtual), and `gpuvis`, `ninja` (the package is `ninja-build`) and `python3-duckdb` cannot be located. `provision.sh` fails at the first step of W0.
- `bpftool` comes from `linux-tools-common` (v7.7.0, "using libbpf v1.7"); noble's `libbpf-dev` is 1.3, older than the libbpf 1.7 chaingraph already vendors and links statically, so linking `bpf/` against the distro one is a downgrade.
- Missing for commitments the plan already makes: `stress-ng` (Appendix B, W4), `glslang-tools` (hitchbench shaders), `libelf-dev` + `zlib1g-dev` (static libbpf link), `xserver-xorg-video-dummy` (§3.1), `dmsetup` (Appendix B). `gamescope` has no candidate in noble although §3.2 names it.

**Change to**

```
- Packages: `clang llvm bpftrace linux-tools-$(uname -r) linux-tools-common pahole trace-cmd
  mangohud vulkan-tools vulkan-validationlayers libvulkan-dev systemtap-sdt-dev (sys/sdt.h)
  cmake ninja-build stress-ng glslang-tools libelf-dev zlib1g-dev xserver-xorg-video-dummy dmsetup`
  (+ perf-with-skeletons build deps: libssl-dev bison flex libtraceevent-dev libdw-dev libzstd-dev;
  libssl-dev is load-bearing — without it Makefile.config:711 disables skeletons with a warning)
  Removed: bpftool (virtual; from linux-tools-common), gpuvis (not in noble — build from source
  or drop), python3-duckdb (not in noble — pip in a venv), ninja (-> ninja-build), libbpf-dev.
- `bpf/` reuses chaingraph's Makefile: vendored libbpf 1.7 built static, `bpftool gen skeleton`,
  vmlinux.h from /sys/kernel/btf/vmlinux — not libbpf-dev.
- Drop the kisak-mesa PPA unless a named RADV feature needs it (noble-updates has Mesa 25.2.8);
  pin and record the Mesa version in the manifest instead.
- Hetzner: install linux-generic-hwe-24.04 explicitly so both machines run the same 7.0 line.
```

- Knock-on: W0's "toolchain installed" checkpoint becomes `apt-get install -s` passing plus the `bpf_skeletons: [ on ]` gate from issue 25; §3.2 (pick Sway headless — gamescope is not installable from noble).

## F. Evaluation environment, schedule and claims

The eval box, the Deck-class emulation, the 12-week plan, and what §1 claims.

### 32. The Deck-class cgroup recipe never reaches the game (major)

**Plan says** (§2.2 line 91; App. B lines 415–416):

> Launch the game with `systemd-run --scope` so wineserver, pressure-vessel helpers and the game land in one cgroup.

> ```bash
> systemd-run --scope --unit=game -p MemoryMax=12G -p AllowedCPUs=0-3 \
>   -p IOReadBandwidthMax="/dev/nvme1n1 90M" -- steam -applaunch <APPID>
> ```

**Why it breaks**

- With a client already running — the normal case, and mandatory for M0 condition 3's background download — `steam -applaunch` hands the command line to the running client over `~/.steam/steam.pipe` and exits. The game is forked by the client's reaper in the *client's* cgroup. No error. (from web sources, verify)
- No `--user`, so this talks to the system manager: as root `steam.sh` refuses ("Cannot run as root user"); as the `steam` user, `org.freedesktop.systemd1.manage-units` is `auth_admin` and prompts. (from web sources, verify)
- `--user` silently drops two of the three limits. Measured here: `user@1000.service` has `DelegateControllers=cpu memory pids`; a test `systemd-run --user --scope -p MemoryMax=512M -p AllowedCPUs=0-1 -p IOReadBandwidthMax="/dev/sda 1M"` exited 0, applied `memory.max`, created no `io.max` and no `cpuset.cpus.effective`, left `Cpus_allowed_list` at `0-9`, and logged nothing.
- `--unit=game` is fixed, so repeat runs fail ("Unit … was already loaded", exit 1); §5 requires ≥ 5 runs.
- §8 records *requested* limits, so M0 conditions 2–3 and M3's "same cgroup constraints" pass review while running unconstrained or half-constrained.

**Change to**

- Constrain the whole `steam` user slice from root through the system manager: it enables `cpuset` and `io` itself, catches an already-running client, the IPC-launched game, wineserver and pressure-vessel, and matches a Deck, where the client shares the same cores and RAM.

```diff
-systemd-run --scope --unit=game -p MemoryMax=12G -p AllowedCPUs=0-3 \
-  -p IOReadBandwidthMax="/dev/nvme1n1 90M" -- steam -applaunch <APPID>
-stress-ng --cpu 2 --cpu-load 60 --timeout 45m &
+# as root, before the steam user logs in / launches anything
+systemctl set-property --runtime user-$(id -u steam).slice \
+    MemoryMax=12G AllowedCPUs=0-3,8-11 \
+    IOReadBandwidthMax="/dev/mapper/slowdisk 90M"
+# unconstrained condition: MemoryMax=infinity AllowedCPUs= IOReadBandwidthMax=
+# background load INSIDE the slice, pinned:
+systemd-run --uid=steam --slice=user-$(id -u steam).slice --scope \
+    taskset -c 2,3 stress-ng --cpu 2 --cpu-load 60 --timeout 45m &
```

- Use that slice's cgroup id as §2.2's `bpf_task_under_cgroup` filter (see issue 39 for the program-type problem).
- Verify *effective* limits per run and abort on mismatch: for the game tgid, wineserver and the pressure-vessel helpers record `/proc/<pid>/cgroup` and `Cpus_allowed_list`; from the cgroup record `cpuset.cpus.effective`, `memory.max`, `io.max`, plus `memory.events` and `io.stat` to show the limits bit.
- Define game-initiated vs external by tgid + fork descendants + the prefix's wineserver, not by cgroup membership — §2.8 already treats steamwebhelper as an external preemptor while this recipe puts it inside the cap.
- Do not move processes after start: it races the reaper's fork, a non-root user cannot write `cgroup.procs` of the common ancestor (cgroup-v2 Delegation Containment), and already-charged memory stays with the old cgroup.
- Knock-on: §2.2, §3.2, App. B, §8 manifest fields, §4 W7 (M0 conditions 2–3) and W10 (M3), §1 claim 4.

---

### 33. The Deck emulation is not faithful: CPU count, SMT layout and GPU memory escape the cgroup (major)

**Plan says** (§3.2 line 186; §1 line 31):

> `systemd-run --scope -p MemoryMax=12G -p AllowedCPUs=0-3 …`

> 4. A prevalence number for Linux games on Deck-class constraints: what fraction of over-budget frames have their excess dominated by kernel-observable stalls, by cause. (M0) — nobody has published this.

**Why it breaks**

- A cpuset does not change the CPU count the game *sees*. Measured here: `taskset -c 0-1 getconf _NPROCESSORS_ONLN` prints 10 while `nproc` prints 2. Wine sets `peb->NumberOfProcessors` from that call and DXVK sizes its pipeline worker pool from `GetSystemInfo().dwNumberOfProcessors`, so on the AX42 the game spawns ~16 workers onto 4 CPUs — inflating exactly the `preempt` and `runqueue` buckets M0 reports. (from web sources for the Wine/DXVK call sites, verify)
- `AllowedCPUs=0-3` is topology-dependent: on an 8C/16T Ryzen siblings are N and N+8, so 0-3 is 4 cores with SMT *off*, not a Deck's 4C/8T. This VM pairs adjacent CPUs (`thread_siblings_list` = `0-1`), so the range cannot be copied between machines.
- `MemoryMax` does not cap APU graphics memory: in v7.0 `drivers/gpu/drm/ttm/ttm_pool.c` GTT pages come from `alloc_pages_node()` with no `__GFP_ACCOUNT` and no memcg in the file (the only cgroup hook in TTM is dmem in `ttm_bo.c`, for VRAM eviction). The TTM pages limit defaults to 50% of RAM, so GPU allocations draw ~32 GB outside the 12 G cap. That is not a 16 GB unified pool.

**Change to**

```bash
# 1. 4C/8T for everything (root, no reboot): keep 4 cores + their siblings
lscpu -e=CPU,CORE   # confirm the sibling map on the AX42 first
for c in 4 5 6 7 12 13 14 15; do echo 0 > /sys/devices/system/cpu/cpu$c/online; done
# -> sysconf(_SC_NPROCESSORS_ONLN) == 8 for native games, Wine, DXVK, vkd3d, Steam
# 2. 16 GB unified pool: boot mem=16G, or reserve ~48 GB via vm.nr_hugepages to
#    switch per condition without a reboot. MemoryMax stays as an extra per-game
#    cap. Do NOT use amdgpu.gttsize (deprecated in v7.0: amdgpu_ttm.c warns
#    "please use ttm.pages_limit").
# 3. clocks: scaling_max_freq ~3.5 GHz, EPP pinned, amdgpu
#    power_dpm_force_performance_level recorded per run.
```

- Manifest: record the CPU count the game actually observes, `/sys/devices/system/cpu/online`, the BIOS VRAM carve-out, swap config, and Wine's "overriding CPU configuration" line.
- Thesis wording: the model reproduces thread count, SMT, the shared memory pool, storage latency and co-located background load; it does not reproduce Zen 2 IPC or RDNA 2 throughput, and the GPU is unconstrained.
- Knock-on: §1 claim 4 (rename "Deck-class" to "Deck-like emulated constraints" wherever it describes a result), §3.2, App. B, §5 M0 row, §8 manifest, §4 W7.

---

### 34. Steam-side confounders and version drift are uncontrolled (major)

**Plan says** (§5 line 279; §8 line 347):

> Controls for all: same binary/build of each game (record build id; game updates invalidate data), same scene/benchmark loop, ≥ 5 runs, report distributions (p50/p99/1% low), cgroup constraints in the manifest, streaming disconnected, shader cache state recorded.

> - **Run manifest** (`results/<run>/manifest.json`): kernel, distro, Mesa, GPU, tool level, budget/predicate, cgroup limits, game + build id, scene, shader-cache state, streaming on/off, hitch injector config.

**Why it breaks**

- The manifest records Mesa and the game build but not Proton, DXVK, vkd3d-proton, the Steam Linux Runtime, the client, or the sync backend — and Steam updates all of them by itself. Proton shipped 11.0-1, 11.0-1b and 11.0-2 inside one quarter; W7/W8/W10 span about four weeks. (from web sources, verify)
- The sync backend decides which hook sees a wait: ntsync waits are `NTSYNC_IOC_WAIT_ANY`/`_ALL` ioctls, invisible to §2.3's futex hooks, while fsync uses `futex_waitv` (nr 449). On this box `CONFIG_NTSYNC=m`, the module is unloaded and `/dev/ntsync` is absent — one `modprobe` or one Steam update silently moves time from `futex` to `other_block` between runs of the same condition.
- "Cold shader cache" collides with Steam's own work: `fossilize_replay` runs at client start across all cores, and with pre-caching on Steam loads its Fossilize layer *inside* the game process, charging that work to the game. (from web sources, verify)
- With 64 GB of RAM, dropping caches only for the "cold" condition means run 1 of every condition reads from disk and runs 2–5 read from page cache, skewing `iowait` and `pagefault_major` across the ≥ 5 repeats.
- `PROTON_LOG=1` is not a free way to capture versions on a measured run: `proton` then sets `WINEDEBUG=+timestamp,+pid,+tid,+seh,+unwind,+threadname,+debugstr,+loaddll,+mscoree`.

**Change to**

```diff
 - **Run manifest**: kernel, distro, Mesa, GPU, tool level, budget/predicate,
-  cgroup limits, game + build id, scene, shader-cache state, streaming on/off,
-  hitch injector config.
+  EFFECTIVE cgroup limits (issue 32), game + appmanifest_<appid>.acf buildid,
+  scene, shader-cache state, streaming on/off, hitch injector config,
+  Steam client version, Proton `version` file, DXVK + vkd3d-proton versions
+  (from DLL version strings, or one unmeasured PROTON_LOG=1 calibration launch
+  — never on a measured run), SteamLinuxRuntime_sniper VERSIONS.txt,
+  Steam shader-precaching settings, sync backend as printed on the game's
+  stderr ("ntsync: up and running." vs the fsync/server-sync line).
```

- Pin the sync backend rather than recording it, to whichever issue 17 settles on — its recommendation is ntsync (`/etc/modules-load.d/ntsync.conf` on both machines), to match SteamOS ≥ 3.7.20 + Proton 11. Set the opposing variable explicitly in every launch script (`PROTON_NO_NTSYNC=1`, or nothing) so the default cannot drift, and hook the bucket for the backend that is pinned: issue 16 step 2's ioctl cmd for ntsync, `futex_waitv` (nr 449) for fsync. One decision, recorded in §8 — this issue and issue 17 must not pin it in opposite directions.
- `apt-mark hold` the kisak-mesa packages (a Mesa bump also makes every shader cache cold), install one Proton build in `compatibilitytools.d` and force it per title, and snapshot `VERSIONS.txt` before and after each block, discarding any block where it changed.
- Turn Shader Pre-Caching off (state that this is not the Deck default) or gate each run on no running `fossilize_replay`. Drop the page cache before *every* run of *every* condition, and randomize run order.
- Replace condition 3's "background Steam download" with a reproducible load outside the client — a fixed `steamcmd` depot download or a rate-limited copy plus zstd decompression onto the same disk, rate recorded.
- Exclude Denuvo titles (changing Proton version or CPU topology counts as a new system against the 5-per-24 h limit) and never change either mid-study. (from web sources, verify)
- Knock-on: §5 controls, §8 manifest, §2.3 `futex` row, §4 W7 and W10.

---

### 35. The 12-week schedule does not fit, and the riskiest dependency is first touched in W6 (major)

**Plan says** (§3.1 line 172; §4 lines 234, 240, 266):

> ### 3.1 Development: Hyper-V VM (weeks 0–5)

> ### W6 (Oct 27–Nov 2) — Hetzner bring-up + real-game smoke

> - 5 titles × 3 conditions (unconstrained; Deck-class cgroups; Deck-class + background Steam download + cold shader cache), ≥ 45 min each, built-in benchmark loops where available.

> - Figures frozen by Dec 10.

**Why it breaks**

- W2–W6 is ~25 working days for the layer, ~12 hook families, the daemon, L1, hitchbench with 8 injectors, 3–4 baselines and full Hetzner bring-up including streaming, Steam and 5 titles — 35–40 person-days. No week schedules run automation: `grep -iE 'automat|runner|unattended'` over the plan finds nothing.
- M0 is 5 × 3 × 45 min = 11.25 h per repetition; §5's ≥ 5 runs makes ~56 h of wall clock inside W7, with no unattended runner in existence.
- Everything genuinely risky is deferred to W6: iGPU and virtual display, whether a host implicit layer loads inside pressure-vessel, real-game wakeup rates, `gpu_wait`. W7 (M0, guaranteed scope) starts a week later.
- Two W4 deliverables cannot be produced on the VM. `perf version --build-options` here prints `bpf_skeletons: [ OFF ]`, so perf 7.0.14 ignores `--off-cpu` — it must be rebuilt with `BUILD_BPF_SKEL=1`, which no week covers; `getdelays` is not packaged. `gpu_heavy`/`gpu_wait` cannot be exercised at all: lavapipe creates no kernel dma_fences and `/dev/dri` holds only `card1` (hyperv_drm) with no render node, so W4's "each injector reproduces its class" DoD is unreachable before W6. `apt-cache policy` also shows no candidate for `gamescope`, `gpuvis` or `python3-duckdb` from §3.3.
- Hourly billing makes the deferral pointless: a 1–2 day AX42 probe is ~€55, holding the box from W0 ~€340 with a single setup fee. (from web sources, verify)

**Change to**

```diff
-W0  VM provisioned, headless, vkcube, BTF, repo skeleton, walker CI
-W1  5 bpftrace studies + walker library + related work
-W2-3 layer + L0 core        W4 harness + baselines     W5 L1
-W6  Hetzner bring-up        W7 M0   W8 M2   W9 L3+M1   W10 M3
-W11 stretch/buffer          W12 write-up (figures frozen Dec 10)
+W0-W1  Rent the AX42 NOW. Go/no-go on day one: /dev/dri/renderD*, RADV on the
+       780M, amdgpu.virtual_display + headless compositor, Steam + one Proton
+       title with MangoHud, bpftrace A3 on that title, and a `chaingraph -K -p
+       <tgid>` pilot (a census of causes/chains + the real wakeup rate — NOT an
+       early M0: it has no frame boundaries).
+       Fork chaingraph as the base: Makefile, vendored libbpf 1.7 (USDT +
+       ringbuf), skeleton, irq-exit attach order, syms.c, run_tests.sh /
+       check_chain.py pattern. Daemon in C (issue 43); drop the C++ daemon and
+       the separate C++ walker. CPU-only hitchbench firing the same USDTs
+       (sync_read, holder_preempt, thousand_cuts); Vulkan layer minimal on vkcube.
+W2-W3  BPF L0 + L1 on task storage (issue 24).
+W4     Baselines: ftrace recorder; perf REBUILT from source with
+       BUILD_BPF_SKEL=1 (Ubuntu's perf ignores --off-cpu); delayacct from
+       tools/accounting/getdelays.c; remaining CPU-side injectors.
+W5     Full bring-up + an UNATTENDED RUNNER as a deliverable: benchmark loop,
+       shader-cache wipes, throttled background load, manifest + MangoHud logs.
+       Validate chains on the real box.
+W6 M0   W7 M2   W8 M3   W9 M1 + buffer   W10 buffer/stretch   W11-W12 write-up
```

Every other issue in this review cites the **old** week numbers. Re-key them by role, not by number: *layer + L0* = the two BPF weeks; *baselines* = the baselines week; *Hetzner bring-up* = the bring-up week (old W6, new W5); *M0* = the prevalence week (old W7, new W6); likewise *M2*, *M3*, *M1*. Apply the renumbering in a single pass over the whole review, or keep the old numbering and cut scope inside it — a half-applied renumbering puts `gpu_wait` validation and M0 in the same week, which is the thing this issue is trying to prevent.

- Cuts, in this order: Perfetto/DuckDB export; median mode (use a per-title fixed budget from a pilot median, or cap FPS with MangoHud — an offline median cannot gate emission); M0 to 3 titles (2 Proton, 1 native) × 3 conditions × 3 runs × 30 min ≈ 13.5 h unattended; the `compaction` and `gpu_heavy` injectors; `--user-stacks`; file-path attribution; the in-kernel walk; the blind-diagnosis study. **Not systing**: issue 28 shows it is the comparator §1's [R] item promises to answer and costs about half a day. If the schedule genuinely cannot hold it, §1 line 41 has to be reworded in the same commit.
- The pressure-vessel layer check can still run on the VM against the standalone `SteamLinuxRuntime_sniper` tarball (`./run -- vkcube`); note that Ubuntu 24.04 sets `kernel.apparmor_restrict_unprivileged_userns=1` and the shipped `steam` profile covers only `/usr/games/steam`, so a standalone run needs its own profile. (from web sources, verify)
- Knock-on: all of §4; §3.1's "weeks 0–5" and §3.2's "weeks 6–11"; §2.1 (`daemon/`, `walker/`); §3.3 package list; §5 M0 workload row; §6.2 (perf must be rebuilt, and `--off-cpu-thresh` takes milliseconds); §10 item 4.

---

### 36. Novelty claims overreach, and direct prior art for online chains is missing (major)

**Plan says** (§1 lines 31, 36; §9 line 362):

> Ours is the *online, bounded, frame-gated execution*, not the algorithm.

> 4. A prevalence number for Linux games on Deck-class constraints: what fraction of over-budget frames have their excess dominated by kernel-observable stalls, by cause. (M0) — nobody has published this.

> | Gregg: off-CPU analysis; off-wake; chain-graph prototype (2016, "outliers… yet") | Verified | Cite the "yet" |

**Why it breaks**

- Gregg's "yet" is a parenthetical under "Future": latency outliers are named as one problem chain graphs do not address, because they sum time over a whole run. That motivates frame-gating; it is not an open problem the thesis "addresses", and §9's "Cite the 'yet'" invites an examiner to look it up. (from web sources, verify)
- "Ours is the online, bounded, frame-gated execution" is contradicted three ways, one by your own repo: Gregg's 2016 prototype was already in-kernel with two waker levels; `src/chaingraph.bpf.c` `record_wakeup()` propagates 8 levels online into task storage with interrupt/idle terminals; and latency-tracker's rt tracker (`efficios/latency-tracker`, `trackers/rt.c`, Desfossez 2015) carries a tracked event online from irq/softirq/hrtimer through `sched_waking` → switch-in → the next thread's waking, with per-step timing, a threshold gate, and a user-written `work_done` deadline marker. §1 cites latency-tracker only for selective capture — it is the closest precedent for deadline-scoped chains. (from web sources for latency-tracker, verify)
- The plan's own L3 is not online either: v1 walks a dumped window in user space and v2 is a post-hoc `bpf_loop` at `frame_end`.
- Claim 4 is an unhedged universal negative over 3 Proton + 2 native titles on one 8700GE/780M box with cgroup-emulated limits and an unconstrained GPU.

**Change to**

```markdown
3. Frame-gated, per-hitch emission of cross-thread blocking chains and
   per-cause stall attribution for over-budget frames of unmodified games,
   with measured always-on cost and output volume. (M1, L3)

4. To our knowledge (sources searched, <date>), no published measurement
   exists of what fraction of over-budget frames in Linux games have their
   excess dominated by kernel-observable stalls, by cause — measured here on
   N titles (list) on one Ryzen 7 PRO 8700GE / 780M system under cgroup- and
   topology-emulated Deck-like CPU/memory/storage limits (GPU unconstrained).

Conceded: online, bounded chain propagation in the kernel.
  - Gregg 2016 bcc prototype: in-kernel, two waker levels.
  - chaingraph (own prior tool): sched_waking copies the waker's links[] into
    the wakee's task storage, up to 8 levels, terminating at
    hardirq/softirq/idle.
  - latency-tracker rt tracker (Desfossez 2015): online from IRQ/softirq/
    hrtimer through waking and switch-in to the next thread's waking, ending
    at a user-written work_done marker, threshold-gated. Closest precedent
    for deadline-scoped chains.
Motivation, not an open problem: Gregg notes chain graphs sum time over a run,
so outliers vanish ("latency outliers is one example of a performance issue
not addressed by these...yet"). Frame-gating is one response.
```

- Add to §9: chaingraph, Gregg's 2016 prototype and latency-tracker rt with the overlaps above; `scx_lavd` (`scheds/rust/scx_lavd/src/bpf/lat_cri.bpf.c`) as *adjacent* only — it passes a scalar latency-criticality score between waker and wakee online for scheduling, records no chain; and Windows WPA "CPU Usage (Precise)" ready-thread wait analysis plus UIforETW circular-buffer capture, the standard offline per-frame stutter workflow and a flight-recorder precedent. (from web sources, verify)
- Knock-on: §1 claims 3 and 4 and the concession list; §0 if issue 24 is taken; §9 rows for Gregg and latency-tracker; §3.2/§5/App. B wherever "Deck-class" labels a result.

---

### 37. dm-delay at Deck-class latencies adds a 1 kHz kthread, and the day-one checklist has wrong blockers (minor)

**Plan says** (§3.2 line 183; App. B line 418; §4 line 252):

> `dmsetup create slowdisk --table "0 $(blockdev --getsz /dev/nvme1n1p3) delay /dev/nvme1n1p3 0 30"`

> remove `nomodeset` from kernel cmdline (Hetzner images blacklist GPU drivers by default)

> - DoD: chains for `sync_read` (Render ← Streaming ← kworker ← IRQ) and `holder_preempt` (Render ← Worker ← preempted-by) recovered on hitchbench; truncation-rate-vs-K plot.

**Why it breaks**

- In v7.0 `drivers/md/dm-delay.c:275-284`, a maximum delay under 50 ms switches dm-delay into kthread mode: `kthread_run(flush_worker_fn, …, "dm-delay-flush-worker")` with `worker_sleep_us = 1000`, and lines 130–147 loop `fsleep(1000)` while `delayed_bios` is non-empty. Every defensible Deck-class latency is under 50 ms, so every run adds ~1 kHz of kthread wakeups outside the game's cpuset against a ~5k/s baseline — straight into `preempt`, `runqueue` and the `sched_waking` cost.
- That also changes the answer the W9 DoD expects: the chain terminates at a timer via the flush worker, not at an NVMe completion. The `kworker` hop is wrong independently — buffered-read completion wakes the reader from the interrupt path.
- The 3-argument table applies the same delay to writes and flushes, and `steamapps/shadercache` and Steam downloads live on that disk. 30 ms is far above Deck storage latency.
- `installimage` defaults to software RAID1 across both NVMe drives (`DEFAULTSWRAID=1`), so `nvme1n1p3` is an md member dm cannot claim, and `IOReadBandwidthMax` on `/dev/nvme1n1` would throttle only the reads md routes to that mirror. (from web sources, verify)
- Hetzner's `blacklist-hetzner.conf` is i915-specific; for amdgpu the blocker is `nomodeset` in `/etc/default/grub.d/hetzner.cfg`. `amdgpu.virtual_display` only helps a DRM-backend compositor — Sway with `WLR_BACKEND=headless` uses no DRM connectors, so pick one. (from web sources, verify)

**Change to**

```diff
-dmsetup create slowdisk --table "0 $(blockdev --getsz /dev/nvme1n1p3) delay /dev/nvme1n1p3 0 30"
+# installimage: SWRAID 0, or give the game library its own non-RAID partition.
+# 6-arg form: read delay only, writes/flushes untouched (shader cache, downloads).
+dmsetup create slowdisk --table \
+  "0 $(blockdev --getsz /dev/nvme1n1p3) delay /dev/nvme1n1p3 0 <READ_MS> /dev/nvme1n1p3 0 0"
+# kthread mode is unavoidable below 50 ms: pin it and declare it.
+taskset -cp <non-game CPUs> $(pgrep -f dm-delay-flush-worker)
+# throttle the device the FS is mounted from:
+#   IOReadBandwidthMax="/dev/mapper/slowdisk 90M"
```

This issue owns the W9 `sync_read` DoD, which four issues rewrote differently:

```
W9 DoD, `sync_read` chain — assert the terminal *type* per configuration, never a fixed comm:
  dev VM, no dm-delay       Render <- Streaming [iowait] <- [softirq] (vmbus tasklet) or ksoftirqd
  Hetzner, no dm-delay      Render <- Streaming [iowait] <- [hardirq] (NVMe MSI-X), or an
                            IPI / BLOCK_SOFTIRQ remote completion
  either, dm-delay in path  Render <- Streaming [iowait] <- dm-delay-flush-worker <- [hardirq] (timer)
  A kworker hop appears only with fscrypt/fs-verity or btrfs checksum workers.
  Produce the expectation first with `chaingraph -p <hitchbench pid> -K -d 6`, then assert it.
```

- Pick a `<READ_MS>` you can defend as Deck-class (NVMe/eMMC-class, not 30 ms) and label the flush worker as an emulation artifact in the analysis.
- Day-one checklist edits: install `linux-generic-hwe-24.04` first (it decides whether ntsync exists at all); check `blacklist-hetzner.conf` for amdgpu/radeon lines *and* remove `nomodeset` from `/etc/default/grub.d/hetzner.cfg`; choose either `amdgpu.virtual_display` with a DRM-backend compositor or a pure headless backend, not both; budget the one-off setup fee, which is charged even if you cancel on day one.
- Knock-on: §4 W9 DoD and App. C's `sync_read` counterpart; §3.2 checklist; App. B.

---

### 38. Depth-8 kernel stacks are mostly tracing and scheduler frames, and stack ids go stale (minor)

**Plan says** (§2.3 line 95; §2.8 line 160; App. A line 405):

> Kernel stack id is captured at switch-out (depth ≤ 8, kernel only) for offline refinement.

> "largest":{"cause":"futex","ns":7000000,"kstack":["futex_wait_queue","futex_wait","do_futex","__x64_sys_futex"]},

> bpftrace -e 'tracepoint:sched:sched_switch /curtask->tgid == $1 && args->prev_state != 0/ { @reason[kstack(3)] = count(); }' <GAME_TGID>

**Why it breaks**

- `bpf_get_stackid` from a tp_btf program starts inside the BPF machinery (`kernel/trace/bpf_trace.c:1625-1639`, via `perf_fetch_caller_regs`), so the leaf frames are `bpf_prog_*`, `bpf_trace_run*`, `__bpf_trace_*`/`__traceiter_*`, then `__schedule`, `schedule`. Your own `is_tracing_frame()` (`src/chaingraph.c:408`) exists to strip exactly those.
- Depth 8 therefore leaves 2–4 informative frames, and A4's `kstack(3)` never reaches past `schedule`/`__schedule` — W1 item 4 would produce no blocked reasons at all.
- The §2.8 example is not this kernel: `futex_wait_queue` does not exist in kallsyms on 7.0, and a real stack from this VM (`tests/out/t4_kernel_stacks_human.txt`) is `…do_futex;futex_wait;__futex_wait;futex_do_wait;schedule;__schedule`.
- Ids go stale over a 45-minute run: a 6-second test already reports `kstack-collide=2`, `ustack-collide=19` (`tests/out/t1_full_chain.stderr`), and chaingraph's symbolize-at-exit pattern prints `[unknown]` for processes that already exited.

**Change to**

```diff
-Kernel stack id is captured at switch-out (depth ≤ 8, kernel only)
+Kernel stack id is captured at switch-out, kernel only, depth 12–16, with a
+skip count in flags (BPF_F_SKIP_FIELD_MASK) calibrated once at load time by
+locating __schedule in a probe stack; reuse chaingraph's is_tracing_frame()
+strip list in the daemon. Track kstack-collide / err counters as chaingraph
+does and size the stack map from them. For the top-N stalls, copy raw frames
+into the record with bpf_get_stack so records are self-contained, and
+symbolize per record while the process is alive (not at exit).
```

```diff
-"kstack":["futex_wait_queue","futex_wait","do_futex","__x64_sys_futex"]
+"kstack":["futex_do_wait","__futex_wait","futex_wait","do_futex","__x64_sys_futex"]
```

- Replace A4 and the blocked half of A2 with the existing tool: `sudo ./build/chaingraph -K -P -p <tgid> -d 2 10` already gives stripped blocked stacks per thread, sleep totals and counts, and the waker per sleep. A retained bpftrace script needs `kstack(12)` plus post-processing.
- Knock-on: §4 W1 item 4 (A4 as written yields nothing); this issue replaces the whole §2.8 kstack array, so issue 17's one-word `futex_wait_queue` → `futex_do_wait` swap must not also be applied.

---

### 39. `bpf_task_under_cgroup` is not callable from the USDT uprobes or syscall tracepoints (minor)

**Plan says** (§2.2 line 91):

> plus an optional **cgroup filter** (`bpf_task_under_cgroup`, kernel ≥ 6.5)

**Why it breaks**

- On 7.0, `bpf_task_under_cgroup` sits in `generic_btf_ids` (`kernel/bpf/helpers.c:4564`), registered only for `BPF_PROG_TYPE_TRACING`, `SCHED_CLS`, `XDP`, `STRUCT_OPS`, `SYSCALL` and `CGROUP_SKB` (`helpers.c:4711-4716`). `tp_btf` and `fentry` qualify; the USDT probes (KPROBE) and `tracepoint/syscalls/*` do not — and `syscalls:*` cannot be `tp_btf`. The filter is unavailable in exactly the programs §2.2 relies on to scope frames and syscall-flag buckets.
- Secondary: `sched_process_fork` fires for thread clones too (`kernel/fork.c:2663`), but the tgid match already covers threads, so the fork hook only matters for child processes — and wineserver is not a child of the game, which is what issue 32's slice filter is for.

**Change to**

```c
/* uprobe / tracepoint programs: */
bpf_current_task_under_cgroup(&cgrp_array, 0);   /* CGROUP_ARRAY */
/* or bpf_get_current_ancestor_cgroup_id(level) — both are in
   bpf_base_func_proto (kernel/bpf/helpers.c:2162-2163). */

/* Better: evaluate once per task in a TRACING program and cache the verdict
   in task storage, then read the bit from the uprobe. Take the tgid from
   bpf_get_current_pid_tgid() in the USDT handler, not from a USDT argument. */
```

- Add a W0 check that reads `/proc/<game>/cgroup` and confirms the scope actually holds the game — the cheap early detector for issue 32.

---

### 40. The §9 feature list is incomplete, the PMU statement is wrong, and the dev VM violates §3.1 (minor)

**Plan says** (§9 line 370; §2.3 line 113; §3.1 line 174):

> | BPF feature versions: 1M insns 5.2; bounded loops 5.3; ringbuf 5.8; `bpf_get_task_stack` 5.9; `bpf_for_each_map_elem` 5.13; `bpf_loop` 5.17; `bpf_find_vma` 5.17; open-coded iterators 6.4; `bpf_task_under_cgroup` 6.5; arena 6.9; private stack 6.13 | Believed correct | Verify each against kernel changelogs before citing |

> per-frame APERF/MPERF or cpufreq deltas (Hetzner only — Hyper-V has no PMU)

> - Gen 2 VM, Secure Boot off (or MS UEFI CA template), **fixed** memory 8 GB (dynamic memory off — ballooning interferes with reclaim tests), ≥ 4 vCPUs.

**Why it breaks**

- Features the design depends on are missing: `bpf_task_pt_regs` 5.15; `bpf_map_lookup_percpu_elem` 5.19; `bpf_ringbuf_reserve_dynptr` 5.19 (needed as soon as a record is variable-size); BTF typed ksyms and task storage in tracing programs 5.11; the open-coded task iterator 6.7; `BPF_F_RB_OVERWRITE` 6.19 if used.
- The PMU claim is inaccurate as stated. This guest has no core `cpu` PMU but does register `msr`: `ls /sys/bus/event_source/devices` → `breakpoint kprobe msr power software tracepoint uprobe`, with `msr` events `aperf mperf pperf smi tsc`, and `aperfmperf` in `/proc/cpuinfo` on all 10 CPUs. BPF has no rdmsr helper — these read through a `PERF_EVENT_ARRAY` with `bpf_perf_event_read_value`, and the CPU-wide `msr` PMU needs CAP_PERFMON or `perf_event_paranoid ≤ 0` (it is 4 here).
- The dev VM does not meet its own spec: `hv_balloon` is loaded and `free -g` shows 7 GB, not a fixed 8 GB. The reclaim injector and the `reclaim`/`pagefault` buckets are being developed on exactly the configuration §3.1 rules out.

**Change to**

```diff
-| BPF feature versions: 1M insns 5.2; … `bpf_task_under_cgroup` 6.5; arena 6.9; private stack 6.13 |
+| BPF feature versions: 1M insns 5.2; bounded loops 5.3; ringbuf 5.8;
+  `bpf_get_task_stack` 5.9; BTF typed ksyms + task storage in tracing 5.11;
+  `bpf_for_each_map_elem` 5.13; `bpf_task_pt_regs` 5.15; `bpf_loop` 5.17;
+  `bpf_find_vma` 5.17; `bpf_map_lookup_percpu_elem` 5.19;
+  `bpf_ringbuf_reserve_dynptr` 5.19; open-coded iterators 6.4;
+  `bpf_task_under_cgroup` 6.5 (TRACING/SYSCALL/… only — see issue 39);
+  `bpf_iter_task_new` 6.7; arena 6.9; private stack 6.13;
+  `BPF_F_RB_OVERWRITE` 6.19 |
```

```diff
-per-frame APERF/MPERF or cpufreq deltas (Hetzner only — Hyper-V has no PMU)
+per-frame APERF/MPERF or cpufreq deltas (no core PMU in the guest, but the
+msr PMU with aperf/mperf is registered — test with one `perf stat -a -e
+msr/aperf/,msr/mperf/` as root before assuming Hetzner-only)
```

- Fix the VM before any reclaim work: Dynamic Memory off, fixed 8 GB, confirm `hv_balloon` is inert, and add both checks (plus the §9 feature probes) to `setup/provision.sh` and the "`bpf/` load test on both kernels".

---

### 41. M1's ladder uses an L2 level the plan never defines (minor)

**Plan says** (§4 line 251; §5 line 275; App. C line 425):

> - M1: information-loss ladder — for each injector, can the diagnosis be made at L0 / L1 / L2 / L3 / full trace?

> | Per injector: diagnosable at L0/L1/L2/L3/full? Bytes per hitch per level |

> L-level is the minimum level at which both (a) and (b) hold.

**Why it breaks**

- `grep -n 'L2'` over the plan hits only lines 251 and 275. §2 defines L0, L1 and L3 only. App. C's "minimum level at which both (a) and (b) hold" is not computable for an undefined level, and "bytes per hitch per level" has no L2 record to measure.

**Change to** — this issue is the single definition of the ladder; other issues cite it rather than restating it:

```markdown
L0  per-frame per-cause totals on the root timeline (per thread and per tgid)   ~200 B
L1  + largest stall's kernel stack, preemptor identity, one-hop waker + gap     ~400 B
L2  + top-N stalls per thread with kernel stacks (no chain beyond one hop)      ~600 B
L3  + chain links[0..d-1] per top-N stall (online propagation, issue 24)        ~1 kB
full  offline critical path over trace-cmd / perf sched (the validator, issue 43)
```

- With the online chain (issue 24) every level is a projection of one record and a `--depth` sweep, so M1 becomes a re-render of the same runs rather than four tool configurations.
- Knock-on: §2 (add the ladder), §4 W9 (M1's DoD), §5 M1 row, App. C.

---

### 42. libbpf refuses USDT attach on 32-bit ELF (minor)

**Plan says** (§2.1 line 77; §7 line 315):

> Implicit Vulkan layer. Intercepts `vkQueuePresentKHR`; fires USDT probes `frame_end` (before present), `present_begin`/`present_end`, `frame_begin` (after present returns)

> | Loader bypass: engines using `vkGetDeviceProcAddr` never call libvulkan's exported `vkQueuePresentKHR` | Hook silently misses presents | Implicit layer with USDT (layers sit in the device chain regardless); ICD-symbol uprobe as fallback; W1/W6 check per title; report as a finding |

**Why it breaks**

- A 32-bit game process loads the 32-bit build of the implicit layer, and `libbpf/src/usdt.c:341-343` refuses outright: "usdt: attaching to 32-bit ELF binary … is not supported". The result is no frame boundaries at all for that title, and it looks exactly like the loader-bypass failure §7 already expects — so it would be recorded as the wrong finding.

**Change to**

- Select 64-bit titles, or confirm the Proton build runs 32-bit games inside new-WoW64 64-bit processes (check per title, not per Proton version).
- Otherwise attach raw uprobes at the USDT note offsets, which §6.1 already computes (`readelf -n libVkLayer_hitchtrace.so`), instead of `bpf_program__attach_usdt`.
- Record process bitness per title in the manifest and add it to W6's per-title **layer** check. Issue 11 replaces the loader-bypass check with the only remaining question — "does the layer's USDT fire inside pressure-vessel?" — and bitness is the second way that check can fail; libbpf's "attaching to 32-bit ELF binary … is not supported" is the discriminator between the two failures.
- Knock-on: §7 loader-bypass row; §4 W6 per-title check (see issue 11).

---

### 43. Decide the daemon language now: C, extending `chaingraph.c` (minor)

**Plan says** (§2.1 line 79; §10 line 379):

> | `daemon/` | C++ (or Rust via libbpf-rs) | Loads BPF, registers target tgid/cgroup, consumes ringbuf, runs the v1 chain walk, symbolizes kernel stacks (`/proc/kallsyms`), snapshots `/proc/<pid>/maps` at flush, writes JSONL (+ optional Perfetto/DuckDB). |

> 4. Daemon language: C++ (fastest for you) vs Rust/libbpf-rs — choose by W1; don't switch later.

**Why it breaks**

- The repo already contains that daemon in C: `chaingraph.c` `open_and_load()` (line 871) and `main()` (line 932 to the end of the 1,140-line file) do skeleton load, `.rodata` config, map sizing, fentry attach order with fallback and ordered detach; `syms.c` is 1,882 lines covering kallsyms including modules, ELF `.symtab`/`.dynsym`, build-id and debuglink debuginfo, perf maps and `/proc/PID/map_files`; libbpf 1.7 is vendored statically with USDT support and a Makefile exists.
- Rust/libbpf-rs means a new build system and a new symbolizer (blazesym) — about a week for a ringbuf-to-JSONL consumer. C++ buys nothing here (bpftool skeletons compile as C++ and `syms.c` compiles as C), and leaving the choice open until W1 risks a rewrite of code that already works.

**Change to**

```diff
-| `daemon/` | C++ (or Rust via libbpf-rs) | … |
-| `walker/` | C++ | Pure user-space chain-walk library with unit tests … |
+| `daemon/` | C, extending `chaingraph.c` + `syms.c` | … |
+| `validator/` | Python | Offline critical path over trace-cmd/perf recordings
+  of hitchbench; agreement rate vs the online chain (M1's "full trace" rung) |
```

- One behavioural change is required: replace the "walk maps once at exit" pattern with per-record consumption, resolving stack ids and user symbols while the process is still alive — chaingraph prints `[unknown]` for processes that exited before symbolization.
- Delete §10 item 4 (decided).
