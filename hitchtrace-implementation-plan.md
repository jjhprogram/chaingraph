# hitchtrace — Implementation Plan

*Frame-scoped, cause-gated attribution of kernel-observable frame hitches in Linux games, using eBPF.*

Working name: `hitchtrace` (placeholder — check for collisions before publishing).
Plan date: 2026-09-19. Thesis window: ~12 weeks (target end: mid-December 2026).
Author: (you). Reviewer notes in **[R]** are things a thesis examiner will probe.

---

## 0. One-paragraph statement

Intermittent frame hitches are rare and not reproducible on demand. Continuous full tracing over hours of play either overwhelms RAM/disk or perturbs the frame timing being measured. Flight-recorder tracers (Perfetto, ftrace snapshots, LTTng) already solve *capture* — keep a ring buffer, dump it when a frame misses its budget — but their trigger sees only the symptom, each dump costs the whole window, and a human still has to find which thread waited on whom and why. `hitchtrace` attributes each over-budget frame of an **unmodified** Linux game to kernel-observable stall causes **at the moment the frame ends, in kernel**: per-thread, per-cause stall time accumulated in BPF maps and gated on the frame budget (normal frames emit nothing), plus a bounded in-kernel window of wakeup edges that is walked **only for bad frames** to recover the cross-thread/cross-process blocking chain. The thesis is a **measurement thesis**: it quantifies what this design captures that per-event thresholds and flight recorders don't, and what it costs.

**Is / is not**

| It is | It is not |
|---|---|
| A bench instrument for platform, scheduler, Proton/Mesa engineers doing hitch triage on long sessions | A consumer tool |
| An always-on, compact, per-hitch attribution of *off-CPU / kernel-observable* stalls (scheduling, locks, I/O, memory, GPU-fence waits, wineserver) | A profiler of the game's own CPU work or of GPU-internal causes |
| A method + measurements on real Proton titles and injected ground truth | A shippable product; adoption is not a success criterion |

---

## 1. Claims the thesis makes — and does not make

**Claimed (and measured):**
1. Frame-scoped accumulation captures hitches composed of many sub-threshold stalls ("death by a thousand cuts") that per-event thresholds miss. (M2)
2. In-kernel, frame-gated emission produces output proportional to *hitches*, not to time, with full cause attribution — vs. flight recorders whose output is proportional to hitch frequency × window size. (M3)
3. On-demand chain walking over a bounded edge window recovers cross-thread/process blocking chains for outlier frames at always-on cost; this addresses the "latency outliers" case Gregg's chain-graph work explicitly left open. (M1, L3)
4. A prevalence number for Linux games on Deck-class constraints: what fraction of over-budget frames have their excess dominated by kernel-observable stalls, by cause. (M0) — nobody has published this.

**Explicitly conceded (cite, don't claim):**
- "eBPF reduces kernel↔user data movement" — the founding premise of BPF tracing, not a contribution.
- Selective/triggered capture — DTrace speculation (2005), latency-tracker (2016), ftrace hist triggers + snapshot, Perfetto STOP_TRACING, Hindsight (NSDI'23).
- The chain-walk algorithm — Giraldeau & Dagenais critical path (TPDS 2016); Perfetto's `thread_executing_span`; wPerf (OSDI'18). Ours is the *online, bounded, frame-gated execution*, not the algorithm.
- Frame boundary via present call — Nsight Systems already marks frames at `vkQueuePresentKHR`; Android tools do uprobe frame detection.
- Per-task cause-classified stall accounting — kernel delay accounting (delayacct/taskstats) does this always-on, and is a baseline.
- Security/"no kernel module" — beats latency-tracker/VarMRI/hwui_mon (modules/patches), not ftrace/Perfetto (which run no custom kernel code).

**[R]** Expect: "systing/Perfetto + a missed-frame trigger gives the same result." Answer with M2/M3 numbers, not architecture arguments.

---

## 2. Architecture

```
 ┌──────────────── game process (unmodified, native or Proton) ────────────────┐
 │  game threads ──► Vulkan loader ──► [hitchtrace implicit layer] ──► ICD     │
 │                                        │ USDT: frame_begin/frame_end/       │
 │                                        │       present_begin/present_end    │
 └────────────────────────────────────────┼─────────────────────────────────────┘
                                          ▼ (uprobe on USDT)
 ┌─────────────────────────────── kernel (BPF) ────────────────────────────────┐
 │ sched_switch  sched_waking  irq/softirq  futex syscalls  handle_mm_fault    │
 │ vmscan/compaction tps  dma_fence_wait  nanosleep/poll syscalls  openat      │
 │        │            │                                                         │
 │        ▼            ▼                                                         │
 │  per-thread accumulators   per-CPU edge rings   per-thread stall rings        │
 │  (tid,cause)->ns, epoch    (ts,wakee,waker,irq) (out,in,cause,stackid,preempt)│
 │        │                                                                      │
 │  frame_end: frame_ns > budget && (predicate) ? emit(record) : lazy-reset      │
 │        │                                                                      │
 │        ▼ BPF ringbuf (only for bad frames)                                    │
 └────────┼──────────────────────────────────────────────────────────────────────┘
          ▼
 ┌──────────────── hitchtraced (user space) ────────────────────────────────────┐
 │ libbpf loader │ ringbuf consumer │ chain walker (v1) │ kernel-symbolizer      │
 │ /proc/pid/maps snapshot │ JSONL/Perfetto/DuckDB writer │ CLI                  │
 └──────────────────────────────────────────────────────────────────────────────┘
```

### 2.1 Components

| Component | Language | Responsibility |
|---|---|---|
| `layer/` | C | Implicit Vulkan layer. Intercepts `vkQueuePresentKHR`; fires USDT probes `frame_end` (before present), `present_begin`/`present_end`, `frame_begin` (after present returns), `init` (pid, budget, swapchain refresh). Zero logic beyond that. |
| `bpf/` | C (libbpf CO-RE) | All in-kernel logic: hooks, accumulators, edge/stall rings, predicate, emitter. One object file loads on both machines via CO-RE. |
| `daemon/` | C++ (or Rust via libbpf-rs) | Loads BPF, registers target tgid/cgroup, consumes ringbuf, runs the v1 chain walk, symbolizes kernel stacks (`/proc/kallsyms`), snapshots `/proc/<pid>/maps` at flush, writes JSONL (+ optional Perfetto/DuckDB). |
| `walker/` | C++ | Pure user-space chain-walk library with unit tests on synthetic edge/stall data. No kernel dependency — build and test this first. |
| `hitchbench/` | C++ / Vulkan | Present loop + job system + stall injectors + ground-truth log. |
| `baselines/` | shell/C | ftrace flight recorder; `perf --off-cpu --off-cpu-thresh`; delayacct poller; optional systing/Perfetto trigger config. |
| `analysis/` | Python (duckdb/pandas) | Result loaders, metrics, plots. |
| `setup/` | shell | Idempotent provisioning for the Hyper-V VM and the Hetzner box. |

### 2.2 Frame model

- **Frame N** = `[frame_begin(N-1), frame_end(N))` where `frame_end` fires at present *entry* and `frame_begin` at present *return*. Time spent inside `vkQueuePresentKHR` (swapchain acquire, vsync wait) is accounted to a separate `present_wait` bucket of frame N+1's window, so a capped-FPS game does not register present waits as stalls.
- **Budget**: `HITCHTRACE_BUDGET_US` (default 16667). Alternative predicate mode `median` (rolling 19-frame median × factor, Nsight/JankStats-style) for uncapped games — configurable, reported in every run manifest.
- **Cause-gated predicate (v2)**: `frame_ns > budget && offcpu_ns >= share * (frame_ns - budget)`. `share=0` reproduces symptom-only gating; `share=0.5` is the default. Both modes are compared in M2.
- **Threads of interest**: all threads of the game tgid (registered at layer `init`; children tracked via `sched_process_fork`), plus an optional **cgroup filter** (`bpf_task_under_cgroup`, kernel ≥ 6.5). Launch the game with `systemd-run --scope` so wineserver, pressure-vessel helpers and the game land in one cgroup.

### 2.3 Cause taxonomy (L0 buckets)

Classification is decided **at switch-in** of a thread of interest, from flags set by other hooks plus the `sched_switch` state. Kernel stack id is captured at switch-out (depth ≤ 8, kernel only) for offline refinement.

| Bucket | Trigger / hook | Rule | Notes |
|---|---|---|---|
| `preempt` | `sched_switch` with `prev_state == TASK_RUNNING` | out→in = runnable-not-running; record `next` as preemptor (tgid, comm); keep top-1 by duration per thread per frame | The *cause* is the preemptor, not a waker |
| `runqueue` | wakeup edge inside a blocked interval | `in_ts − wake_ts` | Falls out of time-interval matching |
| `futex` | `sys_enter/exit_futex` (+ `futex_wait`, `futex_waitv` on ≥6.7) set/clear per-thread flag; capture `uaddr` | blocked while flag set | Wine fsync; `uaddr` histogram per frame detects convoys |
| `iowait` | `prev->in_iowait` (CO-RE bitfield) at switch-out | blocked with in_iowait | Covers `io_schedule`, `folio_wait_bit`; device latency via `block_rq_issue/complete` optional (v2) |
| `pagefault_minor` / `pagefault_major` | `fentry/fexit handle_mm_fault` set/clear flag; `VM_FAULT_MAJOR` in return | on-CPU time inside fault + any blocking inside it | File name via `bpf_find_vma` (v2) |
| `reclaim` | `vmscan/mm_vmscan_direct_reclaim_begin/end` | wall time between begin/end, on thread | In-thread; no chain |
| `compaction` | `compaction/mm_compaction_begin/end` (filter tgid — kcompactd also fires) | wall time | In-thread |
| `gpu_wait` | `fentry/fexit dma_fence_wait_timeout`, `dma_fence_default_wait` | blocked inside | "CPU waited on GPU" — the CPU/GPU-bound triage signal; not *why* the GPU was slow |
| `present_wait` | USDT `present_begin/end` | blocked inside | Benign under FPS caps |
| `timer` | `nanosleep`/`clock_nanosleep` syscall flag, or wake edge with `irq` flag and thread in poll/sleep | blocked | Intentional sleep (frame limiter) — benign terminal |
| `poll` | `poll/ppoll/epoll_wait/select` flag | blocked | Sockets/pipes → wineserver hops (chain) |
| `other_block` | none of the above | blocked | Refine offline from kernel stack (e.g. `split_lock_warn`, `unix_stream_read_generic`, `pipe_read`) |
| `oncpu` | remainder | `frame_ns − Σ(off-CPU)` | Reported so the user knows when *not* to use this tool |

Optional L0 extras (cheap, high value): per-frame syscall counts by nr; per-frame APERF/MPERF or cpufreq deltas (Hetzner only — Hyper-V has no PMU) to catch frequency-ramp hitches.

### 2.4 In-kernel data structures

| Map | Type | Key → Value | Size | Notes |
|---|---|---|---|---|
| `targets` | hash | tgid → {budget_ns, epoch, frame_id, frame_start_ns, mode} | 16 | Written at USDT `init`/`frame_*` |
| `thread_acc` | hash | tid → {epoch, ns[N_CAUSES], largest{cause,ns,stackid}, preemptor{tgid,comm,ns}, flags} | 4096 | **Lazy reset**: on update, if `entry.epoch != targets[tgid].epoch` zero it first. No iteration needed at frame end. |
| `stall_ring` | hash of arrays | tid → ring[K] of {out_ns, in_ns, cause, stackid, preemptor_tid, flags} | K=64 (tune from W1 data) | Single writer per thread (switch-in), so no locking. Overflow sets `truncated` flag. |
| `edge_ring` | percpu array | slot → {ts, wakee_tid, waker_tid, waker_tgid, flags(irq/softirq)} + head | 16k slots/CPU (~4 MB on 8 CPUs) | Written at `sched_waking` on the waker's CPU; lock-free. Covers >100 ms at 1M wakeups/s. |
| `irq_state` | percpu array | cpu → {in_hardirq, in_softirq} | 1 | Maintained from `irq_handler_entry/exit`, `softirq_entry/exit`; read at `sched_waking` |
| `stacks` | stackmap | id → kernel frames | 8192 | `bpf_get_stackid` at switch-out (game threads only) |
| `fd_names` | hash | (tgid, fd) → name[64] | 8192 | From `sys_enter_openat` filename (v2 file attribution) |
| `events` | ringbuf | — | 8 MB | Records only for bad frames |

### 2.5 Emission and lazy reset

At `frame_end`: compute `frame_ns`; evaluate predicate against `targets[tgid]` totals (kept per tgid by atomic adds at switch-in to avoid iterating threads). If over budget → reserve ringbuf record, write header + per-tgid totals, then per-thread details by iterating a bounded `tgid → tid[256]` registry with `bpf_loop`; L3 additionally copies edge slots newer than `frame_start − 2 frames` and the relevant stall rings. Then bump `epoch` (resets everything lazily) and `frame_id`. Normal frames: bump epoch only. **Zero bytes cross to user space for normal frames.**

### 2.6 Chain walk (L3)

Edges are the `sched_waking` events; chains are derived on demand. Per stalled thread interval `[out, in]`:

1. Find the edge with `wakee == tid` and `out ≤ ts ≤ in` (bounded scan; time-interval matching, **not** "last waker" — a thread wakes many times per frame).
2. `runqueue = in − ts`; `blocked = ts − out`.
3. Hop rule: look at the waker `W` immediately before `ts`. If `W` had just resumed from a stall ending ≤ `ts` (its own stall ring) → recurse into that stall. If `W` was running continuously → terminate: cause is `W`'s own execution (or `W`'s preemption, which the preempt bucket catches).
4. Edge flagged `irq`/`softirq` → terminal: `IRQ`/`softirq` (disk completion, network, timer). Distinguish `timer` via the wakee's sleep/poll flag.
5. Depth ≤ 6, visited set (A→B→A loops), stop at window boundary; emit `truncated` if any ring overflowed.

v1 walks in user space over the dumped window (simple, correct, volumetrically ≈ an ftrace snapshot with two events — **say so**). v2 (stretch) walks in-kernel with `bpf_loop` and emits only the relevant chains; the emitted-volume difference between v1 and v2 is a thesis graph.

### 2.7 Stack policy

- **Kernel stacks** (depth ≤ 8) at switch-out for threads of interest: cheap, deduplicated, gives blocked reason on stock kernels that lack Android's `sched_blocked_reason`. Core deliverable.
- **User stacks**: optional (`--user-stacks`). Frame-pointer walk is useless on shipped games; `.eh_frame` unwinding (reuse the OpenTelemetry/Parca BPF unwinder) works for ELF code (native games, Mesa, DXVK, libc). Emit module+offset with a `/proc/<pid>/maps` snapshot; symbolize offline.
- **Explicit non-goal**: PE frames under Wine (different unwind info; Wine's syscall dispatcher switches stacks). One paragraph in the thesis; a "coverage" number (% of stalls resolved to a game-side frame) if `--user-stacks` ships.
- Alternative for studios: timestamp join against their engine profiler — thread names propagate from Wine (verify).

### 2.8 Hitch record (JSONL, one line per bad frame)

```json
{"run":"2026-10-14T18:03:11Z-cp2077-deck12g", "frame_id":18204, "t_end_ns":..., "frame_ns":25100000,
 "budget_ns":16667000, "mode":"fixed", "predicate":"cause_gated:0.5",
 "tgid":41213, "totals_ns":{"oncpu":9800000,"futex":7800000,"preempt":4100000,"runqueue":900000,"iowait":0,
   "pagefault_minor":1800000,"pagefault_major":0,"reclaim":600000,"compaction":0,"gpu_wait":0,
   "present_wait":100000,"timer":0,"poll":0,"other_block":0},
 "threads":[{"tid":41220,"comm":"RenderThread","ns":{"futex":7800000,"preempt":0},
   "largest":{"cause":"futex","ns":7000000,"kstack":["futex_wait_queue","futex_wait","do_futex","__x64_sys_futex"]},
   "preemptor":null,
   "waker":{"tid":41231,"comm":"Worker3","gap_ns":300000}},
   {"tid":41231,"comm":"Worker3","ns":{"preempt":6900000},"preemptor":{"tgid":2210,"comm":"steamwebhelper","ns":6900000}}],
 "chains":[{"root":41220,"hops":[{"tid":41220,"stall":"futex","ns":7000000},{"tid":41231,"stall":"preempt","by":"steamwebhelper","ns":6900000}],"terminal":"preempt","truncated":false}],
 "level":"L3", "flags":{"edge_ring_overflow":false,"stall_ring_overflow":false}}
```

---

## 3. Environment

### 3.1 Development: Hyper-V VM (weeks 0–5)

- Gen 2 VM, Secure Boot off (or MS UEFI CA template), **fixed** memory 8 GB (dynamic memory off — ballooning interferes with reclaim tests), ≥ 4 vCPUs.
- **No PMU** in the guest: no hardware counters, `perf record` uses `cpu-clock`. Counter-based experiments wait for Hetzner.
- GPU: `lavapipe` (Mesa software Vulkan). `vkcube`, MangoHud, the layer and USDT all work; timing is fake.
- **Headless from day one**: Xorg `xf86-video-dummy` or Sway `WLR_BACKEND=headless`; SSH + VS Code Remote-SSH. Same stack as the server.
- Checkpoints after: toolchain installed; headless stack works; first ringbuf record.

### 3.2 Evaluation: Hetzner AX42 (weeks 6–11)

- Ryzen 7 PRO 8700GE (8C/16T, Radeon 780M RDNA3 iGPU), 64 GB DDR5, 2×512 GB NVMe; hourly billing; Falkenstein/Helsinki. Check current price (Hetzner adjusted dedicated pricing June 2026).
- Day-one checklist: `installimage` (same distro as VM) → remove `nomodeset` from kernel cmdline (Hetzner images blacklist GPU drivers by default) → `linux-firmware` + Mesa RADV → `ls /dev/dri` and `vulkaninfo --summary` must show the Phoenix iGPU **before anything else**; if not, cancel.
- Headless display: `amdgpu.virtual_display=<pci-id>,1` → DRM virtual connector → Sway headless (or gamescope) with a vblank cadence; Sunshine (VA-API) for streaming; Moonlight on Windows over Tailscale/WireGuard. Never expose Sunshine to the internet; Robot firewall pinned to your IP.
- Steam via the Steam-Headless recipe or native + multilib; dedicated `steam` user with persistent home; single-player titles with built-in benchmarks (Cyberpunk 2077, Shadow of the Tomb Raider, …) + 2 open-source native titles for reproducibility.
- **Deck-class constraints via cgroups** (methodology, not accident): `systemd-run --scope -p MemoryMax=12G -p AllowedCPUs=0-3 …`; storage latency via `dm-delay` on a partition (`dmsetup create slowdisk --table "0 <sectors> delay /dev/nvmeXnYpZ 0 30"`); background load via `stress-ng`. Every run manifest records these.
- Disconnect the stream during measurement runs (the encoder is an observer); drive scenes with in-game benchmarks or `ydotool`; MangoHud frametime log as ground truth for frame times.

### 3.3 Distro and toolchain (both machines identical)

- Ubuntu 24.04 LTS + HWE kernel (BTF present, frame pointers in system libs, packaged `bpftrace`/`libbpf-dev`/`linux-tools`; kisak-mesa PPA for current RADV). Arch is the alternative if newer kernel/Mesa features are needed. **Same distro/kernel line on both machines**, provisioned by `setup/provision.sh`.
- Packages: `clang llvm libbpf-dev bpftool bpftrace linux-tools-$(uname -r) pahole trace-cmd gpuvis mangohud vulkan-tools vulkan-validationlayers libvulkan-dev systemtap-sdt-dev (sys/sdt.h) cmake ninja python3-duckdb`.
- `vmlinux.h` generated per machine (`bpftool btf dump file /sys/kernel/btf/vmlinux format c`), committed under `bpf/vmlinux/<kernel>.h`; CO-RE relocations do the rest.
- `sysctl kernel.task_delayacct=1` for the delayacct baseline; `kernel.perf_event_paranoid=-1` on the bench box.

---

## 4. Milestones (12 weeks from 2026-09-22)

Each milestone has a **Definition of Done (DoD)** and a **cut line** (what to drop if late). Guaranteed scope: **L0 + L1 + harness + baselines + M0 + M2 + M3**. Stretch: **L3 v1 + M1**, then in-kernel walk.

### W0 (Sep 19–21) — Environment
- Hyper-V VM provisioned by script; headless stack; `vkcube` under lavapipe; BTF verified; repo skeleton; CI job that runs `walker/` unit tests.
- DoD: `bpftrace -e 'uprobe:/usr/lib/x86_64-linux-gnu/libvulkan.so.1:vkQueuePresentKHR { @[comm]=count(); }'` counts vkcube presents.

### W1 (Sep 22–28) — Validate the data model with bpftrace (no real code yet)
Scripts in `docs/w1-bpftrace/`, each with a results note:
1. Present hook fires? (vkcube; later a Proton game on Hetzner → loader-bypass check.)
2. Per-thread off-CPU by `prev_state` for a tgid; stalls-per-frame distribution → sets K.
3. `sched_waking` rate system-wide under load → sets edge-ring size and the always-on cost expectation.
4. Kernel stacks (depth 3) at switch-out in D/S state → what blocked reasons look like.
5. `getdelays` on a thread → know exactly what delayacct reports.
6. `walker/` library: implement time-interval matching + hop rule; unit tests on synthetic timelines incl. loops, IRQ terminals, overflow.
- DoD: numbers for stalls/frame, wakeups/s, and a passing walker test suite.
- Related-work verification (see §9) done this week — it changes the intro, not the code.

### W2–W3 (Sep 29–Oct 12) — Layer + L0 core
- Implicit layer with `init`, `frame_begin`, `frame_end`, `present_begin`, `present_end` USDTs; manifest with `enable_environment`; tested under vkcube and under a Proton title (Hetzner, W6) later.
- BPF: `sched_switch` accumulator with lazy epoch reset; hooks for futex, iowait bitfield, `handle_mm_fault` fentry/fexit, reclaim, compaction, `dma_fence_wait_timeout`, sleep/poll syscalls; per-tgid atomic totals; predicate; ringbuf emitter (header + per-thread details via `bpf_loop` over the tid registry).
- Daemon: load, register tgid, consume ringbuf, kernel symbolization, JSONL writer, CLI (`hitchtrace run -- <cmd>` / `hitchtrace attach <pid>`).
- DoD: a test app that injects one 30 ms sync read / `usleep` / futex wait produces **exactly one** record with the right bucket; a run of 10k normal frames produces zero records and shows constant RSS.
- Cut line: if not done by Oct 12, drop `gpu_wait`/`poll`/`timer` buckets to `other_block` and continue.

### W4 (Oct 13–19) — Harness + baselines
- `hitchbench`: Vulkan present loop (FIFO and immediate), N worker threads with a futex-based job system; injectors (each with a frame-id ground-truth log): `sync_read` (cold cache), `holder_preempt` (lock holder pinned with a `stress-ng` hog on its core), `reclaim` (memory hog inside a `MemoryMax` cgroup), `compaction` (THP + fragmentation), `bg_periodic` (a daemon waking every N s doing 5 ms of work), `timer_sleep`, `gpu_heavy` (fence waits), `thousand_cuts` (N × ~0.5 ms futex/fault stalls, no single stall above threshold).
- Baselines scripted and validated on hitchbench: ftrace flight recorder (§6.1), `perf record --off-cpu --off-cpu-thresh`, delayacct poller at frame boundaries (§6.3), optional systing.
- DoD: each injector reproduces its class deterministically; each baseline produces output for at least one injected class.
- Cut line: `compaction` and `gpu_heavy` injectors are the first to drop.

### W5 (Oct 20–26) — L1
- Blocked reason (kernel stack id) per stall; preemptor identity (top-1 per thread per frame); one-hop waker with gap (runqueue) from the edge ring; `largest` stall per thread; stall ring per thread with overflow flag.
- DoD: for each hitchbench injector, the record's `largest.cause`, `preemptor`, or `waker` names the injected mechanism in ≥ 95% of injected frames.

### W6 (Oct 27–Nov 2) — Hetzner bring-up + real-game smoke
- Provision by script; iGPU/Vulkan verified; virtual display; Sunshine/Moonlight over Tailscale; Steam; 3 Proton titles + 2 native titles installed; MangoHud logs enabled.
- Loader-bypass check on each title (does the loader-symbol uprobe fire? the layer USDT does?). Record per title.
- DoD: `hitchtrace` runs against each title for 30 minutes with < X% CPU (record X) and produces plausible records; MangoHud and hitchtrace frame times agree within tolerance.

### W7 (Nov 3–9) — M0 prevalence study
- 5 titles × 3 conditions (unconstrained; Deck-class cgroups; Deck-class + background Steam download + cold shader cache), ≥ 45 min each, built-in benchmark loops where available.
- Output: distribution of off-CPU share of excess per over-budget frame; breakdown by bucket; game-initiated vs external split.
- DoD: the M0 figure and table exist, whatever they show.

### W8 (Nov 10–16) — M2 aggregate predicate vs per-event threshold
- hitchbench `thousand_cuts` sweep (N × d ms with N·d > budget, d below threshold) + real-title data: classify each over-budget frame by "fraction of excess in the single largest stall".
- Compare capture rate: hitchtrace (fixed + cause-gated) vs `perf --off-cpu-thresh` at several thresholds vs ftrace per-event synthetic events.
- DoD: the M2 figure; the "single dominant stall vs aggregate" distribution on real titles.

### W9 (Nov 17–23) — L3 v1 + M1 (stretch begins)
- Edge rings + stall rings wired; window dump on bad frames; user-space walk; chain in the record; truncation stats vs K.
- M1: information-loss ladder — for each injector, can the diagnosis be made at L0 / L1 / L2 / L3 / full trace? (binary per injector, plus time-to-diagnosis for a human on 5 blind cases, if time allows).
- DoD: chains for `sync_read` (Render ← Streaming ← kworker ← IRQ) and `holder_preempt` (Render ← Worker ← preempted-by) recovered on hitchbench; truncation-rate-vs-K plot.
- Cut line: if W8 slipped, L3 is design-only in the thesis (§2.6 + cost analysis) and W9 goes to M3.

### W10 (Nov 24–30) — M3 observer effect and output volume
- Same workload (hitchbench steady state + one real title benchmark loop), same cgroup constraints, ≥ 5 runs each: baseline (no tool), hitchtrace L0/L1/L3-v1, ftrace flight recorder (snapshot on `frame_lat`), perf off-cpu, delayacct poll, systing.
- Metrics: p99 frame time and 1% lows vs no-tool; tool CPU%; bytes emitted per hour at hitch rates of 0.01/s, 0.1/s, 1/s (drive with `bg_periodic`); flight-recorder drain time and dropped snapshots at 1/s.
- DoD: the M3 table. Be prepared for the flight recorder to win on perturbation; report it either way.

### W11 (Dec 1–7) — Stretch or buffer
- Option A: in-kernel walk (`bpf_loop`, depth ≤ 6) → emitted-volume comparison vs v1.
- Option B: file-path attribution (`openat` map + `bpf_find_vma`) and `gpu_wait` refinement.
- Option C: catch-up.

### W12 (Dec 8–14) — Write-up
- Figures frozen by Dec 10. Surprises file → "lessons" section. Verify every version number and citation.

---

## 5. Measurements (specification)

| ID | Question | Workloads | Metric | Result either way |
|---|---|---|---|---|
| **M0** | What fraction of over-budget frames in Linux games have their excess dominated by kernel-observable stalls, by cause, under Deck-class constraints? | 5 Proton/native titles × 3 conditions | Distribution of `Σ offcpu / (frame − budget)`; bucket shares; game-initiated vs external | Low share → tool scope is narrow (say so); high share → motivation nobody else can cite |
| **M1** | How much diagnostic accuracy is lost at each summary level? | hitchbench injectors (8 classes), real-title blind cases | Per injector: diagnosable at L0/L1/L2/L3/full? Bytes per hitch per level | Shows where the ladder saturates; justifies L1 as the practical core |
| **M2** | Does frame-scoped aggregation catch hitches that per-event thresholds miss? | `thousand_cuts` sweep; real titles | Capture rate vs threshold; fraction of real hitches that are "aggregate" (largest stall < 50% of excess) | If aggregate hitches are rare, thesis narrows to L3; if common, that fraction is the headline |
| **M3** | What does always-on cost, and how does output scale, vs flight recorders and per-event tools? | Steady state + controlled hitch rates | Δp99 / Δ1%-low vs no-tool; tool CPU%; bytes/hour; snapshot drain/drop | Honest table; flight recorder may win on perturbation |

Controls for all: same binary/build of each game (record build id; game updates invalidate data), same scene/benchmark loop, ≥ 5 runs, report distributions (p50/p99/1% low), cgroup constraints in the manifest, streaming disconnected, shader cache state recorded.

---

## 6. Baselines (exact recipes)

### 6.1 ftrace flight recorder (in-kernel frame predicate, no BPF)
```
# uprobes on the layer's two USDT sites (get offsets via `readelf -n libVkLayer_hitchtrace.so` or `perf probe -x ... %hitchtrace:frame_begin`)
echo 'p:hitch/frame_begin /usr/lib/…/libVkLayer_hitchtrace.so:0x<OFF_BEGIN>' >> /sys/kernel/tracing/uprobe_events
echo 'p:hitch/frame_end   /usr/lib/…/libVkLayer_hitchtrace.so:0x<OFF_END>'   >> /sys/kernel/tracing/uprobe_events
echo 'frame_lat u64 lat; pid_t pid' > /sys/kernel/tracing/synthetic_events
echo 'hist:keys=common_pid:ts0=common_timestamp.usecs' > events/hitch/frame_begin/trigger
echo 'hist:keys=common_pid:lat=common_timestamp.usecs-$ts0:onmatch(hitch.frame_begin).frame_lat($lat,common_pid)' > events/hitch/frame_end/trigger
echo 1 > snapshot                                   # allocate the snapshot buffer once
echo 'snapshot if lat > 16600' > events/synthetic/frame_lat/trigger
echo 1 > events/sched/sched_switch/enable; echo 1 > events/sched/sched_waking/enable   # + the same tracepoints hitchtrace uses
echo 1 > tracing_on                                  # ring buffer, overwrite mode
```
Drain `snapshot` from user space after each trigger; record drain time and whether a second trigger overwrote an unread snapshot. This is the "symptom-gated, whole-window" comparator. Note: it cannot express frame-scoped sums or a cause-gated predicate — that is the point.

### 6.2 perf off-CPU with threshold
`perf record --off-cpu --off-cpu-thresh <us> -p <pid>` (needs perf ≥ 6.16 for `--off-cpu-thresh`; verify). Sweep thresholds 100/500/2000 µs for M2. `perf sched record` as the "stream everything" reference (VaporMark's input).

### 6.3 delayacct poll
`sysctl kernel.task_delayacct=1`; a small netlink (TASKSTATS) reader that diffs per-thread `cpu_run_virtual/blkio/swapin/freepages/thrashing/compact/wpcopy/irq` delays at each `frame_begin` (driven by the same USDT via a bpftrace or a tiny BPF that just signals). No futex category, no waker, no per-frame scoping without user-space diffing — the honest "80% with zero kernel code" comparator.

### 6.4 systing / Perfetto (optional)
`systing --pid <pid>` for the Linux-desktop Perfetto pipeline; Perfetto `RING_BUFFER` + `STOP_TRACING` trigger fired from the layer if time allows. Also the ground-truth viewer during development.

---

## 7. Risks and mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Loader bypass: engines using `vkGetDeviceProcAddr` never call libvulkan's exported `vkQueuePresentKHR` | Hook silently misses presents | Implicit layer with USDT (layers sit in the device chain regardless); ICD-symbol uprobe as fallback; W1/W6 check per title; report as a finding |
| BTF missing on the target kernel | CO-RE object won't load | Verify `/sys/kernel/btf/vmlinux` day one on both machines; Ubuntu/Arch kernels ship BTF |
| Hyper-V has no PMU | Counter experiments impossible in VM | Schedule them on Hetzner only |
| Hetzner iGPU not exposed / `nomodeset` | No Vulkan on the eval box | Day-one check; cancel and fall back to a HOSTKEY 4090 box or a cloud VM for functional runs |
| Anti-cheat titles refuse headless/streamed | Fewer real titles | Single-player titles with built-in benchmarks; two open-source native games |
| `sched_waking` system-wide BPF cost exceeds ftrace's write cost | The "cheap always-on" premise weakens | M3 measures it; keep the edge recorder minimal (no stacks); report honestly |
| Concurrency in rings | Corrupted edges/stalls | Per-CPU edge rings (lock-free); per-thread stall rings written only at switch-in |
| Verifier friction (bounded loops, stack size, map iteration) | Time sink | `bpf_loop`/`bpf_for_each_map_elem`; keep records fixed-size; record friction as observations, not results |
| Wine thread names don't propagate | Records harder to read | Fall back to tid + role inference (present caller = render thread); verify early |
| User-stack resolution on Proton | Attribution to game code impossible | Scoped out explicitly (§2.7); kernel stacks are the deliverable |
| L3 v1 volume ≈ ftrace snapshot | Skeptic argument | State it; the in-kernel walk (W11) is where the volume claim lives |
| Game updates during the study | Data invalidated | Disable auto-update; record build ids; keep all runs of a title within one week |
| Time | Everything | Cut lines in §4; guaranteed scope is L0+L1+M0+M2+M3 |

---

## 8. Repository layout and conventions

```
hitchtrace/
  layer/            # Vulkan implicit layer (C), USDT probes, manifest JSON
  bpf/              # BPF programs (libbpf CO-RE), vmlinux/<kernel>.h
  daemon/           # user-space daemon + CLI
  walker/           # chain-walk library + unit tests (synthetic timelines)
  hitchbench/       # Vulkan benchmark + injectors + ground-truth logger
  baselines/        # ftrace/, perf/, delayacct/, systing/
  analysis/         # duckdb/pandas scripts, figure generators
  results/          # JSONL runs (git-lfs) + manifest.json per run
  setup/            # provision.sh (VM & Hetzner), headless stack, tailscale
  docs/             # design.md, surprises.md, related-work.md, w1-bpftrace/
```

- **Run manifest** (`results/<run>/manifest.json`): kernel, distro, Mesa, GPU, tool level, budget/predicate, cgroup limits, game + build id, scene, shader-cache state, streaming on/off, hitch injector config.
- **surprises.md**: every environment/kernel/Wine surprise the day it happens (these become the "lessons" section).
- Tests: `walker/` unit tests in CI; `bpf/` load test on both kernels; hitchbench smoke test producing exactly one record per injected stall.

---

## 9. Related work — verification to-dos before the intro is written

| Item | Status | Action |
|---|---|---|
| LPC 2025 "Gaming on Linux" MC — Min (Igalia/LAVD): no way to trigger on rare spikes; brute-force sampling; "correlation between high-level and microscopic view is missing"; Vernet's per-frame wakeup-chain question | Reported by LWN (Jan 2026); recording + slides linked | **Watch the recording; cite video + slides, not LWN** |
| VarMRI (arXiv 2601.10572, OSU, Jan 2026) — per-request in-kernel cumulative accounting; needed kernel patch; eBPF attempt failed | Abstract verified | Read fully; cite as closest design precedent |
| Giraldeau & Dagenais, IEEE TPDS 2016 (critical path from kernel traces) | Verified | Cite as the walk algorithm |
| Perfetto `thread_executing_span` / critical-path stdlib; `sched_blocked_reason`; `sched_waking` vs `sched_wakeup` | Verified | Cite as offline equivalent + IRQ caveat |
| wPerf (OSDI'18); Patel et al. QRS 2021 (arXiv 2207.06515) | Known | Read; both offline over full traces |
| Gregg: off-CPU analysis; off-wake; chain-graph prototype (2016, "outliers… yet") | Verified | Cite the "yet" |
| perf `--off-cpu` (2022) and `--off-cpu-thresh` (2025) | Verified (LWN) | Confirm perf version on the bench box |
| Linux delay accounting docs (delayacct, `delaytop`); PSI triggers | Verified | Baseline + taxonomy comparison |
| latency-tracker (Desfossez 2016); DTrace speculation; ftrace hist/synthetic events; LTTng triggers; Hindsight (NSDI'23) | Verified | Concede selective capture |
| Nsight Systems: `vkQueuePresentKHR` frames, Frame Health rolling median, blocked-state backtraces; Superluminal (Linux) | Verified | Concede frame marker + offline attribution |
| frame-analyzer-ebpf / hwui_mon (Android uprobe frame detection + jank threshold) | Partially verified (hook target unknown) | Cite as C1 precedent on Android |
| systing (Bacik, 2025–26); GamePulse/RigSignal (May 2026); Mesa Perfetto/gpuvis integration | Verified | Baselines / adjacent tools |
| Vulkan loader: `vkGetDeviceProcAddr` bypasses loader trampolines | Verified (LunarG docs, RenderDoc guide) | Justifies the layer |
| BPF feature versions: 1M insns 5.2; bounded loops 5.3; ringbuf 5.8; `bpf_get_task_stack` 5.9; `bpf_for_each_map_elem` 5.13; `bpf_loop` 5.17; `bpf_find_vma` 5.17; open-coded iterators 6.4; `bpf_task_under_cgroup` 6.5; arena 6.9; private stack 6.13 | Believed correct | Verify each against kernel changelogs before citing |

---

## 10. Open design decisions (decide by end of W2)

1. Budget: fixed vs refresh-derived vs rolling-median mode — support fixed + median; default fixed.
2. Cause-gated predicate default share (0.5) — sweep in M2.
3. K (stall ring) and edge-ring slots — from W1 measurements: p99 stalls/frame + margin; edge window ≥ 3 frames at observed wakeup rate.
4. Daemon language: C++ (fastest for you) vs Rust/libbpf-rs — choose by W1; don't switch later.
5. Output: JSONL always; Perfetto/DuckDB export if time (makes the tool complementary to systing rather than competing).
6. Whether `--user-stacks` ships at all (only if W9 finishes early).

---

## Appendix A — W1 bpftrace scripts (starting points)

```bash
# A1: does the present hook fire? (swap the path for the Proton runtime's libvulkan on Hetzner)
bpftrace -e 'uprobe:/usr/lib/x86_64-linux-gnu/libvulkan.so.1:vkQueuePresentKHR { @presents[comm, pid] = count(); }'

# A2: per-thread off-CPU by prev_state for one tgid, plus stalls-per-frame proxy (prints every second)
bpftrace -e '
tracepoint:sched:sched_switch /args->prev_pid != 0 && curtask->tgid == $1/ { @out[args->prev_pid] = nsecs; @st[args->prev_pid] = args->prev_state; }
tracepoint:sched:sched_switch /@out[args->next_pid]/ {
  $d = nsecs - @out[args->next_pid];
  @offcpu[args->next_comm, @st[args->next_pid] == 0 ? "R(preempt)" : "blocked"] = sum($d);
  @stalls[args->next_comm] = count();
  delete(@out[args->next_pid]); delete(@st[args->next_pid]); }
interval:s:1 { print(@offcpu); print(@stalls); clear(@offcpu); clear(@stalls); }' <GAME_TGID>

# A3: system-wide wakeup edge rate (the always-on cost driver)
bpftrace -e 'tracepoint:sched:sched_waking { @waking = count(); } interval:s:1 { print(@waking); clear(@waking); }'

# A4: blocked reasons — kernel stack (3 frames) at switch-out in non-running state, game threads only
bpftrace -e 'tracepoint:sched:sched_switch /curtask->tgid == $1 && args->prev_state != 0/ { @reason[kstack(3)] = count(); }' <GAME_TGID>

# A5: delayacct snapshot for one thread (after sysctl kernel.task_delayacct=1)
getdelays -d -t <TID>
```

## Appendix B — Deck-class constraint recipe (Hetzner)

```bash
# transient cgroup: 12 GB, 4 cores, throttled reads on the game's disk
systemd-run --scope --unit=game -p MemoryMax=12G -p AllowedCPUs=0-3 \
  -p IOReadBandwidthMax="/dev/nvme1n1 90M" -- steam -applaunch <APPID>
# storage latency emulation (30 ms added to every I/O on a dedicated partition holding the game library)
dmsetup create slowdisk --table "0 $(blockdev --getsz /dev/nvme1n1p3) delay /dev/nvme1n1p3 0 30"
# background contention
stress-ng --cpu 2 --cpu-load 60 --timeout 45m &
```

## Appendix C — Definition of "diagnosed" for M1

A record diagnoses an injected stall if it names, for the over-budget frame, (a) the injected mechanism's bucket as the largest contributor, and (b) where applicable, the correct counterpart: preemptor comm (`holder_preempt`, `bg_periodic`), waker/hop chain ending in `iowait`/IRQ (`sync_read`), file name (v2), or `gpu_wait` (`gpu_heavy`). L-level is the minimum level at which both (a) and (b) hold.
