// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/*
 * hitchtrace prototype: frame-scoped, cause-gated stall attribution.
 *
 * The root thread is whichever thread of the target process hits the frame
 * marker (a uprobe). Between two marks, every off-CPU interval of the root
 * is timed, clipped to the frame window and added to one bucket, so
 * oncpu + runnable + runqueue + blocked == frame_ns on the root.
 *
 * Blocked time is resolved through the wake chain. Every task carries, in
 * task storage, a snapshot of its waker's most recent off-CPU interval, taken
 * when the wakeup happened (links[0]), plus the waker's own chain shifted down
 * (links[1..]). The part of a wait that overlaps the waker's stall is
 * inherited from it; the rest is the waker running on-CPU, i.e. waiting for
 * work rather than a kernel stall.
 *
 * A record crosses to user space only when frame_ns > budget_ns.
 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "hitchtrace.h"

#define EEXIST		17

#define TASK_RUNNING	0x0000
#define TASK_DEAD	0x0080
#define PF_USER_WORKER	0x00004000
#define PF_KTHREAD	0x00200000

#define VM_FAULT_MAJOR	0x000004

/* x86_64 syscall numbers we care about */
#define SYS_read	0
#define SYS_write	1
#define SYS_poll	7
#define SYS_select	23
#define SYS_nanosleep	35
#define SYS_futex	202
#define SYS_epoll_wait	232
#define SYS_clock_nanosleep	230
#define SYS_pselect6	270
#define SYS_ppoll	271
#define SYS_epoll_pwait	281
#define SYS_epoll_pwait2	441
#define SYS_futex_waitv	449
#define SYS_futex_wait	455

#define SOFTIRQ_OFFSET	0x00000100
#define HARDIRQ_MASK	0x000f0000
#define NMI_MASK	0x00f00000

/* Configuration, set by userspace before load (lands in .rodata). */
const volatile __u32 targ_tgid = 0;
const volatile __u64 budget_ns = 16667000;
const volatile __u64 min_stall_ns = 100000;
const volatile __u32 max_hops = HT_MAX_HOPS;
const volatile bool kernel_stacks = true;
const volatile bool user_stacks = false;

/* What we knew about a waker when it issued the wakeup. */
struct hop_link {
	__u64 wake_ts;		/* when this wakeup happened */
	__u64 w_out;		/* waker's last off-CPU interval ... */
	__u64 w_in;		/* ... as of that moment */
	__u32 w_cause;		/* enum ht_cause of that interval */
	__s32 w_kstack;		/* where the waker was blocked, if it was */
	__u32 pid;
	__u32 tgid;
	__u32 flags;		/* HT_HOP_* */
	__u32 __pad;
	char comm[TASK_COMM_LEN];
};

struct task_state {
	/* scheduling, maintained for every task */
	__u64 out_ts;		/* switch-out time; 0 while on-CPU */
	__u64 in_ts;		/* last switch-in time */
	__u64 wake_ts;		/* wakeup since the last switch-in; 0 if none */
	__u32 out_state;	/* prev_state at switch-out (0 = preempted) */
	__u32 out_iowait;	/* in_iowait at switch-out */
	__s32 out_kstack;	/* stacks captured at switch-out */
	__s32 out_ustack;
	__u64 last_out;		/* last completed off-CPU interval ... */
	__u64 last_in;
	__u32 last_cause;	/* ... and what it was */
	__s32 last_kstack;
	__u32 depth;		/* valid entries in links[] */
	__u32 seq;		/* odd while links[] is being rewritten */
	__u32 slot1;		/* per-thread record slot, 1-based; 0 = none */
	__u32 sysclass;		/* HT_SC_*, while inside a syscall */
	__u32 gpu_depth;	/* nested GPU fence waits */
	__u64 fault_start;	/* on-CPU kernel stalls, cumulative */
	__u64 fault_ns;
	__u64 fault_major_ns;
	__u64 reclaim_start;
	__u64 reclaim_ns;
	__u64 compact_start;
	__u64 compact_ns;
	__u64 snap_fault;	/* their values when this frame opened */
	__u64 snap_fault_major;
	__u64 snap_reclaim;
	__u64 snap_compact;
	__u32 preempt_pid;	/* who took the CPU at the last preemption */
	__u32 preempt_tgid;
	char preempt_comm[TASK_COMM_LEN];
	struct hop_link links[HT_MAX_HOPS];

	/* frame accounting, used only by a root thread */
	bool is_root;
	__u64 frame_start;
	__u64 frame_id;
	__u32 nstalls;
	__u32 frame_flags;
	__u64 cause_ns[HT_CAUSE_MAX];
	struct ht_stall worst;
};

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct task_state);
} task_states SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_STACK_TRACE);
	__uint(key_size, sizeof(__u32));
	__uint(value_size, HT_PERF_MAX_STACK * sizeof(__u64));
	__uint(max_entries, 8192);
} kstacks SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_STACK_TRACE);
	__uint(key_size, sizeof(__u32));
	__uint(value_size, HT_PERF_MAX_STACK * sizeof(__u64));
	__uint(max_entries, 4096);
} ustacks SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 8 << 20);
} events SEC(".maps");

/*
 * The frame currently open on the target process. Threads other than the root
 * read it to know which window to clip their intervals to, and which epoch
 * their per-thread slot belongs to.
 */
struct frame_ctx {
	__u64 frame_start;
	__u64 frame_id;
	__u32 epoch;
	__u32 root_pid;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct frame_ctx);
} frame_state SEC(".maps");

/*
 * Per-thread detail for the open frame. Each thread writes its own slot as it
 * is scheduled, so closing a frame is a bounded walk over slots rather than a
 * walk over tasks. Slots are claimed on first sight and never freed: a thread
 * that exits keeps its slot for the run (prototype limitation).
 */
struct thread_slot {
	__u32 epoch;
	__u32 __pad;
	__u64 out_ts;		/* set while the thread is off-CPU */
	__u64 snap_fault;	/* the thread's on-CPU stall counters ... */
	__u64 snap_fault_major;	/* ... when this frame opened, so the frame's */
	__u64 snap_reclaim;	/* share can be carved out of its on-CPU time */
	__u64 snap_compact;
	struct ht_thread th;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, HT_MAX_THREADS);
	__type(key, __u32);
	__type(value, struct thread_slot);
} thread_slots SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u32);
} slot_next SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u32);
} irq_exit_nest SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, HT_STAT_MAX);
	__type(key, __u32);
	__type(value, __u64);
} stats SEC(".maps");

/* Interrupt context, as in chaingraph: the per-CPU preempt count. */
extern const int __preempt_count __ksym __weak;

struct pcpu_hot___ht {
	int preempt_count;
} __attribute__((preserve_access_index));

extern const struct pcpu_hot___ht pcpu_hot __ksym __weak;

static __always_inline int preempt_count(void)
{
#if defined(bpf_target_x86)
	if (bpf_ksym_exists(&__preempt_count))
		return *(int *)bpf_this_cpu_ptr(&__preempt_count);
	if (bpf_ksym_exists(&pcpu_hot) &&
	    bpf_core_field_exists(struct pcpu_hot___ht, preempt_count))
		return ((struct pcpu_hot___ht *)
			bpf_this_cpu_ptr(&pcpu_hot))->preempt_count;
#elif defined(bpf_target_arm64)
	return bpf_get_current_task_btf()->thread_info.preempt.count;
#endif
	return 0;
}

static __always_inline void stat_inc(__u32 idx)
{
	__u64 *v = bpf_map_lookup_elem(&stats, &idx);

	if (v)
		(*v)++;
}

static __always_inline __u32 *irq_exit_depth(void)
{
	__u32 zero = 0;

	return bpf_map_lookup_elem(&irq_exit_nest, &zero);
}

SEC("fentry/irq_exit_rcu")
int BPF_PROG(on_irq_exit_rcu_enter)
{
	__u32 *n = irq_exit_depth();

	if (n)
		(*n)++;
	return 0;
}

SEC("fexit/irq_exit_rcu")
int BPF_PROG(on_irq_exit_rcu_leave)
{
	__u32 *n = irq_exit_depth();

	if (n && *n)
		(*n)--;
	return 0;
}

SEC("fentry/irq_exit")
int BPF_PROG(on_irq_exit_enter)
{
	__u32 *n = irq_exit_depth();

	if (n)
		(*n)++;
	return 0;
}

SEC("fexit/irq_exit")
int BPF_PROG(on_irq_exit_leave)
{
	__u32 *n = irq_exit_depth();

	if (n && *n)
		(*n)--;
	return 0;
}

/* Length of [s, e) clipped to [lo, ...). */
static __always_inline __u64 clip(__u64 s, __u64 e, __u64 lo)
{
	if (e <= s || e <= lo)
		return 0;
	if (s < lo)
		s = lo;
	return e - s;
}

/* Length of [a0, a1) ∩ [b0, b1). */
static __always_inline __u64 overlap(__u64 a0, __u64 a1, __u64 b0, __u64 b1)
{
	__u64 lo = a0 > b0 ? a0 : b0;
	__u64 hi = a1 < b1 ? a1 : b1;

	return hi > lo ? hi - lo : 0;
}

static __always_inline void add_cause(struct task_state *ts, __u32 cause, __u64 ns)
{
	if (!ns || cause >= HT_CAUSE_MAX)
		return;
	ts->cause_ns[cause] += ns;
}

static __always_inline __s32 kernel_stack(void *ctx)
{
	long id;

	if (!kernel_stacks)
		return -1;
	id = bpf_get_stackid(ctx, &kstacks, 0);
	if (id < 0) {
		stat_inc(HT_STAT_STACK_ERR);
		return -1;
	}
	return id;
}

static __always_inline __s32 user_stack(void *ctx, struct task_struct *t)
{
	long id;

	if (!user_stacks || (t->flags & (PF_KTHREAD | PF_USER_WORKER)) || !t->mm)
		return -1;
	id = bpf_get_stackid(ctx, &ustacks, BPF_F_USER_STACK);
	if (id < 0) {
		stat_inc(HT_STAT_STACK_ERR);
		return -1;
	}
	return id;
}

static __always_inline struct frame_ctx *frame_ctx(void)
{
	__u32 zero = 0;

	return bpf_map_lookup_elem(&frame_state, &zero);
}

/* This thread's slot for @epoch, zeroed if it still holds an older frame. */
static __always_inline struct thread_slot *slot_of(struct task_state *ts,
						   struct task_struct *t,
						   __u32 epoch)
{
	struct thread_slot *s;
	__u32 idx, zero = 0, *next;

	if (!ts->slot1) {
		next = bpf_map_lookup_elem(&slot_next, &zero);
		if (!next)
			return NULL;
		idx = __sync_fetch_and_add(next, 1);
		if (idx >= HT_MAX_THREADS) {
			stat_inc(HT_STAT_SLOTS_FULL);
			return NULL;
		}
		ts->slot1 = idx + 1;
	}
	idx = ts->slot1 - 1;
	if (idx >= HT_MAX_THREADS)
		return NULL;
	s = bpf_map_lookup_elem(&thread_slots, &idx);
	if (!s)
		return NULL;
	if (s->epoch != epoch) {
		/* out_ts belongs to the thread, not to the frame: a thread that
		 * is off-CPU stays off-CPU across the boundary */
		__builtin_memset(&s->th, 0, sizeof(s->th));
		s->epoch = epoch;
		s->snap_fault = ts->fault_ns;
		s->snap_fault_major = ts->fault_major_ns;
		s->snap_reclaim = ts->reclaim_ns;
		s->snap_compact = ts->compact_ns;
	}
	s->th.pid = t->pid;
	__builtin_memcpy(s->th.comm, t->comm, TASK_COMM_LEN);
	return s;
}

static __always_inline __u32 sysclass_of(long nr)
{
	switch (nr) {
	case SYS_futex:
	case SYS_futex_waitv:
	case SYS_futex_wait:
		return HT_SC_FUTEX;
	case SYS_poll:
	case SYS_ppoll:
	case SYS_select:
	case SYS_pselect6:
	case SYS_epoll_wait:
	case SYS_epoll_pwait:
	case SYS_epoll_pwait2:
		return HT_SC_POLL;
	case SYS_nanosleep:
	case SYS_clock_nanosleep:
		return HT_SC_SLEEP;
	default:
		return HT_SC_NONE;
	}
}

static __always_inline bool in_target(struct task_struct *t)
{
	return targ_tgid && t->tgid == targ_tgid;
}

/*
 * @p is being woken by the current task. Record what the waker was doing, so
 * that @p can later work out how much of its wait the waker explains.
 */
static __always_inline int record_wakeup(void *ctx, struct task_struct *p)
{
	struct task_struct *waker = bpf_get_current_task_btf();
	struct task_state *ts, *wts = NULL;
	struct hop_link *l;
	__u32 flags = 0, depth = 1;
	bool interrupt;
	int pc;

	if (p->pid == waker->pid)
		return 0;

	ts = bpf_task_storage_get(&task_states, p, NULL,
				  BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!ts) {
		stat_inc(HT_STAT_STORAGE_FAIL);
		return 0;
	}

	pc = preempt_count();
	if (pc & (NMI_MASK | HARDIRQ_MASK)) {
		flags |= HT_HOP_IRQ;
	} else {
		__u32 *nest;

		if (pc & SOFTIRQ_OFFSET)
			flags |= HT_HOP_SOFTIRQ;
		nest = irq_exit_depth();
		if (nest && *nest)
			flags |= HT_HOP_IRQEXIT;
	}
	if (waker->pid == 0)
		flags |= HT_HOP_IDLE;

	/* In interrupt context "current" is only the interrupted task. */
	interrupt = flags & (HT_HOP_IRQ | HT_HOP_IRQEXIT | HT_HOP_IDLE);
	if (!interrupt && max_hops > 1)
		wts = bpf_task_storage_get(&task_states, waker, NULL, 0);

	ts->seq++;
	if (wts && wts->depth) {
		__u32 wseq = wts->seq;

		__builtin_memcpy(&ts->links[1], &wts->links[0],
				 sizeof(struct hop_link) * (HT_MAX_HOPS - 1));
		if (wts->seq != wseq) {	/* rewritten under us */
			__builtin_memset(&ts->links[1], 0,
					 sizeof(struct hop_link) * (HT_MAX_HOPS - 1));
		} else {
			__u32 wdepth = wts->depth;

			if (wdepth > HT_MAX_HOPS - 1)
				wdepth = HT_MAX_HOPS - 1;
			depth = wdepth + 1;
			if (depth > max_hops) {
				depth = max_hops;
				if (depth >= 1 && depth <= HT_MAX_HOPS)
					ts->links[depth - 1].flags |= HT_HOP_TRUNC;
			}
		}
	} else {
		__builtin_memset(&ts->links[1], 0,
				 sizeof(struct hop_link) * (HT_MAX_HOPS - 1));
	}

	l = &ts->links[0];
	__builtin_memset(l, 0, sizeof(*l));
	l->wake_ts = bpf_ktime_get_ns();
	l->flags = flags;
	if (!interrupt) {
		l->pid = waker->pid;
		l->tgid = waker->tgid;
		bpf_get_current_comm(l->comm, sizeof(l->comm));
		if (wts) {
			l->w_out = wts->last_out;
			l->w_in = wts->last_in;
			l->w_cause = wts->last_cause;
			l->w_kstack = wts->last_kstack;
		} else {
			l->w_kstack = -1;
		}
	} else {
		l->w_kstack = -1;
	}
	ts->depth = depth;
	ts->seq++;
	ts->wake_ts = l->wake_ts;
	return 0;
}

/*
 * Which syscall a thread is inside, so a blocked interval can be named by
 * what the thread asked for rather than by who happened to wake it.
 */
SEC("raw_tp/sys_enter")
int BPF_PROG(on_sys_enter, struct pt_regs *regs, long id)
{
	struct task_struct *cur = bpf_get_current_task_btf();
	struct task_state *ts;

	if (!in_target(cur))
		return 0;
	ts = bpf_task_storage_get(&task_states, cur, NULL,
				  BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (ts)
		ts->sysclass = sysclass_of(id);
	return 0;
}

SEC("raw_tp/sys_exit")
int BPF_PROG(on_sys_exit, struct pt_regs *regs, long ret)
{
	struct task_struct *cur = bpf_get_current_task_btf();
	struct task_state *ts;

	if (!in_target(cur))
		return 0;
	ts = bpf_task_storage_get(&task_states, cur, NULL, 0);
	if (ts)
		ts->sysclass = HT_SC_NONE;
	return 0;
}

/* GPU fence waits. RADV waits through the syncobj ioctls, not dma_fence. */
static __always_inline int gpu_enter(void)
{
	struct task_struct *cur = bpf_get_current_task_btf();
	struct task_state *ts;

	if (!in_target(cur))
		return 0;
	ts = bpf_task_storage_get(&task_states, cur, NULL,
				  BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (ts)
		ts->gpu_depth++;
	return 0;
}

static __always_inline int gpu_leave(void)
{
	struct task_struct *cur = bpf_get_current_task_btf();
	struct task_state *ts;

	if (!in_target(cur))
		return 0;
	ts = bpf_task_storage_get(&task_states, cur, NULL, 0);
	if (ts && ts->gpu_depth)
		ts->gpu_depth--;
	return 0;
}

SEC("fentry/dma_fence_wait_timeout")
int BPF_PROG(on_fence_enter) { return gpu_enter(); }

SEC("fexit/dma_fence_wait_timeout")
int BPF_PROG(on_fence_leave) { return gpu_leave(); }

SEC("fentry/drm_syncobj_wait_ioctl")
int BPF_PROG(on_syncobj_enter) { return gpu_enter(); }

SEC("fexit/drm_syncobj_wait_ioctl")
int BPF_PROG(on_syncobj_leave) { return gpu_leave(); }

SEC("fentry/drm_syncobj_timeline_wait_ioctl")
int BPF_PROG(on_syncobj_tl_enter) { return gpu_enter(); }

SEC("fexit/drm_syncobj_timeline_wait_ioctl")
int BPF_PROG(on_syncobj_tl_leave) { return gpu_leave(); }

/* On-CPU kernel stalls: page faults, reclaim, compaction. */
SEC("fentry/handle_mm_fault")
int BPF_PROG(on_fault_enter, struct vm_area_struct *vma, unsigned long address,
	     unsigned int flags, struct pt_regs *regs)
{
	struct task_struct *cur = bpf_get_current_task_btf();
	struct task_state *ts;

	if (!in_target(cur))
		return 0;
	ts = bpf_task_storage_get(&task_states, cur, NULL,
				  BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (ts && !ts->fault_start)
		ts->fault_start = bpf_ktime_get_ns();
	return 0;
}

SEC("fexit/handle_mm_fault")
int BPF_PROG(on_fault_leave, struct vm_area_struct *vma, unsigned long address,
	     unsigned int flags, struct pt_regs *regs, unsigned int ret)
{
	struct task_struct *cur = bpf_get_current_task_btf();
	struct task_state *ts;
	__u64 delta;

	if (!in_target(cur))
		return 0;
	ts = bpf_task_storage_get(&task_states, cur, NULL, 0);
	if (!ts || !ts->fault_start)
		return 0;
	delta = bpf_ktime_get_ns() - ts->fault_start;
	ts->fault_start = 0;
	if (ret & VM_FAULT_MAJOR)
		ts->fault_major_ns += delta;
	else
		ts->fault_ns += delta;
	return 0;
}

static __always_inline int reclaim_enter(void)
{
	struct task_struct *cur = bpf_get_current_task_btf();
	struct task_state *ts;

	if (!in_target(cur))
		return 0;
	ts = bpf_task_storage_get(&task_states, cur, NULL,
				  BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (ts && !ts->reclaim_start)
		ts->reclaim_start = bpf_ktime_get_ns();
	return 0;
}

static __always_inline int reclaim_leave(void)
{
	struct task_struct *cur = bpf_get_current_task_btf();
	struct task_state *ts;

	if (!in_target(cur))
		return 0;
	ts = bpf_task_storage_get(&task_states, cur, NULL, 0);
	if (ts && ts->reclaim_start) {
		ts->reclaim_ns += bpf_ktime_get_ns() - ts->reclaim_start;
		ts->reclaim_start = 0;
	}
	return 0;
}

SEC("tp_btf/mm_vmscan_direct_reclaim_begin")
int BPF_PROG(on_reclaim_begin) { return reclaim_enter(); }

SEC("tp_btf/mm_vmscan_direct_reclaim_end")
int BPF_PROG(on_reclaim_end) { return reclaim_leave(); }

/* MemoryMax/memory.high reclaim goes through the memcg tracepoints instead */
SEC("tp_btf/mm_vmscan_memcg_reclaim_begin")
int BPF_PROG(on_memcg_reclaim_begin) { return reclaim_enter(); }

SEC("tp_btf/mm_vmscan_memcg_reclaim_end")
int BPF_PROG(on_memcg_reclaim_end) { return reclaim_leave(); }

SEC("tp_btf/mm_compaction_begin")
int BPF_PROG(on_compact_begin)
{
	struct task_struct *cur = bpf_get_current_task_btf();
	struct task_state *ts;

	if (!in_target(cur))
		return 0;
	ts = bpf_task_storage_get(&task_states, cur, NULL,
				  BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (ts && !ts->compact_start)
		ts->compact_start = bpf_ktime_get_ns();
	return 0;
}

SEC("tp_btf/mm_compaction_end")
int BPF_PROG(on_compact_end)
{
	struct task_struct *cur = bpf_get_current_task_btf();
	struct task_state *ts;

	if (!in_target(cur))
		return 0;
	ts = bpf_task_storage_get(&task_states, cur, NULL, 0);
	if (ts && ts->compact_start) {
		ts->compact_ns += bpf_ktime_get_ns() - ts->compact_start;
		ts->compact_start = 0;
	}
	return 0;
}

SEC("tp_btf/sched_waking")
int BPF_PROG(on_waking, struct task_struct *p)
{
	return record_wakeup(ctx, p);
}

SEC("tp_btf/sched_wakeup_new")
int BPF_PROG(on_wakeup_new, struct task_struct *p)
{
	return record_wakeup(ctx, p);
}

/*
 * What a blocked interval was waiting for. What the thread asked the kernel
 * for beats who happened to wake it: a futex wait is a futex wait even when
 * the waker is a timer interrupt firing its timeout.
 */
static __always_inline __u32 blocked_cause(struct task_state *ts, bool irq_waker,
					   bool woken)
{
	if (ts->gpu_depth)
		return HT_BLOCK_GPU;
	if (ts->out_iowait)
		return HT_BLOCK_IO;
	switch (ts->sysclass) {
	case HT_SC_FUTEX:
		return HT_BLOCK_FUTEX;
	case HT_SC_POLL:
		return HT_BLOCK_POLL;
	case HT_SC_SLEEP:
		return HT_BLOCK_TIMER;
	default:
		break;
	}
	if (irq_waker)
		return HT_BLOCK_TIMER;
	if (woken)
		return HT_BLOCK_TASK;
	return HT_BLOCK_OTHER;
}

/* Move on-CPU time that was really a kernel stall into its own bucket. */
static __always_inline void carve(__u64 *buckets, __u32 bucket, __u64 ns)
{
	__u64 on = buckets[HT_ONCPU];

	if (!ns || bucket >= HT_CAUSE_PARTITION)
		return;
	if (ns > on)
		ns = on;
	buckets[HT_ONCPU] = on - ns;
	buckets[bucket] += ns;
}

static __always_inline void carve_oncpu(struct task_state *ts, __u32 bucket,
					__u64 ns)
{
	carve(ts->cause_ns, bucket, ns);
}

/*
 * Resolve a blocked interval [lo, hi) of the root through its wake chain and
 * record it as the frame's worst stall if it is the longest so far.
 */
static __always_inline void resolve(struct task_state *ts, __u64 lo, __u64 hi,
				    __u32 cause, __u64 blocked_ns, bool resolvable)
{
	struct ht_stall *w = &ts->worst;
	__u64 prev_out = lo, prev_in = hi, inherited = 0;
	__u32 i, nhops = 0;

	/*
	 * links[0] is the waker; how much of the wait does its own stall cover?
	 * Only a wait ended by another task splits this way: an I/O or timer
	 * wait was not spent waiting for a thread to get around to us.
	 */
	inherited = overlap(lo, hi, ts->links[0].w_out, ts->links[0].w_in);
	if (ts->links[0].flags & (HT_HOP_IRQ | HT_HOP_IRQEXIT | HT_HOP_IDLE))
		inherited = 0;
	if (resolvable) {
		add_cause(ts, HT_RESOLVED_INHERITED, inherited);
		add_cause(ts, HT_RESOLVED_WAIT_ONCPU,
			  blocked_ns > inherited ? blocked_ns - inherited : 0);
		stat_inc(inherited ? HT_STAT_RESOLVED : HT_STAT_UNRESOLVED);
	}

	if (blocked_ns <= w->ns || blocked_ns < min_stall_ns)
		return;

	__builtin_memset(w, 0, sizeof(*w));
	w->ns = blocked_ns;
	w->start_ns = lo;
	w->cause = cause;
	w->kstack_id = ts->out_kstack;
	w->ustack_id = ts->out_ustack;

	for (i = 0; i < HT_MAX_HOPS; i++) {
		struct hop_link *l = &ts->links[i];
		struct ht_hop *h = &w->hops[i];
		__u64 ns;

		if (i >= max_hops || i >= ts->depth)
			break;
		if (!l->flags && !l->pid)
			break;

		ns = overlap(prev_out, prev_in, l->w_out, l->w_in);
		if (l->flags & (HT_HOP_IRQ | HT_HOP_IRQEXIT | HT_HOP_IDLE))
			ns = 0;

		h->pid = l->pid;
		h->tgid = l->tgid;
		__builtin_memcpy(h->comm, l->comm, TASK_COMM_LEN);
		h->ns = ns;
		h->cause = l->w_cause;
		h->kstack_id = l->w_kstack;
		h->flags = l->flags;
		if (!ns)
			h->flags |= HT_HOP_ONCPU;
		if (l->w_in && l->w_in < prev_out)
			h->flags |= HT_HOP_STALE;
		nhops = i + 1;

		/* an interrupt ends the chain */
		if (l->flags & (HT_HOP_IRQ | HT_HOP_IRQEXIT | HT_HOP_IDLE))
			break;
		/* nothing left to explain: the waker was running */
		if (!ns)
			break;
		prev_out = l->w_out;
		prev_in = l->w_in;
	}
	w->nhops = nhops;
}

SEC("tp_btf/sched_switch")
int BPF_PROG(on_switch, bool preempt, struct task_struct *prev,
	     struct task_struct *next, unsigned int prev_state)
{
	struct task_state *ts;
	__u64 now = bpf_ktime_get_ns();
	bool track_prev = in_target(prev), track_next = in_target(next);
	bool irq_waker, woken, resolvable;
	__u64 lo, hi, blocked_ns;
	__u32 cause;

	/* prev leaves the CPU */
	if (prev->pid) {
		ts = bpf_task_storage_get(&task_states, prev, NULL,
					  BPF_LOCAL_STORAGE_GET_F_CREATE);
		if (!ts) {
			stat_inc(HT_STAT_STORAGE_FAIL);
		} else if (!(prev_state & TASK_DEAD)) {
			if (ts->is_root && ts->frame_start && ts->in_ts)
				add_cause(ts, HT_ONCPU,
					  clip(ts->in_ts, now, ts->frame_start));
			if (track_prev) {
				struct frame_ctx *fc = frame_ctx();

				if (fc && fc->frame_start) {
					struct thread_slot *sl =
						slot_of(ts, prev, fc->epoch);

					if (sl) {
						if (ts->in_ts)
							sl->th.cause_ns[HT_ONCPU] +=
								clip(ts->in_ts, now,
								     fc->frame_start);
						/* split off the on-CPU kernel stalls
						 * taken since the last update */
						carve(sl->th.cause_ns,
						      HT_ONCPU_FAULT_MAJOR,
						      ts->fault_major_ns -
						      sl->snap_fault_major);
						carve(sl->th.cause_ns, HT_ONCPU_FAULT,
						      ts->fault_ns - sl->snap_fault);
						carve(sl->th.cause_ns, HT_ONCPU_RECLAIM,
						      ts->reclaim_ns - sl->snap_reclaim);
						carve(sl->th.cause_ns, HT_ONCPU_COMPACT,
						      ts->compact_ns - sl->snap_compact);
						sl->snap_fault = ts->fault_ns;
						sl->snap_fault_major = ts->fault_major_ns;
						sl->snap_reclaim = ts->reclaim_ns;
						sl->snap_compact = ts->compact_ns;
						sl->out_ts = now;
					}
				}
				/* who took the CPU, for the runnable bucket */
				if (preempt || prev_state == TASK_RUNNING) {
					ts->preempt_pid = next->pid;
					ts->preempt_tgid = next->tgid;
					__builtin_memcpy(ts->preempt_comm,
							 next->comm, TASK_COMM_LEN);
				}
			}
			ts->out_ts = now;
			ts->out_state = preempt ? TASK_RUNNING : prev_state;
			ts->out_iowait = BPF_CORE_READ_BITFIELD_PROBED(prev, in_iowait);
			if (track_prev && prev_state != TASK_RUNNING && !preempt) {
				ts->out_kstack = kernel_stack(ctx);
				ts->out_ustack = user_stack(ctx, prev);
			} else {
				ts->out_kstack = -1;
				ts->out_ustack = -1;
			}
		}
	}

	/* next returns to the CPU */
	ts = bpf_task_storage_get(&task_states, next, NULL, 0);
	if (!ts)
		return 0;
	if (!ts->out_ts) {
		ts->in_ts = now;
		return 0;
	}

	lo = ts->out_ts;
	hi = now;
	irq_waker = ts->links[0].flags & (HT_HOP_IRQ | HT_HOP_IRQEXIT | HT_HOP_IDLE);
	woken = ts->wake_ts != 0;
	if (ts->out_state == TASK_RUNNING) {
		cause = HT_RUNNABLE;
	} else {
		if (ts->wake_ts > lo && ts->wake_ts < now)
			hi = ts->wake_ts;	/* the rest is runqueue wait */
		cause = blocked_cause(ts, irq_waker, woken);
	}
	resolvable = cause != HT_RUNNABLE && woken && !irq_waker;

	/* hand this interval to whoever this task wakes next */
	ts->last_out = lo;
	ts->last_in = now;
	ts->last_cause = cause;
	ts->last_kstack = ts->out_kstack;

	if (track_next) {
		struct frame_ctx *fc = frame_ctx();

		if (fc && fc->frame_start) {
			struct thread_slot *sl = slot_of(ts, next, fc->epoch);

			if (sl) {
				__u64 b = clip(lo, hi, fc->frame_start);
				__u64 rq = clip(hi, now, fc->frame_start);

				if (cause < HT_CAUSE_PARTITION)
					sl->th.cause_ns[cause] += b;
				sl->th.cause_ns[HT_RUNQUEUE] += rq;
				sl->out_ts = 0;
				if (b + rq > sl->th.largest_ns) {
					sl->th.largest_ns = b + rq;
					sl->th.largest_cause = cause;
					sl->th.largest_kstack = ts->out_kstack;
				}
				if (cause == HT_RUNNABLE && ts->preempt_pid &&
				    b + rq > sl->th.preemptor_ns) {
					sl->th.preemptor_ns = b + rq;
					sl->th.preemptor_pid = ts->preempt_pid;
					sl->th.preemptor_tgid = ts->preempt_tgid;
					__builtin_memcpy(sl->th.preemptor_comm,
							 ts->preempt_comm,
							 TASK_COMM_LEN);
				}
			}
		}
	}

	if (ts->is_root && ts->frame_start) {
		blocked_ns = clip(lo, hi, ts->frame_start);
		add_cause(ts, cause, blocked_ns);
		add_cause(ts, HT_RUNQUEUE, clip(hi, now, ts->frame_start));
		ts->nstalls++;
		stat_inc(HT_STAT_STALLS);
		if (cause != HT_RUNNABLE && blocked_ns)
			resolve(ts, lo > ts->frame_start ? lo : ts->frame_start,
				hi, cause, blocked_ns, resolvable);
	}

	ts->out_ts = 0;
	ts->wake_ts = 0;
	ts->in_ts = now;
	return 0;
}

/*
 * The frame marker. Closes the frame that was open on this thread, emits it
 * if it went over budget, and opens the next one.
 */
SEC("uprobe")
int BPF_UPROBE(on_frame_mark, unsigned long frame_id)
{
	struct task_struct *cur = bpf_get_current_task_btf();
	__u64 now = bpf_ktime_get_ns(), frame_ns;
	struct frame_ctx *fc = frame_ctx();
	struct task_state *ts;
	struct ht_record *rec;
	__u32 i, nthreads = 0;

	if (!in_target(cur))
		return 0;
	ts = bpf_task_storage_get(&task_states, cur, NULL,
				  BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!ts) {
		stat_inc(HT_STAT_STORAGE_FAIL);
		return 0;
	}

	if (ts->frame_start && ts->frame_start < now) {
		/* the root is running right now: close its on-CPU tail */
		if (ts->in_ts)
			add_cause(ts, HT_ONCPU, clip(ts->in_ts, now, ts->frame_start));
		else
			ts->frame_flags |= HT_FRAME_NO_ROOT_STATE;
		carve_oncpu(ts, HT_ONCPU_FAULT_MAJOR,
			    ts->fault_major_ns - ts->snap_fault_major);
		carve_oncpu(ts, HT_ONCPU_FAULT, ts->fault_ns - ts->snap_fault);
		carve_oncpu(ts, HT_ONCPU_RECLAIM, ts->reclaim_ns - ts->snap_reclaim);
		carve_oncpu(ts, HT_ONCPU_COMPACT, ts->compact_ns - ts->snap_compact);
		frame_ns = now - ts->frame_start;
		stat_inc(HT_STAT_FRAMES);

		if (frame_ns > budget_ns) {
			stat_inc(HT_STAT_HITCHES);
			rec = bpf_ringbuf_reserve(&events, sizeof(*rec), 0);
			if (!rec) {
				stat_inc(HT_STAT_RINGBUF_FULL);
			} else {
				rec->frame_id = frame_id;
				rec->frame_start_ns = ts->frame_start;
				rec->frame_end_ns = now;
				rec->frame_ns = frame_ns;
				rec->budget_ns = budget_ns;
				__builtin_memcpy(rec->cause_ns, ts->cause_ns,
						 sizeof(rec->cause_ns));
				__builtin_memcpy(&rec->worst, &ts->worst,
						 sizeof(rec->worst));
				rec->root_pid = cur->pid;
				rec->root_tgid = cur->tgid;
				__builtin_memcpy(rec->root_comm, cur->comm,
						 TASK_COMM_LEN);
				rec->nstalls = ts->nstalls;
				rec->flags = ts->frame_flags;

				/*
				 * Per-thread detail: each thread of the process
				 * wrote its own slot as it was scheduled. A
				 * thread still off-CPU now never got to close
				 * its last interval, so do it here.
				 */
				for (i = 0; i < HT_MAX_THREADS; i++) {
					struct thread_slot *sl =
						bpf_map_lookup_elem(&thread_slots, &i);

					if (!sl || !fc || !sl->th.pid)
						continue;
					if (sl->epoch != fc->epoch) {
						/* never scheduled this frame: only
						 * interesting if it is still blocked */
						if (!sl->out_ts)
							continue;
						__builtin_memset(sl->th.cause_ns, 0,
								 sizeof(sl->th.cause_ns));
						sl->th.largest_ns = 0;
						sl->th.preemptor_ns = 0;
						sl->th.open_ns = 0;
						sl->th.flags = 0;
						sl->epoch = fc->epoch;
					}
					if (sl->out_ts) {
						sl->th.open_ns =
							clip(sl->out_ts, now,
							     ts->frame_start);
						sl->th.flags |= HT_THREAD_BLOCKED_END;
						rec->flags |= HT_FRAME_OPEN_STALL;
					}
					if (sl->th.pid == (__u32)cur->pid) {
						sl->th.flags |= HT_THREAD_ROOT;
						if (ts->in_ts)
							sl->th.cause_ns[HT_ONCPU] +=
								clip(ts->in_ts, now,
								     ts->frame_start);
						/* same split the record just got */
						carve(sl->th.cause_ns,
						      HT_ONCPU_FAULT_MAJOR,
						      ts->fault_major_ns -
						      sl->snap_fault_major);
						carve(sl->th.cause_ns, HT_ONCPU_FAULT,
						      ts->fault_ns - sl->snap_fault);
						carve(sl->th.cause_ns, HT_ONCPU_RECLAIM,
						      ts->reclaim_ns - sl->snap_reclaim);
						carve(sl->th.cause_ns, HT_ONCPU_COMPACT,
						      ts->compact_ns - sl->snap_compact);
					}
					if (nthreads < HT_MAX_THREADS) {
						__builtin_memcpy(&rec->threads[nthreads],
								 &sl->th,
								 sizeof(struct ht_thread));
						nthreads++;
					}
				}
				rec->nthreads = nthreads;
				rec->__pad = 0;
				/* more threads ran than there are slots */
				i = 0;
				{
					__u32 *used = bpf_map_lookup_elem(&slot_next, &i);

					if (used && *used > HT_MAX_THREADS)
						rec->flags |= HT_FRAME_THREADS_FULL;
				}
				bpf_ringbuf_submit(rec, 0);
				stat_inc(HT_STAT_EMITTED);
			}
		}
	}

	/* open the next frame, and publish it to the other threads */
	if (fc) {
		fc->frame_start = now;
		fc->frame_id = frame_id;
		fc->root_pid = cur->pid;
		fc->epoch++;
	}
	ts->is_root = true;
	ts->frame_start = now;
	ts->frame_id = frame_id;
	ts->snap_fault = ts->fault_ns;
	ts->snap_fault_major = ts->fault_major_ns;
	ts->snap_reclaim = ts->reclaim_ns;
	ts->snap_compact = ts->compact_ns;
	ts->nstalls = 0;
	ts->frame_flags = 0;
	ts->in_ts = now;
	__builtin_memset(ts->cause_ns, 0, sizeof(ts->cause_ns));
	__builtin_memset(&ts->worst, 0, sizeof(ts->worst));
	return 0;
}

char LICENSE[] SEC("license") = "Dual BSD/GPL";
