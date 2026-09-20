// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/*
 * hitchtrace  Frame-scoped attribution of stalls: for every frame that misses
 *             its budget, where the frame's time went on the root thread
 *             (on-CPU, runnable, runqueue, blocked), and what the worst
 *             blocked interval was waiting for, resolved through the chain of
 *             wakeups that ended it.
 *
 * A frame is the interval between two calls of the workload's frame marker
 * (a uprobe; the argument is the frame id). Only over-budget frames cross to
 * user space, as one struct ht_record on a ring buffer.
 *
 * USAGE: hitchtrace -p PID [-b BUDGET_US] [-x BINARY] [-m SYMBOL] [-o OUT.jsonl]
 *                   [-d SECONDS] [-q] [-v] [--min-stall US] [--hops N]
 */
#include <argp.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/btf.h>
#include <bpf/libbpf.h>

#include "hitchtrace.h"
#include "hitchtrace.skel.h"
#include "syms.h"

#define DEFAULT_MARKER		"hitch_frame_mark"
#define DEFAULT_PRESENT_MARKER	"hitch_present_end"
#define DEFAULT_BUDGET_US	16667
#define DEFAULT_MIN_STALL_US	100
#define POLL_TIMEOUT_MS		100

/* largest microsecond value whose nanoseconds fit in 64 bits */
#define MAX_US			((long)(UINT64_MAX / 1000))

static struct env {
	pid_t tgid;
	__u64 budget_us;
	__u64 min_stall_us;
	const char *binary;
	const char *marker;
	const char *present_marker;
	const char *jsonl;
	int hops;
	long duration;
	bool user_stacks;
	bool quiet;
	bool verbose;
} env = {
	.budget_us = DEFAULT_BUDGET_US,
	.min_stall_us = DEFAULT_MIN_STALL_US,
	.marker = DEFAULT_MARKER,
	.present_marker = DEFAULT_PRESENT_MARKER,
	.hops = HT_MAX_HOPS,
};

enum {
	OPT_MIN_STALL = 0x100,
	OPT_HOPS,
};

const char *argp_program_version = "hitchtrace 0.1";
static const char argp_program_doc[] =
"Explain frames that missed their budget: the root thread's time split into\n"
"on-CPU, runnable, runqueue and blocked, and the worst blocked interval\n"
"resolved through the chain of wakeups behind it.\n"
"\n"
"USAGE: hitchtrace -p PID [OPTION...]\n"
"\n"
"EXAMPLES:\n"
"    hitchtrace -p 4123                     # 16.7 ms budget, until Ctrl-C\n"
"    hitchtrace -p 4123 -b 8000 -d 30       # 8 ms budget, 30 seconds\n"
"    hitchtrace -p 4123 -o hitches.jsonl -q # machine-readable only\n"
"    hitchtrace -p 4123 -x ./game -m present_done\n";

static const struct argp_option opts[] = {
	{ "pid", 'p', "PID", 0, "Target process (required); its threads are tracked", 0 },
	{ "budget", 'b', "USEC", 0, "Frame budget in microseconds (default 16667)", 0 },
	{ "binary", 'x', "PATH", 0,
	  "Binary holding the marker symbol (default /proc/PID/exe)", 0 },
	{ "marker", 'm', "SYMBOL", 0,
	  "Frame marker symbol to probe (default " DEFAULT_MARKER ")", 0 },
	{ "present-marker", 'M', "SYMBOL", 0,
	  "Present-return marker; time between it and the frame marker is the "
	  "display pacing the app (default " DEFAULT_PRESENT_MARKER
	  ", \"\" to disable)", 0 },
	{ "jsonl", 'o', "FILE", 0, "Also append one JSON object per hitch to FILE", 0 },
	{ "duration", 'd', "SECONDS", 0,
	  "Stop after this many seconds (default: until Ctrl-C or target exit)", 0 },
	{ "quiet", 'q', NULL, 0, "No human-readable output (JSONL only)", 0 },
	{ "user-stacks", 'U', NULL, 0, "Also collect user stacks for the worst stall", 0 },
	{ "min-stall", OPT_MIN_STALL, "USEC", 0,
	  "Ignore blocked intervals shorter than this when picking the worst "
	  "stall (default 100)", 0 },
	{ "hops", OPT_HOPS, "N", 0, "Chain hops to print, 1-5 (default 5)", 0 },
	{ "verbose", 'v', NULL, 0, "Verbose libbpf output", 0 },
	{ NULL, 'h', NULL, OPTION_HIDDEN, "Show the full help", 0 },
	{},
};

static long parse_long(const char *arg, long min, long max,
		       struct argp_state *state, const char *what)
{
	char *end;
	long v;

	errno = 0;
	v = strtol(arg, &end, 10);
	if (errno || end == arg || *end || v < min || v > max) {
		fprintf(stderr, "invalid %s: %s (expected %ld..%ld)\n",
			what, arg, min, max);
		argp_usage(state);
	}
	return v;
}

static error_t parse_arg(int key, char *arg, struct argp_state *state)
{
	switch (key) {
	case 'h':
		argp_state_help(state, stderr, ARGP_HELP_STD_HELP);
		break;
	case 'v':
		env.verbose = true;
		break;
	case 'q':
		env.quiet = true;
		break;
	case 'U':
		env.user_stacks = true;
		break;
	case 'p':
		env.tgid = parse_long(arg, 1, INT_MAX, state, "PID");
		break;
	case 'b':
		env.budget_us = parse_long(arg, 1, MAX_US, state, "budget");
		break;
	case 'x':
		env.binary = arg;
		break;
	case 'm':
		env.marker = arg;
		break;
	case 'M':
		env.present_marker = arg;
		break;
	case 'o':
		env.jsonl = arg;
		break;
	case 'd':
		env.duration = parse_long(arg, 1, LONG_MAX, state, "duration");
		break;
	case OPT_MIN_STALL:
		env.min_stall_us = parse_long(arg, 0, MAX_US, state, "min stall time");
		break;
	case OPT_HOPS:
		env.hops = parse_long(arg, 1, HT_MAX_HOPS, state, "hops");
		break;
	case ARGP_KEY_ARG:
		fprintf(stderr, "unrecognized positional argument: %s\n", arg);
		argp_usage(state);
		break;
	case ARGP_KEY_END:
		if (!env.tgid) {
			fprintf(stderr, "-p/--pid is required\n");
			argp_usage(state);
		}
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static int libbpf_print_fn(enum libbpf_print_level level, const char *format,
			   va_list args)
{
	if (level == LIBBPF_DEBUG && !env.verbose)
		return 0;
	return vfprintf(stderr, format, args);
}

static double ms(__u64 ns)
{
	return (double)ns / 1e6;
}

/* ---- naming ------------------------------------------------------------ */

/*
 * The buckets of enum ht_cause and the keys they carry in the JSON, in enum
 * order. Listed once here: the array and the coverage check below are both
 * built from this, so a bucket added to the enum without a name is a build
 * error rather than a null key in the output.
 */
#define HT_CAUSE_LIST(X)						\
	X(HT_ONCPU,			"oncpu")			\
	X(HT_ONCPU_FAULT,		"oncpu_fault")			\
	X(HT_ONCPU_FAULT_MAJOR,		"oncpu_fault_major")		\
	X(HT_ONCPU_RECLAIM,		"oncpu_reclaim")		\
	X(HT_ONCPU_COMPACT,		"oncpu_compact")		\
	X(HT_RUNNABLE,			"runnable")			\
	X(HT_RUNQUEUE,			"runqueue")			\
	X(HT_BLOCK_FUTEX,		"block_futex")			\
	X(HT_BLOCK_POLL,		"block_poll")			\
	X(HT_BLOCK_IO,			"block_io")			\
	X(HT_BLOCK_TIMER,		"block_timer")			\
	X(HT_BLOCK_GPU,			"block_gpu")			\
	X(HT_BLOCK_PRESENT,		"block_present")		\
	X(HT_BLOCK_TASK,		"block_task")			\
	X(HT_BLOCK_OTHER,		"block_other")			\
	X(HT_RESOLVED_WAIT_ONCPU,	"resolved_wait_oncpu")		\
	X(HT_RESOLVED_INHERITED,	"resolved_inherited")

#define HT_CAUSE_NAME(cause, name)	[cause] = name,
#define HT_CAUSE_ONE(cause, name)	+ 1

static const char *const cause_names[HT_CAUSE_MAX] = {
	HT_CAUSE_LIST(HT_CAUSE_NAME)
};

static_assert(0 HT_CAUSE_LIST(HT_CAUSE_ONE) == HT_CAUSE_MAX,
	      "cause_names[] does not name every bucket of enum ht_cause");

/*
 * The two runs of buckets the human block renders under one total: the
 * on-CPU time with its kernel stalls carved out, and the blocked time.
 */
#define HT_ONCPU_LAST	HT_ONCPU_COMPACT
#define HT_BLOCK_FIRST	HT_BLOCK_FUTEX
#define HT_BLOCK_LAST	HT_BLOCK_OTHER

static_assert(HT_ONCPU_LAST + 1 == HT_RUNNABLE &&
	      HT_BLOCK_LAST + 1 == HT_CAUSE_PARTITION,
	      "the on-CPU and blocked buckets are no longer contiguous");

/* Human phrasing of what a thread was doing, used when there is no stack. */
static const char *cause_phrase(__u32 cause)
{
	switch (cause) {
	case HT_ONCPU:			return "running";
	case HT_ONCPU_FAULT:		return "running, in a page fault";
	case HT_ONCPU_FAULT_MAJOR:	return "running, in a major fault";
	case HT_ONCPU_RECLAIM:		return "running, in reclaim";
	case HT_ONCPU_COMPACT:		return "running, in compaction";
	case HT_RUNNABLE:		return "preempted, runnable";
	case HT_RUNQUEUE:		return "waiting for a CPU";
	case HT_BLOCK_FUTEX:		return "blocked on a futex";
	case HT_BLOCK_POLL:		return "blocked in poll";
	case HT_BLOCK_IO:		return "blocked on I/O";
	case HT_BLOCK_TIMER:		return "sleeping on a timer";
	case HT_BLOCK_GPU:		return "waiting on a GPU fence";
	case HT_BLOCK_PRESENT:		return "blocked in present";
	case HT_BLOCK_TASK:		return "blocked on a task";
	case HT_BLOCK_OTHER:		return "blocked";
	default:			return "unknown";
	}
}

/* A bucket without its group prefix, for a line the group already names. */
static const char *short_name(__u32 cause)
{
	const char *name = cause_names[cause];
	const char *sep = strchr(name, '_');

	return sep ? sep + 1 : name;
}

/* bit position -> name, for the JSON flag lists */
static const char *const hop_flag_names[] = {
	"irq", "softirq", "irqexit", "idle", "oncpu", "trunc", "stale",
};
static const char *const frame_flag_names[] = {
	"open_stall", "no_root_state", "lost", "threads_full",
};

/* comm from BPF is a fixed-size field and need not be NUL-terminated */
static void comm_str(char *buf, size_t sz, const char *comm)
{
	size_t n = sz - 1 < TASK_COMM_LEN ? sz - 1 : TASK_COMM_LEN;

	memcpy(buf, comm, n);
	buf[n] = '\0';
}

/*
 * "comm/tid". A task can name itself "[hardirq]"; quote it so it cannot pose
 * as one of the interrupt labels below.
 */
static void task_label(char *buf, size_t sz, const char *comm, __u32 pid)
{
	char name[TASK_COMM_LEN + 1];

	comm_str(name, sizeof(name), comm);
	if (!name[0])
		snprintf(buf, sz, "[no comm]/%u", pid);
	else
		snprintf(buf, sz, name[0] == '[' ? "'%s'/%u" : "%s/%u", name, pid);
}

/* ---- stacks and symbols ------------------------------------------------ */

static struct ksyms *ksyms;
static struct usyms *usyms;

struct stack {
	__u64 *ips;
	int nr;
	int state;	/* 0: not read yet, 1: ok, -1: missing */
};

struct stack_map {
	int fd;
	struct stack *cache;	/* indexed by stack id */
	size_t sz;
};

static struct stack_map kstacks, ustacks;

static const struct stack *get_stack(struct stack_map *m, __s32 id)
{
	struct stack *s;
	__u32 key = id;
	__u64 *ips;

	if (id < 0 || (size_t)id >= m->sz || !m->cache)
		return NULL;
	s = &m->cache[id];
	if (!s->state) {
		ips = calloc(HT_PERF_MAX_STACK, sizeof(*ips));
		if (!ips || bpf_map_lookup_elem(m->fd, &key, ips)) {
			free(ips);
			s->state = -1;
			return NULL;
		}
		while (s->nr < HT_PERF_MAX_STACK && ips[s->nr])
			s->nr++;
		s->ips = ips;
		s->state = 1;
	}
	return s->state == 1 ? s : NULL;
}

static const char *ksym_name(__u64 addr)
{
	const char *name = ksyms ? ksyms_lookup(ksyms, addr, NULL, NULL) : NULL;

	return name ? name : "[unknown]";
}

static const char *usym_name(int tgid, __u64 addr, char *buf, size_t sz)
{
	const char *dso = NULL, *name, *base;

	name = usyms ? usyms_lookup(usyms, tgid, addr, NULL, &dso) : NULL;
	if (name)
		return name;
	if (!dso)
		return "[unknown]";
	base = strrchr(dso, '/');
	base = base ? base + 1 : dso;
	if (base[0] == '[')
		snprintf(buf, sz, "%s", base);
	else
		snprintf(buf, sz, "[%s]", base);
	return buf;
}

static bool has_prefix(const char *s, const char *prefix)
{
	return !strncmp(s, prefix, strlen(prefix));
}

/*
 * Frames of the tracing machinery itself, at the leaf end of kernel stacks.
 * Kept in step with chaingraph's copy; deliberately not shared, the two tools
 * are independent front ends.
 */
static bool is_tracing_frame(const char *name)
{
	static const char *const prefixes[] = {
		"bpf_prog_", "bpf_trace_run", "__bpf_trace_", "__traceiter_",
		"bpf_get_stack", "__bpf_get_stack", "perf_trace_",
		"trace_call_bpf", "__bpf_prog_run", "bpf_trampoline_",
		"__probestub_",
	};

	for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
		if (has_prefix(name, prefixes[i]))
			return true;
	return false;
}

/*
 * The scheduler's own entry path, which every blocked stack ends in and which
 * says nothing about what was being waited for.
 */
static bool is_scheduler_frame(const char *name)
{
	static const char *const prefixes[] = {
		"__schedule", "schedule", "io_schedule", "preempt_schedule",
		"__cond_resched", "cond_resched", "__sched_text",
	};

	for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
		if (has_prefix(name, prefixes[i]))
			return true;
	return false;
}

/*
 * The plumbing a wait sits in below the call that asked for it. "futex_wait"
 * only says the thread waited; its caller says what for, so these are skipped
 * like the scheduler frames above.
 */
static bool is_wait_wrapper(const char *name)
{
	static const char *const prefixes[] = {
		"futex_wait", "futex_do_wait", "do_futex",
		"do_epoll_wait", "do_select",
	};

	for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
		if (has_prefix(name, prefixes[i]))
			return true;
	return false;
}

/*
 * What the blocking stack was waiting in: the first frame from the leaf that
 * is neither the tracer nor a wrapper. If the stack is nothing but wrappers,
 * the innermost wrapper is still better than nothing.
 */
static const char *blocked_in(__s32 kstack_id)
{
	const struct stack *s = get_stack(&kstacks, kstack_id);
	const char *fallback = NULL;

	if (!s)
		return NULL;
	for (int i = 0; i < s->nr; i++) {
		const char *name = ksym_name(s->ips[i]);

		if (is_tracing_frame(name))
			continue;
		if (is_scheduler_frame(name) || is_wait_wrapper(name)) {
			if (!fallback)
				fallback = name;
			continue;
		}
		if (!strcmp(name, "[unknown]"))
			continue;
		return name;
	}
	return fallback;
}

/* ---- human-readable output --------------------------------------------- */

/* snprintf that appends, keeping *len in step and dropping what does not fit */
static void append(char *buf, size_t sz, size_t *len, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (*len + 1 >= sz)
		return;
	va_start(ap, fmt);
	n = vsnprintf(buf + *len, sz - *len, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	*len = (size_t)n < sz - *len ? *len + (size_t)n : sz - 1;
}

/* Time one timeline spent in the buckets [first, last]. */
static __u64 sum_causes(const __u64 *cause_ns, __u32 first, __u32 last)
{
	__u64 ns = 0;

	for (__u32 i = first; i <= last; i++)
		ns += cause_ns[i];
	return ns;
}

/*
 * The buckets of [first, last] that hold time, as "name 1.2" joined by sep,
 * to be printed under a total the caller has already named. A bucket holding
 * the whole of that total is named without repeating the figure; a range with
 * nothing in it yields an empty string.
 */
static void bucket_list(char *buf, size_t sz, const __u64 *cause_ns,
			__u32 first, __u32 last, const char *sep, __u64 total)
{
	size_t len = 0;

	buf[0] = '\0';
	for (__u32 i = first; i <= last; i++) {
		if (!cause_ns[i])
			continue;
		if (cause_ns[i] == total) {
			append(buf, sz, &len, "%s", short_name(i));
			return;
		}
		append(buf, sz, &len, "%s%s %.1f", len ? sep : "",
		       short_name(i), ms(cause_ns[i]));
	}
}

static bool hop_is_irq(const struct ht_hop *h)
{
	return h->flags & (HT_HOP_IRQ | HT_HOP_SOFTIRQ | HT_HOP_IRQEXIT);
}

static void hop_label(char *buf, size_t sz, const struct ht_hop *h)
{
	if (h->flags & HT_HOP_IRQ)
		snprintf(buf, sz, "[hardirq]");
	else if (h->flags & HT_HOP_SOFTIRQ)
		snprintf(buf, sz, "[softirq]");
	else if (h->flags & HT_HOP_IRQEXIT)
		snprintf(buf, sz, "[irq exit]");
	else
		task_label(buf, sz, h->comm, h->pid);
}

static void hop_desc(char *buf, size_t sz, const struct ht_hop *h)
{
	const char *stale = (h->flags & HT_HOP_STALE) ? " [stale]" : "";
	const char *where;

	if (hop_is_irq(h)) {
		const char *what;

		if (h->cause == HT_BLOCK_TIMER)
			what = "woken by a timer interrupt";
		else if (h->flags & HT_HOP_IRQ)
			what = "woken by an interrupt";
		else if (h->flags & HT_HOP_SOFTIRQ)
			what = "woken from a softirq";
		else
			what = "woken on interrupt exit";
		snprintf(buf, sz, "%s%s", what, stale);
		return;
	}
	if (h->flags & HT_HOP_ONCPU) {
		snprintf(buf, sz, "was running (waiting for work) %.1f ms%s",
			 ms(h->ns), stale);
		return;
	}
	if (h->flags & HT_HOP_IDLE) {
		snprintf(buf, sz, "was idle%s", stale);
		return;
	}
	where = blocked_in(h->kstack_id);
	if (where)
		snprintf(buf, sz, "blocked %.1f ms in %s%s", ms(h->ns), where, stale);
	else
		snprintf(buf, sz, "%s %.1f ms%s", cause_phrase(h->cause), ms(h->ns),
			 stale);
}

/* description column of the chain, so nested hops stay aligned */
#define HOP_DESC_COL	25

static void print_hop(int indent, const char *label, const char *desc)
{
	int pad = HOP_DESC_COL - indent - 3 - (int)strlen(label);

	printf("%*s<- %s%*s%s\n", indent, "", label, pad > 1 ? pad : 1, "", desc);
}

static void print_chain(const struct ht_stall *st)
{
	__u32 n = st->nhops;
	int indent = 4;

	if (n > HT_MAX_HOPS)
		n = HT_MAX_HOPS;
	if (n > (__u32)env.hops)
		n = env.hops;
	for (__u32 i = 0; i < n; i++) {
		const struct ht_hop *h = &st->hops[i];
		char label[TASK_COMM_LEN + 24], desc[160];

		hop_label(label, sizeof(label), h);
		hop_desc(desc, sizeof(desc), h);
		print_hop(indent, label, desc);
		if (hop_is_irq(h))	/* an interrupt ends the chain */
			return;
		indent += 2;
		if (h->flags & HT_HOP_TRUNC) {
			printf("%*s... (chain truncated)\n", indent, "");
			return;
		}
	}
	if (st->nhops > n)	/* clipped by --hops, not by the kernel */
		printf("%*s... (chain truncated)\n", indent, "");
}

static void frame_flags_str(char *buf, size_t sz, __u32 flags)
{
	/* HT_FRAME_THREADS_FULL is left out: the threads block says so itself */
	static const char *const names[] = {
		"open stall", "partial frame", "lost accounting",
	};
	char tmp[96] = "";
	size_t len = 0;

	buf[0] = '\0';
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		int n;

		if (!(flags & (1U << i)))
			continue;
		n = snprintf(tmp + len, sizeof(tmp) - len, "%s%s",
			     len ? ", " : "  [", names[i]);
		if (n < 0 || (size_t)n >= sizeof(tmp) - len)
			break;
		len += n;
	}
	if (len)
		snprintf(buf, sz, "%s]", tmp);
}

/* ---- per-thread detail -------------------------------------------------- */

#define THREAD_LINES	8	/* thread lines printed per record */

/*
 * Off-CPU time of one thread inside the frame window: every bucket that is
 * not on-CPU time, so not the kernel stalls carved out of it either. This
 * orders the lines; it is never added to the frame, which only the root's
 * timeline partitions. open_ns is deliberately left out: a thread that slept
 * through the whole frame has nothing but open time, and it is context rather
 * than the frame's problem. It is still printed, last.
 */
static __u64 thread_offcpu_ns(const struct ht_thread *t)
{
	return sum_causes(t->cause_ns, HT_RUNNABLE, HT_BLOCK_LAST);
}

/* A thread the frame never scheduled has nothing to say about it. */
static bool thread_has_detail(const struct ht_thread *t)
{
	if (t->open_ns)
		return true;
	for (__u32 i = 0; i < HT_CAUSE_PARTITION; i++)
		if (t->cause_ns[i])
			return true;
	return false;
}

/*
 * Threads worth printing, root first and the rest by descending off-CPU time,
 * as indices into r->threads[]. Insertion sort: at most HT_MAX_THREADS of them.
 */
static __u32 thread_order(const struct ht_record *r, __u32 *idx)
{
	__u32 n = 0, nthreads = r->nthreads;

	if (nthreads > HT_MAX_THREADS)
		nthreads = HT_MAX_THREADS;
	for (__u32 i = 0; i < nthreads; i++) {
		const struct ht_thread *t = &r->threads[i];
		bool root = t->flags & HT_THREAD_ROOT;
		__u64 ns = thread_offcpu_ns(t);
		__u32 j;

		if (!thread_has_detail(t))
			continue;
		for (j = n; j > 0; j--) {
			const struct ht_thread *p = &r->threads[idx[j - 1]];

			if (p->flags & HT_THREAD_ROOT)
				break;
			if (!root && thread_offcpu_ns(p) >= ns)
				break;
			idx[j] = idx[j - 1];
		}
		idx[j] = i;
		n++;
	}
	return n;
}

/* The buckets a thread spent time in, then what stands out about its frame. */
static void thread_detail(char *buf, size_t sz, const struct ht_thread *t)
{
	__u64 oncpu = sum_causes(t->cause_ns, HT_ONCPU, HT_ONCPU_LAST);
	__u64 blocked = sum_causes(t->cause_ns, HT_BLOCK_FIRST, HT_BLOCK_LAST);
	char sub[160];
	const char *where;
	size_t len = 0;

	buf[0] = '\0';
	if (oncpu) {
		bucket_list(sub, sizeof(sub), t->cause_ns, HT_ONCPU + 1,
			    HT_ONCPU_LAST, ", ", oncpu);
		append(buf, sz, &len, "oncpu %.1f", ms(oncpu));
		if (sub[0])
			append(buf, sz, &len, " (%s)", sub);
	}
	for (__u32 i = HT_RUNNABLE; i <= HT_RUNQUEUE; i++)
		if (t->cause_ns[i])
			append(buf, sz, &len, "%s%s %.1f", len ? "  " : "",
			       cause_names[i], ms(t->cause_ns[i]));
	/*
	 * The carve-outs above are part of the on-CPU total, so they are
	 * parenthesized; the blocked buckets add up to their own total.
	 */
	if (blocked) {
		bucket_list(sub, sizeof(sub), t->cause_ns, HT_BLOCK_FIRST,
			    HT_BLOCK_LAST, " | ", blocked);
		append(buf, sz, &len, "%sblocked %.1f %s", len ? "  " : "",
		       ms(blocked), sub);
	}
	/* HT_THREAD_BLOCKED_END: the tail of an interval the frame outlived */
	if (t->open_ns)
		append(buf, sz, &len, "%s(still blocked at frame end %.1f ms)",
		       len ? " " : "", ms(t->open_ns));
	/*
	 * The root's longest interval is the worst stall, printed above with
	 * its chain; for the other threads this is the only place it shows.
	 */
	where = t->largest_ns && !(t->flags & HT_THREAD_ROOT) ?
		blocked_in(t->largest_kstack) : NULL;
	if (where)
		append(buf, sz, &len, "   largest %.1f ms in %s",
		       ms(t->largest_ns), where);
	if (t->preemptor_ns) {
		char who[TASK_COMM_LEN + 24];

		task_label(who, sizeof(who), t->preemptor_comm, t->preemptor_pid);
		append(buf, sz, &len, "   preempted by %s for %.1f ms", who,
		       ms(t->preemptor_ns));
	}
}

/*
 * The other threads of the process, as context for the frame: they run in
 * parallel, so their off-CPU time is not part of the frame and is never
 * summed into it.
 */
static void print_threads(const struct ht_record *r)
{
	char labels[HT_MAX_THREADS][TASK_COMM_LEN + 24];
	__u32 idx[HT_MAX_THREADS], n, shown;
	bool any_root = false;
	int width = 0;

	n = thread_order(r, idx);
	if (!n && !(r->flags & HT_FRAME_THREADS_FULL))
		return;
	shown = n < THREAD_LINES ? n : THREAD_LINES;
	for (__u32 i = 0; i < shown; i++) {
		const struct ht_thread *t = &r->threads[idx[i]];
		int len;

		task_label(labels[i], sizeof(labels[i]), t->comm, t->pid);
		len = (int)strlen(labels[i]);
		if (len > width)
			width = len;
		if (t->flags & HT_THREAD_ROOT)
			any_root = true;
	}

	printf("  threads\n");
	for (__u32 i = 0; i < shown; i++) {
		const struct ht_thread *t = &r->threads[idx[i]];
		char detail[320];

		thread_detail(detail, sizeof(detail), t);
		if (any_root)
			printf("    %-*s  %-6s  %s\n", width, labels[i],
			       (t->flags & HT_THREAD_ROOT) ? "(root)" : "",
			       detail);
		else
			printf("    %-*s  %s\n", width, labels[i], detail);
	}
	if (n > shown)
		printf("    ... and %u more\n", n - shown);
	if (r->flags & HT_FRAME_THREADS_FULL)
		printf("    (thread list truncated at %d threads)\n",
		       HT_MAX_THREADS);
}

/*
 * How the frame's time went, in two or three short lines: the root's on-CPU
 * time with the kernel stalls carved out of it, the blocked time under its
 * total, and what the waits resolved to. A bucket nothing was spent in is
 * left out; the on-CPU line stays when the frame has nothing else to show.
 */
static void print_timeline(const struct ht_record *r)
{
	const __u64 *c = r->cause_ns;
	__u64 oncpu = sum_causes(c, HT_ONCPU, HT_ONCPU_LAST);
	__u64 blocked = sum_causes(c, HT_BLOCK_FIRST, HT_BLOCK_LAST);
	char sub[160], buf[160];
	size_t len = 0;

	if (oncpu || c[HT_RUNNABLE] || c[HT_RUNQUEUE] || !blocked) {
		bucket_list(sub, sizeof(sub), c, HT_ONCPU + 1, HT_ONCPU_LAST,
			    ", ", oncpu);
		printf("  oncpu   %6.1f ms", ms(oncpu));
		if (sub[0])
			printf("  (%s)", sub);
		if (c[HT_RUNNABLE])
			printf("   runnable %.1f ms", ms(c[HT_RUNNABLE]));
		if (c[HT_RUNQUEUE])
			printf("   runqueue %.1f ms", ms(c[HT_RUNQUEUE]));
		printf("\n");
	}
	if (blocked) {
		bucket_list(sub, sizeof(sub), c, HT_BLOCK_FIRST, HT_BLOCK_LAST,
			    " | ", blocked);
		printf("  blocked %6.1f ms  %s\n", ms(blocked), sub);
	}
	if (c[HT_RESOLVED_WAIT_ONCPU] || c[HT_RESOLVED_INHERITED]) {
		buf[0] = '\0';
		if (c[HT_RESOLVED_WAIT_ONCPU])
			append(buf, sizeof(buf), &len,
			       "waiting on a running task %.1f ms",
			       ms(c[HT_RESOLVED_WAIT_ONCPU]));
		if (c[HT_RESOLVED_INHERITED])
			append(buf, sizeof(buf), &len, "%sinherited stalls %.1f ms",
			       len ? ", " : "", ms(c[HT_RESOLVED_INHERITED]));
		printf("  resolved   %s\n", buf);
	}
}

static void print_record(const struct ht_record *r)
{
	__u64 over = r->frame_ns > r->budget_ns ? r->frame_ns - r->budget_ns : 0;
	char root[TASK_COMM_LEN + 24], flags[80];
	const char *where;

	task_label(root, sizeof(root), r->root_comm, r->root_pid);
	frame_flags_str(flags, sizeof(flags), r->flags);
	printf("hitch frame %llu  %.1f ms  (budget %.1f ms, over by %.1f ms)  root %s%s\n",
	       (unsigned long long)r->frame_id, ms(r->frame_ns), ms(r->budget_ns),
	       ms(over), root, flags);
	print_timeline(r);

	if (r->worst.ns) {
		where = blocked_in(r->worst.kstack_id);
		if (where)
			printf("  worst stall %.1f ms  blocked in %s\n",
			       ms(r->worst.ns), where);
		else
			printf("  worst stall %.1f ms  %s\n", ms(r->worst.ns),
			       cause_phrase(r->worst.cause));
		print_chain(&r->worst);
	}
	print_threads(r);
	printf("  %u stall%s this frame\n\n", r->nstalls, r->nstalls == 1 ? "" : "s");
	fflush(stdout);
}

/* ---- JSONL output ------------------------------------------------------- */

static FILE *jsonl;

static void json_str(FILE *f, const char *s)
{
	fputc('"', f);
	for (; s && *s; s++) {
		unsigned char c = *s;

		if (c == '"' || c == '\\')
			fprintf(f, "\\%c", c);
		else if (c < 0x20 || c == 0x7f)
			fprintf(f, "\\u%04x", c);
		else
			fputc(c, f);
	}
	fputc('"', f);
}

static void json_comm(FILE *f, const char *comm)
{
	char name[TASK_COMM_LEN + 1];

	comm_str(name, sizeof(name), comm);
	json_str(f, name);
}

static void json_flags(FILE *f, __u32 flags, const char *const *names, size_t n)
{
	bool first = true;

	fputc('[', f);
	for (size_t i = 0; i < n; i++) {
		if (!(flags & (1U << i)))
			continue;
		fprintf(f, "%s", first ? "" : ",");
		json_str(f, names[i]);
		first = false;
	}
	fputc(']', f);
}

static void json_kstack(FILE *f, __s32 id)
{
	const struct stack *s = get_stack(&kstacks, id);
	bool first = true;

	fputc('[', f);
	for (int i = 0; s && i < s->nr; i++) {
		const char *name = ksym_name(s->ips[i]);

		if (is_tracing_frame(name))
			continue;
		fprintf(f, "%s", first ? "" : ",");
		json_str(f, name);
		first = false;
	}
	fputc(']', f);
}

static void json_ustack(FILE *f, __s32 id, __u32 tgid)
{
	const struct stack *s = get_stack(&ustacks, id);
	char buf[256];

	fputc('[', f);
	for (int i = 0; s && i < s->nr; i++) {
		fprintf(f, "%s", i ? "," : "");
		json_str(f, usym_name(tgid, s->ips[i], buf, sizeof(buf)));
	}
	fputc(']', f);
}

static void json_hop(FILE *f, const struct ht_hop *h)
{
	bool blocked = !hop_is_irq(h) && !(h->flags & (HT_HOP_ONCPU | HT_HOP_IDLE));
	const char *where = blocked ? blocked_in(h->kstack_id) : NULL;

	/* pid is the thread id and tgid the process, as in the kernel */
	fprintf(f, "{\"pid\":%u,\"tgid\":%u,\"comm\":", h->pid, h->tgid);
	json_comm(f, h->comm);
	fprintf(f, ",\"ns\":%llu,\"cause\":", (unsigned long long)h->ns);
	json_str(f, h->cause < HT_CAUSE_MAX ? cause_names[h->cause] : "unknown");
	fprintf(f, ",\"flags\":");
	json_flags(f, h->flags, hop_flag_names,
		   sizeof(hop_flag_names) / sizeof(hop_flag_names[0]));
	fprintf(f, ",\"kstack_id\":%d", h->kstack_id);
	fprintf(f, ",\"blocked_in\":");
	if (where)
		json_str(f, where);
	else
		fprintf(f, "null");
	fputc('}', f);
}

/*
 * One thread's frame. cause_ns holds only the partition buckets, and for
 * every thread but the root it is context, not a share of frame_ns.
 */
static void json_thread(FILE *f, const struct ht_thread *t)
{
	/* largest_kstack is only meaningful once there is an interval */
	const char *where = t->largest_ns ? blocked_in(t->largest_kstack) : NULL;

	fprintf(f, "{\"tid\":%u,\"comm\":", t->pid);
	json_comm(f, t->comm);
	fprintf(f, ",\"root\":%s",
		(t->flags & HT_THREAD_ROOT) ? "true" : "false");

	fprintf(f, ",\"cause_ns\":{");
	for (__u32 i = 0; i < HT_CAUSE_PARTITION; i++) {
		fprintf(f, "%s", i ? "," : "");
		json_str(f, cause_names[i]);
		fprintf(f, ":%llu", (unsigned long long)t->cause_ns[i]);
	}
	fprintf(f, "},\"open_ns\":%llu", (unsigned long long)t->open_ns);
	fprintf(f, ",\"flags\":[");
	if (t->flags & HT_THREAD_ROOT)
		fprintf(f, "\"root\"");
	if (t->flags & HT_THREAD_BLOCKED_END)
		fprintf(f, "%s\"blocked_at_frame_end\"",
			(t->flags & HT_THREAD_ROOT) ? "," : "");
	fprintf(f, "]");

	/*
	 * largest_* only means anything once there was an interval; with no
	 * off-CPU time at all the cause is null rather than a bucket name
	 * that nothing was spent in.
	 */
	fprintf(f, ",\"largest\":{\"ns\":%llu,\"cause\":",
		(unsigned long long)t->largest_ns);
	if (!t->largest_ns)
		fprintf(f, "null");
	else
		json_str(f, t->largest_cause < HT_CAUSE_MAX ?
			    cause_names[t->largest_cause] : "unknown");
	fprintf(f, ",\"blocked_in\":");
	if (where)
		json_str(f, where);
	else
		fprintf(f, "null");
	fprintf(f, ",\"kstack\":");
	if (t->largest_ns)
		json_kstack(f, t->largest_kstack);
	else
		fprintf(f, "[]");

	fprintf(f, "},\"preemptor\":");
	if (t->preemptor_ns) {
		fprintf(f, "{\"tid\":%u,\"tgid\":%u,\"comm\":",
			t->preemptor_pid, t->preemptor_tgid);
		json_comm(f, t->preemptor_comm);
		fprintf(f, ",\"ns\":%llu}", (unsigned long long)t->preemptor_ns);
	} else {
		fprintf(f, "null");
	}
	fputc('}', f);
}

static void json_record(FILE *f, const struct ht_record *r)
{
	const char *where = blocked_in(r->worst.kstack_id);
	__u32 nhops = r->worst.nhops;
	__u32 nthreads = r->nthreads;

	if (nhops > HT_MAX_HOPS)
		nhops = HT_MAX_HOPS;
	if (nthreads > HT_MAX_THREADS)
		nthreads = HT_MAX_THREADS;

	fprintf(f, "{\"frame_id\":%llu", (unsigned long long)r->frame_id);
	fprintf(f, ",\"root_pid\":%u,\"root_tgid\":%u,\"root_comm\":",
		r->root_pid, r->root_tgid);
	json_comm(f, r->root_comm);
	fprintf(f, ",\"frame_start_ns\":%llu", (unsigned long long)r->frame_start_ns);
	fprintf(f, ",\"frame_end_ns\":%llu", (unsigned long long)r->frame_end_ns);
	/* same instant as frame_end_ns, under the name the record spec uses */
	fprintf(f, ",\"t_end_ns\":%llu", (unsigned long long)r->frame_end_ns);
	fprintf(f, ",\"frame_ns\":%llu", (unsigned long long)r->frame_ns);
	fprintf(f, ",\"budget_ns\":%llu", (unsigned long long)r->budget_ns);

	fprintf(f, ",\"cause_ns\":{");
	for (__u32 i = 0; i < HT_CAUSE_MAX; i++) {
		fprintf(f, "%s", i ? "," : "");
		json_str(f, cause_names[i]);
		fprintf(f, ":%llu", (unsigned long long)r->cause_ns[i]);
	}
	fputc('}', f);

	fprintf(f, ",\"nstalls\":%u,\"flags\":", r->nstalls);
	json_flags(f, r->flags, frame_flag_names,
		   sizeof(frame_flag_names) / sizeof(frame_flag_names[0]));

	fprintf(f, ",\"worst\":{\"ns\":%llu,\"start_ns\":%llu,\"cause\":",
		(unsigned long long)r->worst.ns,
		(unsigned long long)r->worst.start_ns);
	json_str(f, r->worst.cause < HT_CAUSE_MAX ? cause_names[r->worst.cause]
						  : "unknown");
	fprintf(f, ",\"kstack_id\":%d,\"ustack_id\":%d",
		r->worst.kstack_id, r->worst.ustack_id);
	fprintf(f, ",\"blocked_in\":");
	if (where)
		json_str(f, where);
	else
		fprintf(f, "null");
	fprintf(f, ",\"kstack\":");
	json_kstack(f, r->worst.kstack_id);
	fprintf(f, ",\"ustack\":");
	json_ustack(f, r->worst.ustack_id, r->root_tgid);
	fprintf(f, ",\"hops\":[");
	for (__u32 i = 0; i < nhops; i++) {
		fprintf(f, "%s", i ? "," : "");
		json_hop(f, &r->worst.hops[i]);
	}
	fprintf(f, "]}");

	/* every thread the frame saw, in slot order, or nothing at all */
	if (nthreads) {
		fprintf(f, ",\"threads\":[");
		for (__u32 i = 0; i < nthreads; i++) {
			fprintf(f, "%s", i ? "," : "");
			json_thread(f, &r->threads[i]);
		}
		fputc(']', f);
	}

	fprintf(f, "}\n");
	fflush(f);
}

/* ---- ring buffer -------------------------------------------------------- */

static unsigned long long nrecords;
static unsigned long long nshort;

static int handle_event(void *ctx, void *data, size_t size)
{
	const struct ht_record *r = data;

	(void)ctx;
	if (size < sizeof(*r)) {
		nshort++;
		return 0;
	}
	nrecords++;
	if (!env.quiet)
		print_record(r);
	if (jsonl)
		json_record(jsonl, r);
	return 0;
}

/* ---- stats -------------------------------------------------------------- */

/* CLOCK_MONOTONIC, the clock behind bpf_ktime_get_ns() and the workload's log */
static __u64 mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (__u64)ts.tv_sec * 1000000000ULL + (__u64)ts.tv_nsec;
}

static void print_stats(struct hitchtrace_bpf *obj)
{
	static const char *const names[HT_STAT_MAX] = {
		[HT_STAT_FRAMES] = "frames",
		[HT_STAT_HITCHES] = "hitches",
		[HT_STAT_EMITTED] = "emitted",
		[HT_STAT_RINGBUF_FULL] = "ringbuf-full",
		[HT_STAT_STALLS] = "stalls",
		[HT_STAT_RESOLVED] = "resolved",
		[HT_STAT_UNRESOLVED] = "unresolved",
		[HT_STAT_STORAGE_FAIL] = "storage-fail",
		[HT_STAT_STACK_ERR] = "stack-err",
		[HT_STAT_SLOTS_FULL] = "slots-full",
	};
	__u64 totals[HT_STAT_MAX] = {};
	int ncpus = libbpf_num_possible_cpus();
	int fd = bpf_map__fd(obj->maps.stats);
	double pct;
	__u64 *vals;

	if (ncpus <= 0)
		return;
	vals = calloc(ncpus, sizeof(*vals));
	if (!vals)
		return;
	for (__u32 i = 0; i < HT_STAT_MAX; i++) {
		if (bpf_map_lookup_elem(fd, &i, vals))
			continue;
		for (int c = 0; c < ncpus; c++)
			totals[i] += vals[c];
	}
	free(vals);

	fprintf(stderr, "stats:");
	for (int i = 0; i < HT_STAT_MAX; i++)
		fprintf(stderr, " %s=%llu", names[i], (unsigned long long)totals[i]);
	fprintf(stderr, "\n");

	/* closing line of the JSONL: the trace window and the totals */
	if (jsonl) {
		fprintf(jsonl, "{\"type\":\"summary\",\"trace_end_ns\":%llu",
			(unsigned long long)mono_ns());
		for (int i = 0; i < HT_STAT_MAX; i++)
			fprintf(jsonl, ",\"%s\":%llu", names[i],
				(unsigned long long)totals[i]);
		fprintf(jsonl, ",\"records\":%llu}\n", (unsigned long long)nrecords);
		fflush(jsonl);
	}

	pct = totals[HT_STAT_FRAMES] ?
	      100.0 * (double)totals[HT_STAT_HITCHES] / (double)totals[HT_STAT_FRAMES] : 0.0;
	fprintf(stderr, "%llu frames, %llu hitches (%.1f%%), %llu records\n",
		(unsigned long long)totals[HT_STAT_FRAMES],
		(unsigned long long)totals[HT_STAT_HITCHES], pct, nrecords);

	if (totals[HT_STAT_RINGBUF_FULL])
		fprintf(stderr, "warning: %llu records dropped, ring buffer full; "
			"hitches are being missed\n",
			(unsigned long long)totals[HT_STAT_RINGBUF_FULL]);
	if (totals[HT_STAT_STORAGE_FAIL])
		fprintf(stderr, "warning: task storage unavailable for %llu events; "
			"their time is unaccounted\n",
			(unsigned long long)totals[HT_STAT_STORAGE_FAIL]);
	if (totals[HT_STAT_STACK_ERR])
		fprintf(stderr, "warning: %llu stack collection failures; some "
			"stalls have no blocking stack\n",
			(unsigned long long)totals[HT_STAT_STACK_ERR]);
	if (totals[HT_STAT_SLOTS_FULL])
		fprintf(stderr, "warning: %llu threads found no per-thread slot "
			"(more than %d threads ran); their detail is missing "
			"from the records\n",
			(unsigned long long)totals[HT_STAT_SLOTS_FULL],
			HT_MAX_THREADS);
	if (nshort)
		fprintf(stderr, "warning: %llu short ring buffer records ignored "
			"(BPF/userspace contract mismatch?)\n", nshort);
}

/* ---- target process ----------------------------------------------------- */

static bool target_alive(pid_t tgid)
{
	char path[64], buf[256], *p;
	bool alive = true;
	FILE *f;

	if (kill(tgid, 0) < 0 && errno == ESRCH)
		return false;
	snprintf(path, sizeof(path), "/proc/%d/stat", (int)tgid);
	f = fopen(path, "r");
	if (!f)
		return false;
	/* comm is parenthesized and can contain anything, including ') ' */
	if (fgets(buf, sizeof(buf), f)) {
		p = strrchr(buf, ')');
		if (p && p[1] == ' ' && (p[2] == 'Z' || p[2] == 'X'))
			alive = false;
	}
	fclose(f);
	return alive;
}

/* The path libbpf should parse for the marker symbol. */
static const char *resolve_binary(pid_t tgid, char *buf, size_t sz)
{
	char link[64];
	ssize_t n;

	if (env.binary)
		return env.binary;
	snprintf(link, sizeof(link), "/proc/%d/exe", (int)tgid);
	n = readlink(link, buf, sz - 1);
	if (n > 0) {
		buf[n] = '\0';
		if (!access(buf, R_OK))
			return buf;
	}
	/* unreadable, replaced or deleted: the /proc link still resolves */
	snprintf(buf, sz, "%s", link);
	return buf;
}

/* ---- load and attach ---------------------------------------------------- */

/* fentry/fexit pairs that tell BPF when a wakeup happens on interrupt exit */
struct irq_hook {
	const char *func;
	bool available;
	struct bpf_link *enter_link;
	struct bpf_link *leave_link;
};

static struct irq_hook irq_hooks[] = {
	{ .func = "irq_exit_rcu" },
	{ .func = "irq_exit" },
};

static void irq_hook_progs(struct hitchtrace_bpf *obj, size_t i,
			   struct bpf_program **enter, struct bpf_program **leave)
{
	*enter = i == 0 ? obj->progs.on_irq_exit_rcu_enter : obj->progs.on_irq_exit_enter;
	*leave = i == 0 ? obj->progs.on_irq_exit_rcu_leave : obj->progs.on_irq_exit_leave;
}

static struct hitchtrace_bpf *open_and_load(bool with_irq_hooks, int *errp)
{
	struct hitchtrace_bpf *obj;
	struct bpf_program *enter, *leave;
	int err;

	obj = hitchtrace_bpf__open();
	if (!obj) {
		*errp = -errno;
		fprintf(stderr, "failed to open BPF object: %s\n", strerror(errno));
		return NULL;
	}

	obj->rodata->targ_tgid = env.tgid;
	obj->rodata->budget_ns = env.budget_us * 1000;
	obj->rodata->min_stall_ns = env.min_stall_us * 1000;
	obj->rodata->max_hops = env.hops;
	obj->rodata->kernel_stacks = true;
	obj->rodata->user_stacks = env.user_stacks;

	/* the marker uprobe needs a binary and a pid, so it is attached by hand */
	bpf_program__set_autoattach(obj->progs.on_frame_mark, false);

	/* attached by hand after load, so a missing hook is not fatal */
	for (size_t i = 0; i < sizeof(irq_hooks) / sizeof(irq_hooks[0]); i++) {
		irq_hook_progs(obj, i, &enter, &leave);
		bpf_program__set_autoattach(enter, false);
		bpf_program__set_autoattach(leave, false);
		if (!with_irq_hooks || !irq_hooks[i].available) {
			bpf_program__set_autoload(enter, false);
			bpf_program__set_autoload(leave, false);
		}
	}

	err = hitchtrace_bpf__load(obj);
	if (err) {
		hitchtrace_bpf__destroy(obj);
		*errp = err;
		return NULL;
	}
	return obj;
}

static size_t roundup_pow2(size_t v)
{
	size_t r = 1;

	while (r < v)
		r <<= 1;
	return r;
}

int main(int argc, char **argv)
{
	static const struct argp argp = {
		.options = opts,
		.parser = parse_arg,
		.doc = argp_program_doc,
	};
	struct hitchtrace_bpf *obj = NULL;
	struct bpf_program *enter, *leave;
	struct bpf_link *frame_link = NULL, *present_link = NULL;
	struct ring_buffer *rb = NULL;
	struct btf *vmlinux_btf;
	const char *binary;
	char binbuf[PATH_MAX];
	bool any_hooks = false;
	double deadline = 0;
	struct timespec now;
	sigset_t sigs;
	int err, poll_err = 0;

	err = argp_parse(&argp, argc, argv, 0, NULL, NULL);
	if (err)
		return err;

	if (!target_alive(env.tgid)) {
		fprintf(stderr, "no such process: %d\n", (int)env.tgid);
		return 1;
	}
	binary = resolve_binary(env.tgid, binbuf, sizeof(binbuf));

	if (env.jsonl) {
		jsonl = fopen(env.jsonl, "a");
		if (!jsonl) {
			fprintf(stderr, "cannot open %s: %s\n", env.jsonl,
				strerror(errno));
			return 1;
		}
		/* opening line: when tracing started, so a joiner can tell
		 * which frames were actually covered */
		fprintf(jsonl, "{\"type\":\"header\",\"trace_start_ns\":%llu,"
			"\"pid\":%d,\"budget_ns\":%llu}\n",
			(unsigned long long)mono_ns(), (int)env.tgid,
			(unsigned long long)(env.budget_us * 1000));
		fflush(jsonl);
	}

	libbpf_set_print(libbpf_print_fn);

	vmlinux_btf = btf__load_vmlinux_btf();
	for (size_t i = 0; i < sizeof(irq_hooks) / sizeof(irq_hooks[0]); i++) {
		irq_hooks[i].available = vmlinux_btf &&
			btf__find_by_name_kind(vmlinux_btf, irq_hooks[i].func,
					       BTF_KIND_FUNC) >= 0;
		any_hooks |= irq_hooks[i].available;
	}
	btf__free(vmlinux_btf);

	obj = open_and_load(any_hooks, &err);
	if (!obj && any_hooks && err != -EPERM) {
		fprintf(stderr, "retrying without irq_exit hooks\n");
		obj = open_and_load(false, &err);
		for (size_t i = 0; i < sizeof(irq_hooks) / sizeof(irq_hooks[0]); i++)
			irq_hooks[i].available = false;
	}
	if (!obj) {
		fprintf(stderr, "failed to load BPF programs: %s%s\n",
			strerror(-err),
			err == -EPERM ? " (needs root: CAP_BPF + CAP_PERFMON)" : "");
		err = 1;
		goto cleanup;
	}

	kstacks.fd = bpf_map__fd(obj->maps.kstacks);
	ustacks.fd = bpf_map__fd(obj->maps.ustacks);
	kstacks.sz = roundup_pow2(bpf_map__max_entries(obj->maps.kstacks));
	ustacks.sz = roundup_pow2(bpf_map__max_entries(obj->maps.ustacks));
	kstacks.cache = calloc(kstacks.sz, sizeof(*kstacks.cache));
	ustacks.cache = calloc(ustacks.sz, sizeof(*ustacks.cache));
	if (!kstacks.cache || !ustacks.cache) {
		fprintf(stderr, "out of memory\n");
		err = 1;
		goto cleanup;
	}

	/* kallsyms problems should show up now, not after a long trace */
	ksyms = ksyms_load();
	if (!ksyms)
		fprintf(stderr, "warning: cannot read kernel symbols from "
			"/proc/kallsyms; blocking stacks will be [unknown]\n");
	usyms = usyms_new();

	rb = ring_buffer__new(bpf_map__fd(obj->maps.events), handle_event, NULL, NULL);
	if (!rb) {
		fprintf(stderr, "failed to open the events ring buffer: %s\n",
			strerror(errno));
		err = 1;
		goto cleanup;
	}

	/* irq_exit hooks first, so wakeups are classified from the start */
	for (size_t i = 0; i < sizeof(irq_hooks) / sizeof(irq_hooks[0]); i++) {
		struct irq_hook *h = &irq_hooks[i];

		if (!h->available)
			continue;
		/*
		 * leave before enter: an irq_exit that begins between the two
		 * attaches must not be counted in without being counted out
		 */
		irq_hook_progs(obj, i, &enter, &leave);
		h->leave_link = bpf_program__attach(leave);
		h->enter_link = h->leave_link ? bpf_program__attach(enter) : NULL;
		if (!h->enter_link || !h->leave_link) {
			bpf_link__destroy(h->leave_link);
			h->leave_link = NULL;
			if (env.verbose)
				fprintf(stderr, "cannot hook %s; interrupt-exit "
					"wakeups will not be flagged\n", h->func);
		}
	}

	/* signals are taken synchronously below; a second one kills us later */
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGINT);
	sigaddset(&sigs, SIGTERM);
	sigprocmask(SIG_BLOCK, &sigs, NULL);

	err = hitchtrace_bpf__attach(obj);
	if (err) {
		fprintf(stderr, "failed to attach BPF programs: %s\n", strerror(-err));
		err = 1;
		goto cleanup;
	}

	/*
	 * The present-return marker first, so the bracket can never be opened
	 * without a probe able to close it: the BPF side only starts marking
	 * present time once we tell it the probe is there.
	 */
	if (env.present_marker && env.present_marker[0]) {
		LIBBPF_OPTS(bpf_uprobe_opts, popts,
			    .retprobe = false,
			    .func_name = env.present_marker);

		present_link = bpf_program__attach_uprobe_opts(obj->progs.on_present_end,
							       env.tgid, binary, 0,
							       &popts);
		if (present_link) {
			obj->bss->have_present_marker = true;
		} else if (!env.quiet) {
			fprintf(stderr, "note: %s() not found in %s; time inside "
				"present will be named by what it blocked in "
				"instead\n", env.present_marker, binary);
		}
	}

	/* the marker goes last: no frame opens before the scheduler hooks live */
	{
		LIBBPF_OPTS(bpf_uprobe_opts, uopts,
			    .retprobe = false,
			    .func_name = env.marker);

		frame_link = bpf_program__attach_uprobe_opts(obj->progs.on_frame_mark,
							     env.tgid, binary, 0,
							     &uopts);
		if (!frame_link) {
			err = -errno;
			fprintf(stderr, "cannot probe %s() in %s: %s\n",
				env.marker, binary, strerror(-err));
			if (err == -ENOENT)
				fprintf(stderr, "the symbol was not found; check "
					"'nm -C %s | grep %s' (a stripped or "
					"inlined marker cannot be probed)\n",
					binary, env.marker);
			err = 1;
			goto cleanup;
		}
	}

	if (!env.quiet) {
		fprintf(stderr, "Tracing frames of PID %d (marker %s, budget %.1f ms)... ",
			(int)env.tgid, env.marker, ms(env.budget_us * 1000));
		if (env.duration)
			fprintf(stderr, "for %ld seconds, or ", env.duration);
		fprintf(stderr, "hit Ctrl-C to end.\n");
	}

	if (env.duration) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		deadline = now.tv_sec + env.duration + now.tv_nsec / 1e9;
	}

	for (;;) {
		static const struct timespec nowait = {};
		int n;

		n = ring_buffer__poll(rb, POLL_TIMEOUT_MS);
		if (n < 0 && n != -EINTR) {
			fprintf(stderr, "ring buffer poll failed: %s\n", strerror(-n));
			poll_err = n;
			break;
		}
		if (sigtimedwait(&sigs, NULL, &nowait) >= 0)
			break;
		if (env.duration) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			if (now.tv_sec + now.tv_nsec / 1e9 >= deadline)
				break;
		}
		if (!target_alive(env.tgid)) {
			if (!env.quiet)
				fprintf(stderr, "target %d exited\n", (int)env.tgid);
			break;
		}
	}
	sigprocmask(SIG_UNBLOCK, &sigs, NULL);

	/* stop new frames, then drain what the kernel already emitted */
	bpf_link__destroy(frame_link);
	frame_link = NULL;
	bpf_link__destroy(present_link);
	present_link = NULL;
	ring_buffer__consume(rb);
	hitchtrace_bpf__detach(obj);
	for (size_t i = 0; i < sizeof(irq_hooks) / sizeof(irq_hooks[0]); i++) {
		bpf_link__destroy(irq_hooks[i].enter_link);
		bpf_link__destroy(irq_hooks[i].leave_link);
		irq_hooks[i].enter_link = irq_hooks[i].leave_link = NULL;
	}

	fflush(stdout);
	print_stats(obj);
	err = poll_err ? 1 : 0;

cleanup:
	bpf_link__destroy(frame_link);
	bpf_link__destroy(present_link);
	for (size_t i = 0; i < sizeof(irq_hooks) / sizeof(irq_hooks[0]); i++) {
		bpf_link__destroy(irq_hooks[i].enter_link);
		bpf_link__destroy(irq_hooks[i].leave_link);
	}
	ring_buffer__free(rb);
	for (struct stack_map *m = &kstacks; m; m = (m == &kstacks) ? &ustacks : NULL) {
		if (!m->cache)
			continue;
		for (size_t i = 0; i < m->sz; i++)
			free(m->cache[i].ips);
		free(m->cache);
		m->cache = NULL;
	}
	if (usyms)
		usyms_free(usyms);
	if (ksyms)
		ksyms_free(ksyms);
	if (jsonl)
		fclose(jsonl);
	hitchtrace_bpf__destroy(obj);
	return err != 0;
}
