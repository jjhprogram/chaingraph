/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */
/*
 * chaingraph: off-CPU time annotated with the full chain of wakeups that
 * ended each sleep ("who woke the waker?").
 *
 * Types shared between the BPF program and the userspace front end.
 */
#ifndef __CHAINGRAPH_H
#define __CHAINGRAPH_H

#define TASK_COMM_LEN		16

/*
 * Compile-time cap on waker levels stored per task. The runtime --depth
 * option selects 1..MAX_CHAIN_DEPTH; raising this constant grows every
 * per-task record and every aggregation key by sizeof(struct chain_link).
 */
#define MAX_CHAIN_DEPTH		8

#define PERF_MAX_STACK_DEPTH	127

/* --softirq: whether a chain continues past a softirq-context waker */
#define CG_SOFTIRQ_AUTO		0	/* cut only softirqs run on interrupt exit */
#define CG_SOFTIRQ_KEEP		1
#define CG_SOFTIRQ_CUT		2

/* --state classes, matched against state_mask (like ps(1) letters) */
#define CG_STATE_S		1	/* TASK_INTERRUPTIBLE */
#define CG_STATE_D		2	/* TASK_UNINTERRUPTIBLE, not TASK_NOLOAD */
#define CG_STATE_I		4	/* TASK_IDLE */
#define CG_STATE_OTHER		8	/* stopped, traced, parked, frozen, ... */

/* chain_link.flags */
#define LINK_VALID	(1U << 0)	/* slot holds a waker */
#define LINK_HARDIRQ	(1U << 1)	/* wakeup issued from hardirq context */
#define LINK_SOFTIRQ	(1U << 2)	/* wakeup issued while serving a softirq */
#define LINK_NMI	(1U << 3)	/* wakeup issued from NMI context */
#define LINK_FORK	(1U << 4)	/* edge is a fork (wake_up_new_task) */
#define LINK_TRUNC	(1U << 5)	/* waker had older history cut by --depth */
#define LINK_IDLE	(1U << 6)	/* waker was an idle task (pid 0) */
#define LINK_IRQEXIT	(1U << 7)	/* issued on the way out of an interrupt */
#define LINK_THAW	(1U << 8)	/* wakee was frozen: a thaw, not its waker */
#define LINK_TORN	(1U << 9)	/* waker's chain changed while being read */

/*
 * One level of a wakeup chain: the task that issued a wakeup, and where it
 * was when it did so. Stack ids index the "kstacks"/"ustacks" maps; a
 * negative id is either "not collected" (-1) or the negative errno returned
 * by bpf_get_stackid(). pid is zero unless per-thread; tgid is zero unless
 * per-thread or needed to symbolize ustack_id. For links whose chain was cut
 * because the waker was only interrupted (see LINK_HARDIRQ etc.), comm, pid
 * and tgid are zero; kstack_id still includes the interrupted context below
 * the interrupt entry, which userspace trims.
 */
struct chain_link {
	__s32 kstack_id;
	__s32 ustack_id;
	__u32 pid;
	__u32 tgid;
	__u32 flags;		/* LINK_* */
	char comm[TASK_COMM_LEN];
	__u32 __pad;
};

/* chain_key.flags */
#define KEY_NO_WAKER	(1U << 0)	/* no wakeup seen since last switch-in */

/*
 * Aggregation key: a blocked task, where it blocked, and the chain of
 * wakers that led to it running again. links[0] woke the target,
 * links[1] woke links[0], and so on. Slots at index >= depth are zero.
 */
struct chain_key {
	__u32 pid;		/* target thread id (0 unless per-thread) */
	__u32 tgid;		/* target process id (0 unless needed) */
	__s32 kstack_id;	/* target off-CPU kernel stack */
	__s32 ustack_id;	/* target off-CPU user stack */
	char comm[TASK_COMM_LEN];
	__u32 flags;		/* KEY_* */
	__u32 depth;		/* number of valid links */
	struct chain_link links[MAX_CHAIN_DEPTH];
};

struct chain_val {
	__u64 total_ns;		/* summed off-CPU time */
	__u64 count;		/* number of sleeps */
};

/* Indexes into the per-CPU "stats" array. */
enum cg_stat {
	STAT_BLOCKED,		/* tracked tasks that went off-CPU to sleep */
	STAT_ACCOUNTED,		/* sleeps added to the chains map */
	STAT_WAKEUPS,		/* sched_waking events recorded */
	STAT_WAKEUPS_IRQ,	/* ... of which from hardirq/NMI context */
	STAT_WAKEUPS_IRQEXIT,	/* ... of which on interrupt exit */
	STAT_WAKEUPS_SOFTIRQ,	/* ... of which from softirq context */
	STAT_FORKS,		/* sched_wakeup_new events recorded */
	STAT_NO_WAKER,		/* sleeps that ended without a seen wakeup */
	STAT_FILTERED_TIME,	/* sleeps dropped by --min/--max block time */
	STAT_TORN,		/* waker chains dropped by a concurrent rewrite */
	STAT_IRQEXIT_RESYNC,	/* irq_exit nesting found nonzero at a context switch */
	STAT_STORAGE_FAIL,	/* bpf_task_storage_get() returned NULL */
	STAT_KSTACK_COLLIDE,	/* kernel stack map hash-bucket collisions */
	STAT_USTACK_COLLIDE,	/* user stack map hash-bucket collisions */
	STAT_KSTACK_ERR,	/* other kernel bpf_get_stackid() failures */
	STAT_USTACK_ERR,	/* other user bpf_get_stackid() failures */
	STAT_CHAINS_FULL,	/* chains map at --max-chains */
	STAT_CHAINS_ERR,	/* other chains map update failures */
	STAT_MAX,
};

#endif /* __CHAINGRAPH_H */
