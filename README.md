# chaingraph

Off-CPU time for blocked threads, annotated with the **whole chain of wakeups**
that ended each sleep: who woke the thread, who woke *that* waker, and so on
until the chain reaches an interrupt.

This is a libbpf/CO-RE implementation of the chain graph idea from Brendan
Gregg's [*Who is waking the waker? (Linux chain graph prototype)*](https://www.brendangregg.com/blog/2016-02-05/ebpf-chaingraph-prototype.html)
(2016), which was a bcc prototype limited by the eBPF of the time.

Real output from `chaingraph -K -d 4 5` on the test workload (Hyper-V VM,
kernel 7.0), abridged where marked:

```
    waker 4: [hardirq]
        asm_sysvec_hyperv_stimer0
        sysvec_hyperv_stimer0
        ...
        hrtimer_interrupt
        __hrtimer_run_queues
        hrtimer_wakeup
        wake_up_process
        try_to_wake_up
    --
    waker 3: cg_source
        entry_SYSCALL_64_after_hwframe
        ...
        anon_pipe_write
        ...
        try_to_wake_up
    --
    waker 2: cg_relay1
        ... (same pipe-write path)
    --
    waker 1: cg_relay2
        ... (same pipe-write path)
    --
        __schedule
        schedule
        anon_pipe_read
        vfs_read
        ...
        entry_SYSCALL_64_after_hwframe
    target:   cg_sink
        4984040 us over 499 sleeps
```

Read it bottom-up: `cg_sink` spent 4.98 s blocked in `anon_pipe_read`; it was
woken by `cg_relay2` writing to a pipe, which was woken by `cg_relay1`, which
was woken by `cg_source`, which was woken by a timer interrupt.

## Quick start

Requirements: root, and a kernel with BTF (`/sys/kernel/btf/vmlinux`). The
programs need Linux 5.18 or newer to load, but only 7.0 has been tested, and
a recent 6.x or 7.x kernel is strongly recommended: older kernels allocate map
memory with plain atomic allocations under scheduler locks.

To build: `clang gcc make libelf-dev zlib1g-dev` and `bpftool` (on Ubuntu:
`linux-tools-common linux-tools-$(uname -r)`). libbpf v1.7.0 (`libbpf/`) is
built statically; FlameGraph (`FlameGraph/`) is vendored for SVG output.

```bash
make                                   # as your normal user
sudo ./build/chaingraph 10             # 10 s, human-readable
sudo ./build/chaingraph -f 10 > out.folded
FlameGraph/flamegraph.pl --colors=chain --countname=us < out.folded > chain.svg
sudo ./chaingraph-svg.sh chain.svg -d 4 -m 1000 10   # both steps at once
```

In the SVG, the bottom (blue) part of each tower is the blocked thread's
off-CPU stack; above each `--` (aqua) is the next waker's stack, upside down
so that the wakeup points meet, topped by the waker's name.

Common options (`--help` for all):

| option | meaning |
|---|---|
| `-p PID` / `-t TID` / `-c COMM` | only aggregate sleeps of these tasks (wakers are always tracked) |
| `-d N` | waker levels to keep, 1–8 (default 4); the final interrupt or idle link counts as a level |
| `-m USEC` / `-M USEC` | ignore sleeps shorter / longer than this |
| `--state MASK` | 1 = S, 2 = D, 4 = I (`TASK_IDLE`), 8 = other |
| `-U` / `-K` | user stacks only / kernel stacks only |
| `-P` | keep thread ids (`comm/TID`) |
| `-f` | folded output for `flamegraph.pl --colors=chain` |
| `--softirq auto\|keep\|cut` | chain continuation past softirq-context wakers |
| `--stack-storage-size N`, `--perf-max-stack-depth N`, `--max-chains N` | map sizing |

## How it works

```
 sched_waking (in waker W's context)          sched_switch
 ─────────────────────────────────────        ───────────────────────────────
 T.links = [W: comm, kstack, ustack,          prev sleeps  → T.offcpu_ts, off-CPU stacks
            W.links[0..depth-2]]              next resumes → chains[T, stacks, T.links]
 unless W was only *interrupted* (cut)                        += now - offcpu_ts
```

* **Per-task state** lives in `BPF_MAP_TYPE_TASK_STORAGE`: blocked timestamp,
  off-CPU stack ids, and the chain `links[0..7]` (who woke me, who woke them…).
  Storage is freed with the task.
* **Chain propagation** (`tp_btf/sched_waking`, `tp_btf/sched_wakeup_new` for
  fork edges): the wakee's chain becomes the waker's current kernel+user stack
  followed by the waker's own chain, capped at `--depth` (`[chain truncated]`).
  Each chain has a sequence count so a reader can detect that another CPU was
  rewriting it at the same time (`[chain torn]`, rare).
* **Where chains end.** "All roads lead to metal":
  * hardirq/NMI context, from the per-CPU `__preempt_count` (a weak typed
    ksym; the `pcpu_hot` layout of v6.2–v6.14 is handled too) → `[hardirq]`;
  * work done on the way out of an interrupt — softirqs run inside
    `irq_exit_rcu()`, or ksoftirqd wakeups on `threadirqs` kernels — detected
    with an `fentry`/`fexit` nesting counter on `irq_exit_rcu`/`irq_exit`
    → `[softirq]` / `[irq exit]`. The kernel stack of these links is trimmed
    at the interrupt entry, since what lies below was merely interrupted;
  * the idle task waking something from process context, shown as
    `swapper/N` with its full stack. Interrupts that land on an idle CPU are
    labelled and trimmed like any other interrupt.

  Softirqs run from a task's own `local_bh_enable()` (e.g. loopback
  networking) keep the chain through that task.
* **Accounting** (`tp_btf/sched_switch`): only voluntary sleeps
  (`!preempt && prev_state != TASK_RUNNING`) of tasks passing the filters are
  timed; when the task is switched back in, the time (switch-out → switch-in,
  like `offwaketime`) is added in-kernel to a hash map keyed by
  `struct chain_key` (target, off-CPU stacks, full chain). The wakeup that
  ends a sleep is consumed at switch-in, which is robust to wakers racing
  ahead of the switch-out tracepoint.
* **Userspace** (`src/chaingraph.c`) sets `.rodata` config, sizes maps, attaches,
  then walks the maps once at the end, symbolizes (`src/syms.c`: kallsyms;
  ELF `.symtab`/`.dynsym`, build-id and debuglink debuginfo, perf maps, via
  `/proc/PID/map_files`), strips tracing frames, merges keys that render
  identically, and prints.

### The 2016 prototype's "current issues", revisited

| 2016 limitation | here |
|---|---|
| waker stacks limited to 7 frames (stack hack, `MAX_BPF_STACK`) | `bpf_get_stackid` + stack-trace maps, up to 127 frames |
| kernel stacks only | kernel and user stacks for every level |
| only the last chain of wakeups | still the latest chain per sleep, but up to 8 levels, aggregated per distinct chain |
| all threads shown, some duplication | still shown (each blocked thread's time is real); use `-p/-t/-c` to focus |
| per-thread timestamps/stacks could exhaust a `BPF_HASH` (10240) | task-local storage, freed on exit; chains map sized by `--max-chains` |
| overhead of scheduler tracing | in-kernel aggregation, BTF tracepoints, no per-event output; see below |

## Output

Folded (`-f`), one line per distinct chain, time in microseconds:

```
target;ustack(root→leaf);-;kstack(root→leaf);--;kstack(leaf→root);-;ustack(leaf→root);waker1;--;...;wakerN COUNT
```

Labels: `comm` (or `comm/TID` with `-P`), with suffixes ` [softirq]`,
` [fork]`, ` [thaw]` (the wakee was frozen, so the recorded waker is the
thawer); `[hardirq]`, `[nmi]`, `[softirq]`, `[irq exit]` for interrupt links.
Markers: `[chain truncated]`, `[chain torn]`, `[unknown waker]`. Frames:
`[unknown]`, `[libfoo.so]` (mapping known, no symbol), `[missing kernel stack]`,
`[missing user stack]`. Names that would read as structure are printed in
single quotes: a frame named `-`, `--` or like a marker, and any task name
starting with `[` (so a task can't pose as `[hardirq]`).

The default human-readable output prints the same chains, lightest first so
the heaviest end up at the bottom of the terminal. Within a chain it goes top
down, like the flame graph: the oldest waker first, each waker's user stack
(root to leaf) then kernel stack ending in `try_to_wake_up`, then the blocked
task's kernel stack (leaf to root) and user stack, its name, and its time.

Stats go to stderr, e.g. `sleeps`, `accounted`, `wakeups` (with `irq-`,
`irq-exit-`, `softirq-` breakdowns), `no-waker`, `torn`, `storage-fail`,
`kstack-collide`/`ustack-collide` (stacks lost to stack-map hash collisions:
raise `--stack-storage-size`), `chains-full` (raise `--max-chains`).

## Overhead and tuning

Every wakeup on the system is recorded, because any task may turn out to be
an intermediate waker of a target: one task-storage update and up to two
stack walks per wakeup, with IRQs off inside `try_to_wake_up`. Filters
(`-p`, `-t`, `-c`) only reduce this at `-d 1`. Budget roughly
*wakeups/s × ~1 µs*; check `wakeups=` in the stats. To reduce it: `-K`/`-U`,
a smaller `--perf-max-stack-depth`, `-d 1` with filters. Context switches cost
one task-storage lookup, plus stack walks for tracked sleeps. The interrupt-exit
hooks add an `fentry`/`fexit` pair per hardware interrupt.

Memory: two preallocated stack maps of `--stack-storage-size` ×
(8 × frames + ~24) bytes (≈ 34 MB each by default); the chains map, ≈ 512 B
per chain allocated on demand plus a 16 B × `--max-chains` bucket array
(1 MB by default); and ≈ 700 B of task storage per task that has been woken.

Interrupt links keep the stack id of the whole interrupted stack (it is only
trimmed when printed), so interrupts landing in many different places use up
stack-map entries and chain entries that later merge in the output.

## Caveats

* A chain is the *latest* wakeup history of each waker at the time it issued the
  wakeup, not a full causal graph; a waker that has been running for a long time
  still carries the chain that last woke it.
* Softirqs run from `local_bh_enable()` may be finishing work raised by an
  interrupt that arrived while bottom halves were disabled; such chains are
  kept and are best-effort.
* x86_64 is what was tested (kernel 7.0). arm64 paths exist but are untested.
  PREEMPT_RT (softirq accounting outside `preempt_count`) and proxy-execution
  kernels need more work. The typed ksym `__preempt_count` needs
  `CONFIG_KALLSYMS_ALL=y` (set by major distributions).
* User stacks need frame pointers (Ubuntu 24.04+ and Fedora 38+ build with
  them). Even then, glibc's syscall wrappers (`read`, `write`, `futex`, …)
  don't set up a frame, so the function that called them is usually missing
  from the user stack; the test workload avoids this with its own syscall
  trampoline. Symbols are resolved after tracing ends, so processes that
  exited in the meantime show `[unknown]`. Names are not demangled.
* `[thaw]` covers the PM (suspend) and cgroup v1 freezers. The cgroup v2
  freezer (`docker pause`, `systemctl freeze`) wakes tasks with a signal, so
  the task writing `cgroup.freeze` appears as an ordinary waker.
* With `-d 1` and a target filter, wakers outside the filter are not tracked,
  so `[chain truncated]` can be missing.
* Stack-map collisions drop stacks rather than mixing them up; watch
  `*-collide` in the stats.

## Tests

```bash
make                                      # builds build/chaingraph and build/chainload
sudo tests/run_tests.sh                   # end-to-end on the running kernel (~1 min)
python3 -m unittest tests/test_check_chain.py
gcc -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -Isrc \
    tests/syms_selftest.c src/syms.c -lelf -lz -o build/syms_selftest && build/syms_selftest
```

`tests/chainload.c` builds a known chain: `cg_source` (hrtimer sleep) → pipe →
`cg_relay1` → pipe → `cg_relay2` → pipe → `cg_sink`. The end-to-end tests
check that `cg_sink`'s time is attributed to
`cg_relay2 ← cg_relay1 ← cg_source ← [hardirq]` with the expected kernel
(`anon_pipe_read`, `anon_pipe_write`, `hrtimer_wakeup`) and user
(`cg_stage_*`) frames. They also check that every interrupt link system-wide
is trimmed at its interrupt entry and never labelled as the idle task, and
they cover depth truncation, PID filtering, kernel-only human-readable
output, and SVG rendering.

## Layout

```
src/chaingraph.bpf.c   BPF programs (tp_btf sched_{waking,wakeup_new,switch}, fentry/fexit irq_exit*)
src/chaingraph.h       types shared with userspace
src/chaingraph.c       loader, map walking, rendering
src/syms.{c,h}         kernel and user symbolization
chaingraph-svg.sh      trace + flamegraph.pl --colors=chain
tests/                 workload, checker, end-to-end and unit tests
libbpf/, FlameGraph/   vendored upstream projects (their own licenses)
```
