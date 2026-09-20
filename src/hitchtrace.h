/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */
/*
 * hitchtrace prototype: frame-scoped, cause-gated attribution of stalls.
 *
 * A frame is the interval between two marks on one "root" thread (the thread
 * that would call vkQueuePresentKHR; here a uprobe on the workload's frame
 * marker). Within a frame the root's timeline is split into on-CPU, runnable
 * and blocked time, each interval clipped to the frame window, so
 *
 *	oncpu + runnable + runqueue + Σ(blocked buckets) == frame_ns
 *
 * holds on the root. Blocked time is then resolved through the wake chain:
 * the part of a wait that overlaps the waker's own stall is inherited from
 * the waker, and the rest is the waker running on-CPU ("waiting for work",
 * not a kernel stall).
 *
 * Records cross to user space only for frames over budget.
 *
 * Types shared between the BPF program and the userspace front end.
 */
#ifndef __HITCHTRACE_H
#define __HITCHTRACE_H

#define TASK_COMM_LEN		16
#define HT_MAX_HOPS		5	/* chain hops kept for the worst stall */
#define HT_PERF_MAX_STACK	32	/* frames per kernel stack */

/*
 * Root-timeline buckets. HT_ONCPU..HT_BLOCK_OTHER partition the frame;
 * the HT_RESOLVED_* pair splits HT_BLOCK_TASK by what the waker was doing
 * and is reported separately (it does not add to the partition).
 */
enum ht_cause {
	HT_ONCPU,		/* running */
	HT_RUNNABLE,		/* preempted: runnable, not running */
	HT_RUNQUEUE,		/* woken, waiting for a CPU */
	HT_BLOCK_TASK,		/* blocked, woken by another task */
	HT_BLOCK_IO,		/* blocked with in_iowait set */
	HT_BLOCK_TIMER,		/* blocked, woken from interrupt context */
	HT_BLOCK_OTHER,		/* blocked, no wakeup seen */
	HT_CAUSE_PARTITION,	/* ---- above sums to frame_ns ---- */
	HT_RESOLVED_WAIT_ONCPU = HT_CAUSE_PARTITION,	/* waker was running */
	HT_RESOLVED_INHERITED,	/* waker was itself stalled */
	HT_CAUSE_MAX,
};

/* hop.flags / hitch_record.flags */
#define HT_HOP_IRQ	(1U << 0)	/* woken from hardirq/NMI context */
#define HT_HOP_SOFTIRQ	(1U << 1)	/* woken from softirq context */
#define HT_HOP_IRQEXIT	(1U << 2)	/* woken on the way out of an interrupt */
#define HT_HOP_IDLE	(1U << 3)	/* waker was an idle task */
#define HT_HOP_ONCPU	(1U << 4)	/* waker was running, not stalled */
#define HT_HOP_TRUNC	(1U << 5)	/* chain continues beyond HT_MAX_HOPS */
#define HT_HOP_STALE	(1U << 6)	/* waker's stall ended before this wait began */

#define HT_FRAME_OPEN_STALL	(1U << 0)	/* a stall was still open at frame end */
#define HT_FRAME_NO_ROOT_STATE	(1U << 1)	/* root state was created mid-frame */
#define HT_FRAME_LOST		(1U << 2)	/* some accounting was dropped */

/*
 * One hop of the resolved chain for the frame's worst blocked interval.
 * hops[0] is the task that woke the root, hops[1] woke hops[0], ...
 */
struct ht_hop {
	__u32 pid;
	__u32 tgid;
	char comm[TASK_COMM_LEN];
	__u64 ns;		/* of the waited time this hop explains */
	__u32 cause;		/* enum ht_cause: what the waker was doing */
	__u32 flags;		/* HT_HOP_* */
	__s32 kstack_id;	/* where the waker was blocked, if it was */
	__u32 __pad;
};

/* The worst single blocked interval of the frame, and its chain. */
struct ht_stall {
	__u64 ns;		/* clipped to the frame window */
	__u64 start_ns;
	__u32 cause;		/* enum ht_cause */
	__s32 kstack_id;	/* the root's own blocking stack */
	__s32 ustack_id;
	__u32 nhops;
	struct ht_hop hops[HT_MAX_HOPS];
};

/* One over-budget frame. */
struct ht_record {
	__u64 frame_id;
	__u64 frame_start_ns;
	__u64 frame_end_ns;
	__u64 frame_ns;
	__u64 budget_ns;
	__u64 cause_ns[HT_CAUSE_MAX];
	struct ht_stall worst;
	__u32 root_pid;
	__u32 root_tgid;
	char root_comm[TASK_COMM_LEN];
	__u32 nstalls;		/* blocked/runnable intervals in this frame */
	__u32 flags;		/* HT_FRAME_* */
};

/* Per-CPU stats, indexed by enum ht_stat. */
enum ht_stat {
	HT_STAT_FRAMES,		/* frames seen */
	HT_STAT_HITCHES,	/* frames over budget */
	HT_STAT_EMITTED,	/* records written to the ring buffer */
	HT_STAT_RINGBUF_FULL,	/* records dropped, ring buffer full */
	HT_STAT_STALLS,		/* off-CPU intervals accounted */
	HT_STAT_RESOLVED,	/* blocked intervals resolved through a waker */
	HT_STAT_UNRESOLVED,	/* ... where the waker's state was unusable */
	HT_STAT_STORAGE_FAIL,	/* bpf_task_storage_get() returned NULL */
	HT_STAT_STACK_ERR,	/* bpf_get_stackid() failures */
	HT_STAT_MAX,
};

#endif /* __HITCHTRACE_H */
