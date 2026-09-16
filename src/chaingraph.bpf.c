// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/*
 * chaingraph: off-CPU time annotated with the full chain of wakeups that
 * ended each sleep.
 *
 * Every task carries, in BPF task-local storage, the chain of wakers that
 * led to its most recent wakeup: links[0] woke it, links[1] woke links[0],
 * and so on. When task W wakes task T (tp_btf/sched_waking runs in W's
 * context), T's chain becomes W's current stacks followed by W's own chain,
 * shifted down one level. Wakeups issued from interrupt context end the
 * chain there: the interrupted task did not cause the wakeup.
 *
 * tp_btf/sched_switch records when and where a tracked task blocks, and when
 * it is switched back in, adds the off-CPU time to an in-kernel hash map
 * keyed by (target, off-CPU stacks, chain).
 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "chaingraph.h"

#define E2BIG		7
#define EEXIST		17

#define TASK_RUNNING		0x0000
#define TASK_INTERRUPTIBLE	0x0001
#define TASK_UNINTERRUPTIBLE	0x0002
#define TASK_DEAD		0x0080
#define TASK_NOLOAD		0x0400
#define TASK_RTLOCK_WAIT	0x1000
#define TASK_FROZEN		0x8000
#define PF_USER_WORKER		0x00004000
#define PF_KTHREAD		0x00200000

#define SOFTIRQ_OFFSET	0x00000100
#define HARDIRQ_MASK	0x000f0000
#define NMI_MASK	0x00f00000

/* Configuration, set by userspace before load (lands in .rodata). */
const volatile pid_t targ_tgid = 0;
const volatile pid_t targ_pid = 0;
const volatile bool filter_comm = false;
const volatile char targ_comm[TASK_COMM_LEN] = {};
const volatile bool user_threads_only = false;
const volatile bool kernel_threads_only = false;
const volatile __u32 state_mask = 0;		/* CG_STATE_*; 0: any sleep */
const volatile __u64 min_block_ns = 1;
const volatile __u64 max_block_ns = ~0ULL;
const volatile bool kernel_stacks = true;
const volatile bool user_stacks = true;
const volatile bool per_thread = false;
const volatile __u32 max_depth = 4;		/* 1..MAX_CHAIN_DEPTH */
const volatile __u32 softirq_mode = CG_SOFTIRQ_AUTO;

struct task_state {
	__u64 offcpu_ts;	/* when a tracked sleep began; 0 if none */
	__s32 kstack_id;	/* off-CPU stacks captured at switch-out */
	__s32 ustack_id;
	__u32 woken;		/* a wakeup happened since the last switch-in */
	__u32 depth;		/* valid entries in links[] */
	__u32 seq;		/* odd while links[] is being rewritten */
	__u32 __pad;
	struct chain_link links[MAX_CHAIN_DEPTH];
};

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct task_state);
} task_states SEC(".maps");

/* Separate maps so kernel and user stacks don't compete for buckets. */
struct {
	__uint(type, BPF_MAP_TYPE_STACK_TRACE);
	__uint(key_size, sizeof(__u32));
	__uint(value_size, PERF_MAX_STACK_DEPTH * sizeof(__u64));
	__uint(max_entries, 16384);
} kstacks SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_STACK_TRACE);
	__uint(key_size, sizeof(__u32));
	__uint(value_size, PERF_MAX_STACK_DEPTH * sizeof(__u64));
	__uint(max_entries, 16384);
} ustacks SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, struct chain_key);
	__type(value, struct chain_val);
	__uint(max_entries, 65536);
} chains SEC(".maps");

/* chain_key is too large for the 512-byte BPF stack. */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct chain_key);
} key_scratch SEC(".maps");

/* Depth of irq_exit_rcu()/irq_exit() calls in progress on this CPU. */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u32);
} irq_exit_nest SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, STAT_MAX);
	__type(key, __u32);
	__type(value, __u64);
} stats SEC(".maps");

/*
 * Interrupt-context detection. On x86 the preempt count is the per-CPU
 * variable __preempt_count, except between v6.2 and v6.14 where it lived in
 * struct pcpu_hot. Both are weak typed ksyms: whichever the running kernel
 * lacks resolves to NULL and its branch is pruned by the verifier.
 */
extern const int __preempt_count __ksym __weak;

struct pcpu_hot___cg {
	int preempt_count;
} __attribute__((preserve_access_index));

extern const struct pcpu_hot___cg pcpu_hot __ksym __weak;

static __always_inline int preempt_count(void)
{
#if defined(bpf_target_x86)
	if (bpf_ksym_exists(&__preempt_count))
		return *(int *)bpf_this_cpu_ptr(&__preempt_count);
	if (bpf_ksym_exists(&pcpu_hot) &&
	    bpf_core_field_exists(struct pcpu_hot___cg, preempt_count))
		return ((struct pcpu_hot___cg *)
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

/*
 * Softirqs raised by a hardirq usually run inside irq_exit_rcu() with the
 * hardirq bits already cleared, on behalf of whatever task was interrupted.
 * Threaded-irq kernels wake ksoftirqd/ktimerd from the same spot. Userspace
 * attaches these only where the functions are traceable.
 */
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

static __always_inline bool target_matches(struct task_struct *t)
{
	char comm[TASK_COMM_LEN];

	if (t->pid == 0)
		return false;
	if (targ_tgid && t->tgid != targ_tgid)
		return false;
	if (targ_pid && t->pid != targ_pid)
		return false;
	if (user_threads_only && (t->flags & PF_KTHREAD))
		return false;
	if (kernel_threads_only && !(t->flags & PF_KTHREAD))
		return false;
	if (filter_comm) {
		__builtin_memcpy(comm, t->comm, TASK_COMM_LEN);
		if (bpf_strncmp(comm, TASK_COMM_LEN, (const char *)targ_comm))
			return false;
	}
	return true;
}

/* Same letters as ps(1): frozen and rtlock sleeps report as D there too. */
static __always_inline __u32 state_class(unsigned int state)
{
	if (state & TASK_INTERRUPTIBLE)
		return CG_STATE_S;
	if (state & TASK_UNINTERRUPTIBLE)
		return (state & TASK_NOLOAD) ? CG_STATE_I : CG_STATE_D;
	if (state & (TASK_RTLOCK_WAIT | TASK_FROZEN))
		return CG_STATE_D;
	return CG_STATE_OTHER;
}

static __always_inline __s32 kernel_stack(void *ctx)
{
	long id;

	if (!kernel_stacks)
		return -1;
	id = bpf_get_stackid(ctx, &kstacks, 0);
	if (id == -EEXIST)
		stat_inc(STAT_KSTACK_COLLIDE);
	else if (id < 0)
		stat_inc(STAT_KSTACK_ERR);
	return id;
}

/* User stack of the current task; @t must be current. */
static __always_inline __s32 user_stack(void *ctx, struct task_struct *t)
{
	long id;

	/* io_uring/vhost workers and exiting tasks have no user regs */
	if (!user_stacks || (t->flags & (PF_KTHREAD | PF_USER_WORKER)) || !t->mm)
		return -1;
	id = bpf_get_stackid(ctx, &ustacks, BPF_F_USER_STACK);
	if (id == -EEXIST)
		stat_inc(STAT_USTACK_COLLIDE);
	else if (id < 0)
		stat_inc(STAT_USTACK_ERR);
	return id;
}

/*
 * @p is being woken (or started, for fork) by the current task. Rebuild p's
 * chain as: current task, then current task's own chain. Callers hold
 * p->pi_lock, so p's chain has a single writer; seq lets the lock-free
 * readers (tasks that p wakes) detect a concurrent rewrite.
 */
static __always_inline int record_wakeup(void *ctx, struct task_struct *p,
					 bool fork)
{
	struct task_struct *waker = bpf_get_current_task_btf();
	struct task_state *ts, *wts = NULL;
	struct chain_link *l;
	__u32 flags = LINK_VALID, depth = 1, wdepth = 0, wseq = 0;
	__u32 maxd = max_depth, *nest;
	bool interrupted, cut;
	int pc;

	if (maxd < 1)
		maxd = 1;
	if (maxd > MAX_CHAIN_DEPTH)
		maxd = MAX_CHAIN_DEPTH;

	if (p->pid == waker->pid)
		return 0;
	/* With a single level, only a target's own chain is ever read. */
	if (maxd == 1 && !target_matches(p))
		return 0;

	ts = bpf_task_storage_get(&task_states, p, NULL,
				  BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!ts) {
		stat_inc(STAT_STORAGE_FAIL);
		return 0;
	}

	pc = preempt_count();
	if (pc & NMI_MASK) {
		flags |= LINK_NMI;
	} else if (pc & HARDIRQ_MASK) {
		flags |= LINK_HARDIRQ;
	} else {
		if (pc & SOFTIRQ_OFFSET)
			flags |= LINK_SOFTIRQ;
		nest = irq_exit_depth();
		if (nest && *nest)
			flags |= LINK_IRQEXIT;
	}
	if (fork)
		flags |= LINK_FORK;
	if (waker->pid == 0)
		flags |= LINK_IDLE;
	if (p->__state & TASK_FROZEN)
		flags |= LINK_THAW;

	/*
	 * Continue through the waker's own chain only if it caused this
	 * wakeup. In hardirq/NMI context, or on the way out of an interrupt,
	 * "current" is merely the interrupted task (often an idle one). Other
	 * softirqs usually run for the current task (local_bh_enable(),
	 * ksoftirqd) and are kept unless --softirq=cut. An idle task waking
	 * something in process context ends the chain too, but as itself.
	 */
	interrupted = (flags & (LINK_NMI | LINK_HARDIRQ)) ||
		      ((flags & LINK_IRQEXIT) &&
		       !((flags & LINK_SOFTIRQ) && softirq_mode == CG_SOFTIRQ_KEEP)) ||
		      ((flags & LINK_SOFTIRQ) && softirq_mode == CG_SOFTIRQ_CUT);
	cut = interrupted || (flags & LINK_IDLE);

	if (!cut)
		wts = bpf_task_storage_get(&task_states, waker, NULL, 0);

	ts->seq++;
	if (wts) {
		wseq = wts->seq;
		wdepth = wts->depth;
	}
	if (wts && wdepth && maxd == 1) {
		/* no room for the waker's history; just say it exists */
		__builtin_memset(&ts->links[1], 0,
				 sizeof(struct chain_link) * (MAX_CHAIN_DEPTH - 1));
		flags |= LINK_TRUNC;
	} else if (wts && wdepth && !(wseq & 1)) {
		/* links[1..] = waker's links[0..]; beyond its depth are zeros */
		__builtin_memcpy(&ts->links[1], &wts->links[0],
				 sizeof(struct chain_link) * (MAX_CHAIN_DEPTH - 1));
		if (wts->seq != wseq) {
			__builtin_memset(&ts->links[1], 0,
					 sizeof(struct chain_link) * (MAX_CHAIN_DEPTH - 1));
			flags |= LINK_TORN;
			stat_inc(STAT_TORN);
		} else {
			if (wdepth > maxd)
				wdepth = maxd;
			depth = wdepth + 1;
			if (depth > maxd) {
				depth = maxd;
				if (depth < MAX_CHAIN_DEPTH)
					__builtin_memset(&ts->links[depth], 0,
							 sizeof(struct chain_link));
				ts->links[depth - 1].flags |= LINK_TRUNC;
			}
		}
	} else {
		__builtin_memset(&ts->links[1], 0,
				 sizeof(struct chain_link) * (MAX_CHAIN_DEPTH - 1));
		if (wts && (wseq & 1)) {
			flags |= LINK_TORN;
			stat_inc(STAT_TORN);
		}
	}

	l = &ts->links[0];
	l->flags = flags;
	l->__pad = 0;
	l->kstack_id = kernel_stack(ctx);
	if (interrupted) {
		/* an interrupted task's identity would only split the key */
		l->pid = 0;
		l->tgid = 0;
		l->ustack_id = -1;
		__builtin_memset(l->comm, 0, sizeof(l->comm));
	} else {
		bpf_get_current_comm(l->comm, sizeof(l->comm));
		l->pid = per_thread ? waker->pid : 0;
		l->ustack_id = cut ? -1 : user_stack(ctx, waker);
		l->tgid = (per_thread || l->ustack_id >= 0) ? waker->tgid : 0;
	}
	ts->depth = depth;
	ts->seq++;

	if (fork) {
		stat_inc(STAT_FORKS);
	} else {
		ts->woken = 1;
		stat_inc(STAT_WAKEUPS);
		if (flags & (LINK_NMI | LINK_HARDIRQ))
			stat_inc(STAT_WAKEUPS_IRQ);
		else if (flags & LINK_IRQEXIT)
			stat_inc(STAT_WAKEUPS_IRQEXIT);
		else if (flags & LINK_SOFTIRQ)
			stat_inc(STAT_WAKEUPS_SOFTIRQ);
	}
	return 0;
}

SEC("tp_btf/sched_waking")
int BPF_PROG(on_waking, struct task_struct *p)
{
	return record_wakeup(ctx, p, false);
}

SEC("tp_btf/sched_wakeup_new")
int BPF_PROG(on_wakeup_new, struct task_struct *p)
{
	return record_wakeup(ctx, p, true);
}

SEC("tp_btf/sched_switch")
int BPF_PROG(on_switch, bool preempt, struct task_struct *prev,
	     struct task_struct *next, unsigned int prev_state)
{
	struct task_state *ts;
	struct chain_key *key;
	struct chain_val *val, init;
	__u64 now = 0, delta;
	__u32 zero = 0, *nest;
	bool woken;
	long err;

	/*
	 * Nothing schedules inside irq_exit_rcu(), so a nonzero depth here
	 * means an enter/leave pair was split (e.g. while attaching); resync.
	 */
	nest = irq_exit_depth();
	if (nest && *nest) {
		*nest = 0;
		stat_inc(STAT_IRQEXIT_RESYNC);
	}

	/* prev is going to sleep (current == prev here). */
	if (!preempt && prev_state != TASK_RUNNING &&
	    !(prev_state & TASK_DEAD) &&
	    (!state_mask || (state_class(prev_state) & state_mask)) &&
	    target_matches(prev)) {
		ts = bpf_task_storage_get(&task_states, prev, NULL,
					  BPF_LOCAL_STORAGE_GET_F_CREATE);
		if (ts) {
			now = bpf_ktime_get_ns();
			ts->offcpu_ts = now;
			ts->kstack_id = kernel_stack(ctx);
			ts->ustack_id = user_stack(ctx, prev);
			stat_inc(STAT_BLOCKED);
		} else {
			stat_inc(STAT_STORAGE_FAIL);
		}
	}

	/* next is getting the CPU back. */
	ts = bpf_task_storage_get(&task_states, next, NULL, 0);
	if (!ts)
		return 0;

	/*
	 * The wakeup that ends a sleep always happens after the task's
	 * previous switch-in (it may precede the switch-out tracepoint when
	 * the waker races with schedule()), so consume it here.
	 */
	woken = ts->woken;
	if (woken)
		ts->woken = 0;
	if (!ts->offcpu_ts)
		return 0;
	if (!now)
		now = bpf_ktime_get_ns();
	delta = now - ts->offcpu_ts;
	ts->offcpu_ts = 0;
	if (delta < min_block_ns || delta > max_block_ns) {
		stat_inc(STAT_FILTERED_TIME);
		return 0;
	}

	key = bpf_map_lookup_elem(&key_scratch, &zero);
	if (!key)
		return 0;
	__builtin_memset(key, 0, sizeof(*key));
	key->pid = per_thread ? next->pid : 0;
	key->kstack_id = ts->kstack_id;
	key->ustack_id = ts->ustack_id;
	key->tgid = (per_thread || ts->ustack_id >= 0) ? next->tgid : 0;
	__builtin_memcpy(key->comm, next->comm, TASK_COMM_LEN);

	/* next is running, so nobody rewrites its chain concurrently */
	if (woken) {
		key->depth = ts->depth;
		__builtin_memcpy(key->links, ts->links, sizeof(key->links));
	} else {
		key->flags |= KEY_NO_WAKER;
		stat_inc(STAT_NO_WAKER);
	}

	val = bpf_map_lookup_elem(&chains, key);
	if (!val) {
		init.total_ns = delta;
		init.count = 1;
		err = bpf_map_update_elem(&chains, key, &init, BPF_NOEXIST);
		if (!err)
			goto accounted;
		if (err != -EEXIST) {
			stat_inc(err == -E2BIG ? STAT_CHAINS_FULL : STAT_CHAINS_ERR);
			return 0;
		}
		val = bpf_map_lookup_elem(&chains, key);
		if (!val)
			return 0;
	}
	__sync_fetch_and_add(&val->total_ns, delta);
	__sync_fetch_and_add(&val->count, 1);
accounted:
	stat_inc(STAT_ACCOUNTED);
	return 0;
}

char LICENSE[] SEC("license") = "Dual BSD/GPL";
