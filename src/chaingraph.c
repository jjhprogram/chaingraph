// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/*
 * chaingraph  Off-CPU time with the full chain of wakeups that ended each
 *             sleep: who woke the task, who woke the waker, and so on,
 *             until the chain reaches an interrupt.
 *
 * A libbpf/CO-RE take on the 2016 bcc prototype by Brendan Gregg:
 * https://www.brendangregg.com/blog/2016-02-05/ebpf-chaingraph-prototype.html
 *
 * USAGE: chaingraph [-h] [-p PID | -t TID] [-d DEPTH] [-f] [duration]
 */
#include <argp.h>
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

#include "chaingraph.h"
#include "chaingraph.skel.h"
#include "syms.h"

static struct env {
	pid_t tgid;
	pid_t pid;
	char comm[TASK_COMM_LEN];
	bool filter_comm;
	bool user_threads_only;
	bool kernel_threads_only;
	bool user_stacks_only;
	bool kernel_stacks_only;
	int depth;
	__u64 min_block_us;
	__u64 max_block_us;
	__u32 state_mask;
	bool folded;
	bool per_thread;
	int softirq;		/* CG_SOFTIRQ_* */
	int stack_storage_size;
	int perf_max_stack_depth;
	int max_chains;
	bool verbose;
	long duration;
} env = {
	.depth = 4,
	.min_block_us = 1,
	.max_block_us = (__u64)-1,
	.softirq = CG_SOFTIRQ_AUTO,
	.stack_storage_size = 32768,
	.perf_max_stack_depth = PERF_MAX_STACK_DEPTH,
	.max_chains = 65536,
};

/* largest microsecond value whose nanoseconds fit in 64 bits */
#define MAX_BLOCK_US	((long)(UINT64_MAX / 1000))

enum {
	OPT_STATE = 0x100,
	OPT_SOFTIRQ,
	OPT_STACK_STORAGE_SIZE,
	OPT_PERF_MAX_STACK_DEPTH,
	OPT_MAX_CHAINS,
};

const char *argp_program_version = "chaingraph 0.1";
static const char argp_program_doc[] =
"Summarize off-CPU time by blocked stack and the chain of wakeups that\n"
"ended each sleep (who woke the task, who woke the waker, ...).\n"
"\n"
"USAGE: chaingraph [OPTION...] [DURATION]\n"
"\n"
"EXAMPLES:\n"
"    chaingraph                 # trace until Ctrl-C\n"
"    chaingraph 5               # trace for 5 seconds\n"
"    chaingraph -f 5 | FlameGraph/flamegraph.pl --colors=chain \\\n"
"        --countname=us > chain.svg\n"
"    chaingraph -p 185 -d 6     # PID 185 only, up to 6 waker levels\n"
"    chaingraph -m 1000         # only sleeps of at least 1 ms\n"
"    chaingraph -K --state 2    # kernel stacks, uninterruptible sleeps\n";

static const struct argp_option opts[] = {
	{ "pid", 'p', "PID", 0, "Only show sleeps of this process", 0 },
	{ "tid", 't', "TID", 0, "Only show sleeps of this thread", 0 },
	{ "comm", 'c', "COMM", 0, "Only show sleeps of tasks with this name", 0 },
	{ "user-threads-only", 'u', NULL, 0, "Only show sleeps of user threads", 0 },
	{ "kernel-threads-only", 'k', NULL, 0, "Only show sleeps of kernel threads", 0 },
	{ "user-stacks-only", 'U', NULL, 0, "Collect user stacks only", 0 },
	{ "kernel-stacks-only", 'K', NULL, 0, "Collect kernel stacks only", 0 },
	{ "depth", 'd', "N", 0, "Waker levels to record, 1-8 (default 4)", 0 },
	{ "min-block", 'm', "USEC", 0, "Ignore sleeps shorter than this", 0 },
	{ "max-block", 'M', "USEC", 0, "Ignore sleeps longer than this", 0 },
	{ "state", OPT_STATE, "MASK", 0,
	  "Only sleeps in these states: 1 = S (interruptible), "
	  "2 = D (uninterruptible), 4 = I (idle), 8 = other", 0 },
	{ "folded", 'f', NULL, 0, "Folded output for flamegraph.pl --colors=chain", 0 },
	{ "per-thread", 'P', NULL, 0, "Keep thread ids (labels become comm/TID)", 0 },
	{ "softirq", OPT_SOFTIRQ, "auto|keep|cut", 0,
	  "Continue chains past softirq-context wakers? auto keeps the chain "
	  "unless the softirq ran on interrupt exit (default auto)", 0 },
	{ "stack-storage-size", OPT_STACK_STORAGE_SIZE, "N", 0,
	  "Entries in each of the kernel and user stack maps (default 32768; "
	  "about 1 KB each at 127 frames)", 0 },
	{ "perf-max-stack-depth", OPT_PERF_MAX_STACK_DEPTH, "N", 0,
	  "Frames per stack (default 127, capped by "
	  "kernel.perf_event_max_stack); lower bounds unwind cost", 0 },
	{ "max-chains", OPT_MAX_CHAINS, "N", 0,
	  "Distinct chains to track (default 65536)", 0 },
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
	case 'p':
		env.tgid = parse_long(arg, 1, INT_MAX, state, "PID");
		break;
	case 't':
		env.pid = parse_long(arg, 1, INT_MAX, state, "TID");
		break;
	case 'c':
		if (strlen(arg) >= TASK_COMM_LEN) {
			fprintf(stderr, "comm too long (max %d chars): %s\n",
				TASK_COMM_LEN - 1, arg);
			argp_usage(state);
		}
		strcpy(env.comm, arg);
		env.filter_comm = true;
		break;
	case 'u':
		env.user_threads_only = true;
		break;
	case 'k':
		env.kernel_threads_only = true;
		break;
	case 'U':
		env.user_stacks_only = true;
		break;
	case 'K':
		env.kernel_stacks_only = true;
		break;
	case 'd':
		env.depth = parse_long(arg, 1, MAX_CHAIN_DEPTH, state, "depth");
		break;
	case 'm':
		env.min_block_us = parse_long(arg, 0, MAX_BLOCK_US, state,
					      "min block time");
		break;
	case 'M':
		env.max_block_us = parse_long(arg, 0, MAX_BLOCK_US, state,
					      "max block time");
		break;
	case OPT_STATE:
		env.state_mask = parse_long(arg, 1, CG_STATE_S | CG_STATE_D |
					    CG_STATE_I | CG_STATE_OTHER,
					    state, "state mask");
		break;
	case 'f':
		env.folded = true;
		break;
	case 'P':
		env.per_thread = true;
		break;
	case OPT_SOFTIRQ:
		if (!strcmp(arg, "auto")) {
			env.softirq = CG_SOFTIRQ_AUTO;
		} else if (!strcmp(arg, "keep")) {
			env.softirq = CG_SOFTIRQ_KEEP;
		} else if (!strcmp(arg, "cut")) {
			env.softirq = CG_SOFTIRQ_CUT;
		} else {
			fprintf(stderr, "invalid --softirq: %s\n", arg);
			argp_usage(state);
		}
		break;
	case OPT_STACK_STORAGE_SIZE:
		env.stack_storage_size = parse_long(arg, 1, 1 << 24, state,
						    "stack storage size");
		break;
	case OPT_PERF_MAX_STACK_DEPTH:
		env.perf_max_stack_depth = parse_long(arg, 1, 1024, state,
						      "perf max stack depth");
		break;
	case OPT_MAX_CHAINS:
		env.max_chains = parse_long(arg, 1, 1 << 24, state, "max chains");
		break;
	case ARGP_KEY_ARG:
		if (state->arg_num != 0) {
			fprintf(stderr, "unrecognized positional argument: %s\n", arg);
			argp_usage(state);
		}
		env.duration = parse_long(arg, 1, LONG_MAX, state, "duration");
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

static void *xrealloc(void *p, size_t sz)
{
	p = realloc(p, sz);
	if (!p) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	return p;
}

/* ---- folded string builder -------------------------------------------- */

struct sbuf {
	char *buf;
	size_t len;
	size_t cap;
};

static void sb_reserve(struct sbuf *sb, size_t more)
{
	size_t cap = sb->cap ? sb->cap : 256;

	if (sb->len + more + 1 <= sb->cap)
		return;
	while (cap < sb->len + more + 1)
		cap *= 2;
	sb->buf = xrealloc(sb->buf, cap);
	sb->cap = cap;
}

static bool is_marker(const char *frame);

/*
 * Append one frame. ';' separates frames, and "-", "--" and the markers are
 * structure, so names coming from tasks or symbols can't be exactly those.
 */
static void sb_frame(struct sbuf *sb, const char *name)
{
	char buf[64];
	size_t n;

	if (!name || !*name)
		name = "[empty]";
	if (!strcmp(name, "-") || !strcmp(name, "--") || is_marker(name)) {
		snprintf(buf, sizeof(buf), "'%s'", name);
		name = buf;
	}
	n = strlen(name);
	sb_reserve(sb, n + 1);
	if (sb->len)
		sb->buf[sb->len++] = ';';
	for (size_t i = 0; i < n; i++) {
		char c = name[i];

		sb->buf[sb->len++] = (c == ';' || c == '\n' || c == '\r') ? ':' : c;
	}
	sb->buf[sb->len] = '\0';
}

/* Append a separator, marker or other frame generated by this tool. */
static void sb_raw(struct sbuf *sb, const char *frame)
{
	size_t n = strlen(frame);

	sb_reserve(sb, n + 1);
	if (sb->len)
		sb->buf[sb->len++] = ';';
	memcpy(sb->buf + sb->len, frame, n);
	sb->len += n;
	sb->buf[sb->len] = '\0';
}

/* Append all frames of @src. */
static void sb_frames(struct sbuf *sb, const struct sbuf *src)
{
	if (!src->len)
		return;
	sb_reserve(sb, src->len + 1);
	if (sb->len)
		sb->buf[sb->len++] = ';';
	memcpy(sb->buf + sb->len, src->buf, src->len);
	sb->len += src->len;
	sb->buf[sb->len] = '\0';
}

static void sb_clear(struct sbuf *sb)
{
	sb->len = 0;
	if (sb->buf)
		sb->buf[0] = '\0';
}

/* ---- stacks and symbols ------------------------------------------------ */

static struct ksyms *ksyms;
static struct usyms *usyms;
/* whether fentry/fexit on irq_exit_rcu() are feeding LINK_IRQEXIT */
static bool irq_exit_hooked;

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

	if (id < 0 || (size_t)id >= m->sz)
		return NULL;
	s = &m->cache[id];
	if (!s->state) {
		ips = calloc(env.perf_max_stack_depth, sizeof(*ips));
		if (!ips || bpf_map_lookup_elem(m->fd, &key, ips)) {
			free(ips);
			s->state = -1;
			return NULL;
		}
		while (s->nr < env.perf_max_stack_depth && ips[s->nr])
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

/* Frames of the tracing machinery itself, at the leaf end of kernel stacks. */
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

/* Frames showing that a softirq ran on the way out of a hardware interrupt. */
static bool is_irq_exit_frame(const char *name)
{
	return has_prefix(name, "irq_exit") || has_prefix(name, "__irq_exit") ||
	       strstr(name, "sysvec_") || strstr(name, "common_interrupt") ||
	       has_prefix(name, "el1_interrupt") ||
	       has_prefix(name, "el0_interrupt") ||
	       has_prefix(name, "gic_handle_irq");
}

/* Outermost frame of an interrupt; below it is whatever was interrupted. */
static bool is_irq_entry_frame(const char *name)
{
	return has_prefix(name, "asm_sysvec_") ||
	       has_prefix(name, "asm_common_interrupt") ||
	       has_prefix(name, "asm_fred_entrypoint") ||
	       has_prefix(name, "el1h_64_irq") || has_prefix(name, "el0t_64_irq");
}

/*
 * Append a kernel stack, leaf first or root first. Returns frames added.
 * -1 means "not collected"; other negative ids are collection errors.
 * With @irq_only, frames below the interrupt entry (the interrupted
 * context, unrelated to the wakeup) are dropped.
 */
static int add_kstack(struct sbuf *sb, __s32 id, bool leaf_first, bool irq_only)
{
	const struct stack *s;
	int lo = 0, hi, i, added = 0;

	if (id == -1)
		return 0;
	s = get_stack(&kstacks, id);
	if (!s) {
		sb_raw(sb, "[missing kernel stack]");
		return 1;
	}
	hi = s->nr;	/* exclusive */
	if (irq_only) {
		for (i = lo; i < s->nr; i++) {
			if (is_irq_entry_frame(ksym_name(s->ips[i]))) {
				hi = i + 1;
				break;
			}
		}
	}
	/* the tracer's own frames (e.g. the irq_exit_rcu trampoline) go */
	for (int k = 0, j; k < hi - lo; k++) {
		j = leaf_first ? lo + k : hi - 1 - k;
		if (ksyms && is_tracing_frame(ksym_name(s->ips[j])))
			continue;
		sb_frame(sb, ksym_name(s->ips[j]));
		added++;
	}
	return added;
}

static int add_ustack(struct sbuf *sb, __s32 id, int tgid, bool leaf_first)
{
	const struct stack *s;
	char buf[256];
	int i;

	if (id == -1)
		return 0;
	s = get_stack(&ustacks, id);
	if (!s) {
		sb_raw(sb, "[missing user stack]");
		return 1;
	}
	if (leaf_first)
		for (i = 0; i < s->nr; i++)
			sb_frame(sb, usym_name(tgid, s->ips[i], buf, sizeof(buf)));
	else
		for (i = s->nr - 1; i >= 0; i--)
			sb_frame(sb, usym_name(tgid, s->ips[i], buf, sizeof(buf)));
	return s->nr;
}

/*
 * Was this waker merely the interrupted task, so the chain ends here and its
 * identity is meaningless? Matches the BPF program's decision, except that
 * without the irq_exit hooks auto mode can only tell afterwards, by looking
 * for interrupt-exit frames in the waker's kernel stack (BPF then kept the
 * interrupted task's chain, which is dropped here).
 */
static bool link_interrupted(const struct chain_link *l)
{
	const struct stack *s;

	if (l->flags & (LINK_HARDIRQ | LINK_NMI))
		return true;
	if (l->flags & LINK_IRQEXIT)
		return !((l->flags & LINK_SOFTIRQ) && env.softirq == CG_SOFTIRQ_KEEP);
	if (!(l->flags & LINK_SOFTIRQ) || env.softirq == CG_SOFTIRQ_KEEP)
		return false;
	if (env.softirq == CG_SOFTIRQ_CUT)
		return true;
	if (irq_exit_hooked)
		return false;
	s = ksyms ? get_stack(&kstacks, l->kstack_id) : NULL;
	if (!s)	/* can't tell: trust ksoftirqd, distrust everyone else */
		return strncmp(l->comm, "ksoftirqd/", 10) != 0;
	for (int i = 0; i < s->nr; i++)
		if (is_irq_exit_frame(ksym_name(s->ips[i])))
			return true;
	return false;
}

static void task_label(char *buf, size_t sz, const char *comm, __u32 pid,
		       const char *suffix)
{
	char name[TASK_COMM_LEN + 1];

	memcpy(name, comm, TASK_COMM_LEN);
	name[TASK_COMM_LEN] = '\0';
	if (!name[0]) {
		snprintf(buf, sz, "[no comm]%s", suffix);
		return;
	}
	/* a task can name itself "[hardirq]"; don't let it pose as one */
	if (env.per_thread)
		snprintf(buf, sz, name[0] == '[' ? "'%s'/%u%s" : "%s/%u%s",
			 name, pid, suffix);
	else
		snprintf(buf, sz, name[0] == '[' ? "'%s'%s" : "%s%s", name, suffix);
}

/*
 * Render a chain in folded form, bottom (target) to top (oldest waker):
 *   target;ustack(root..leaf);-;kstack(root..leaf)
 *   ;--;kstack(leaf..root);-;ustack(leaf..root);waker ...
 */
static void render_chain(const struct chain_key *k, struct sbuf *out)
{
	struct sbuf ks = {}, us = {};
	char label[64];

	sb_clear(out);
	task_label(label, sizeof(label), k->comm, k->pid, "");
	sb_frame(out, label);
	add_ustack(&us, k->ustack_id, k->tgid, false);
	add_kstack(&ks, k->kstack_id, false, false);
	sb_frames(out, &us);
	if (us.len && ks.len)
		sb_raw(out, "-");
	sb_frames(out, &ks);

	if (k->flags & KEY_NO_WAKER) {
		sb_raw(out, "--");
		sb_raw(out, "[unknown waker]");
		goto out;
	}

	for (__u32 i = 0; i < k->depth && i < MAX_CHAIN_DEPTH; i++) {
		const struct chain_link *l = &k->links[i];
		bool cut, interrupt;
		char suffix[32];

		if (!(l->flags & LINK_VALID))
			break;
		interrupt = link_interrupted(l);
		cut = interrupt || (l->flags & LINK_IDLE);

		sb_clear(&ks);
		sb_clear(&us);
		sb_raw(out, "--");
		add_kstack(&ks, l->kstack_id, true, interrupt);
		if (!interrupt)	/* an interrupted task's user stack is noise */
			add_ustack(&us, l->ustack_id, l->tgid, true);
		sb_frames(out, &ks);
		if (ks.len && us.len)
			sb_raw(out, "-");
		sb_frames(out, &us);

		snprintf(suffix, sizeof(suffix), "%s%s%s",
			 (l->flags & LINK_SOFTIRQ) ? " [softirq]" : "",
			 (l->flags & LINK_FORK) ? " [fork]" : "",
			 (l->flags & LINK_THAW) ? " [thaw]" : "");
		if (l->flags & LINK_NMI)
			sb_raw(out, "[nmi]");
		else if (l->flags & LINK_HARDIRQ)
			sb_raw(out, "[hardirq]");
		else if (interrupt && (l->flags & LINK_SOFTIRQ))
			sb_raw(out, "[softirq]");
		else if (interrupt)
			sb_raw(out, "[irq exit]");
		else {
			task_label(label, sizeof(label), l->comm, l->pid, suffix);
			sb_frame(out, label);
		}

		if (cut)
			break;
		if (l->flags & LINK_TORN) {
			sb_raw(out, "--");
			sb_raw(out, "[chain torn]");
			break;
		}
		if (l->flags & LINK_TRUNC) {
			sb_raw(out, "--");
			sb_raw(out, "[chain truncated]");
			break;
		}
	}
out:
	free(ks.buf);
	free(us.buf);
}

/* ---- collection and output --------------------------------------------- */

struct entry {
	char *folded;
	__u64 total_ns;
	__u64 count;
};

static int cmp_folded(const void *a, const void *b)
{
	return strcmp(((const struct entry *)a)->folded,
		      ((const struct entry *)b)->folded);
}

static int cmp_total(const void *a, const void *b)
{
	const struct entry *x = a, *y = b;

	if (x->total_ns != y->total_ns)
		return x->total_ns < y->total_ns ? -1 : 1;
	return strcmp(x->folded, y->folded);
}

static size_t collect(int chains_fd, struct entry **entriesp)
{
	struct chain_key key, next;
	struct chain_val val;
	struct entry *entries = NULL;
	struct sbuf sb = {};
	size_t n = 0, cap = 0, out;
	bool first = true;

	while (!bpf_map_get_next_key(chains_fd, first ? NULL : &key, &next)) {
		first = false;
		key = next;
		if (bpf_map_lookup_elem(chains_fd, &key, &val))
			continue;
		render_chain(&key, &sb);
		if (n == cap) {
			cap = cap ? cap * 2 : 1024;
			entries = xrealloc(entries, cap * sizeof(*entries));
		}
		entries[n].folded = strdup(sb.buf);
		if (!entries[n].folded) {
			fprintf(stderr, "out of memory\n");
			exit(1);
		}
		entries[n].total_ns = val.total_ns;
		entries[n].count = val.count;
		n++;
	}
	free(sb.buf);

	/* distinct keys can render identically, e.g. once pids are dropped */
	qsort(entries, n, sizeof(*entries), cmp_folded);
	for (size_t i = out = 0; i < n; i++) {
		if (out && !strcmp(entries[out - 1].folded, entries[i].folded)) {
			entries[out - 1].total_ns += entries[i].total_ns;
			entries[out - 1].count += entries[i].count;
			free(entries[i].folded);
			continue;
		}
		entries[out++] = entries[i];
	}
	qsort(entries, out, sizeof(*entries), cmp_total);
	*entriesp = entries;
	return out;
}

static bool is_marker(const char *frame)
{
	return !strcmp(frame, "[chain truncated]") ||
	       !strcmp(frame, "[chain torn]") ||
	       !strcmp(frame, "[unknown waker]");
}

/*
 * Human-readable form of one chain, printed top-down like the flame graph:
 * oldest waker first, the blocked task and its time last.
 */
static void print_entry(const struct entry *e)
{
	char *dup = strdup(e->folded), *save = NULL, *tok;
	char **frames = NULL;
	size_t nframes = 0, cap = 0;
	size_t seg_start[MAX_CHAIN_DEPTH + 3];
	int nseg = 0;

	if (!dup)
		return;
	for (tok = strtok_r(dup, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
		if (nframes == cap) {
			cap = cap ? cap * 2 : 64;
			frames = xrealloc(frames, cap * sizeof(*frames));
		}
		frames[nframes++] = tok;
	}
	if (!nframes)
		goto out;

	seg_start[nseg++] = 0;
	for (size_t i = 0; i < nframes; i++)
		if (!strcmp(frames[i], "--") &&
		    nseg < (int)(sizeof(seg_start) / sizeof(seg_start[0])))
			seg_start[nseg++] = i + 1;

	for (int s = nseg - 1; s >= 0; s--) {
		size_t a = seg_start[s];
		size_t b = (s == nseg - 1) ? nframes : seg_start[s + 1] - 1;

		if (s == 0) {
			for (size_t i = b; i-- > a + 1;)
				printf("        %s\n", frames[i]);
			printf("    %-9s %s\n", "target:", frames[a]);
			break;
		}
		if (b - a == 1 && is_marker(frames[a])) {
			printf("    %s\n", frames[a]);
		} else if (b > a) {
			printf("    waker %d: %s\n", s, frames[b - 1]);
			for (size_t i = b - 1; i-- > a;)
				printf("        %s\n", frames[i]);
		}
		printf("    --\n");
	}
	printf("        %llu us over %llu sleep%s\n\n",
	       (unsigned long long)(e->total_ns / 1000),
	       (unsigned long long)e->count, e->count == 1 ? "" : "s");
out:
	free(frames);
	free(dup);
}

static void print_stats(struct chaingraph_bpf *obj, size_t nchains)
{
	static const char *const names[STAT_MAX] = {
		[STAT_BLOCKED] = "sleeps",
		[STAT_ACCOUNTED] = "accounted",
		[STAT_WAKEUPS] = "wakeups",
		[STAT_WAKEUPS_IRQ] = "irq-wakeups",
		[STAT_WAKEUPS_IRQEXIT] = "irq-exit-wakeups",
		[STAT_WAKEUPS_SOFTIRQ] = "softirq-wakeups",
		[STAT_FORKS] = "forks",
		[STAT_NO_WAKER] = "no-waker",
		[STAT_FILTERED_TIME] = "time-filtered",
		[STAT_TORN] = "torn",
		[STAT_IRQEXIT_RESYNC] = "irq-exit-resync",
		[STAT_STORAGE_FAIL] = "storage-fail",
		[STAT_KSTACK_COLLIDE] = "kstack-collide",
		[STAT_USTACK_COLLIDE] = "ustack-collide",
		[STAT_KSTACK_ERR] = "kstack-err",
		[STAT_USTACK_ERR] = "ustack-err",
		[STAT_CHAINS_FULL] = "chains-full",
		[STAT_CHAINS_ERR] = "chains-err",
	};
	__u64 totals[STAT_MAX] = {};
	int ncpus = libbpf_num_possible_cpus();
	int fd = bpf_map__fd(obj->maps.stats);
	__u64 *vals;

	if (ncpus <= 0)
		return;
	vals = calloc(ncpus, sizeof(*vals));
	if (!vals)
		return;
	for (__u32 i = 0; i < STAT_MAX; i++) {
		if (bpf_map_lookup_elem(fd, &i, vals))
			continue;
		for (int c = 0; c < ncpus; c++)
			totals[i] += vals[c];
	}
	free(vals);

	fprintf(stderr, "chains: %zu, stats:", nchains);
	for (int i = 0; i < STAT_MAX; i++)
		fprintf(stderr, " %s=%llu", names[i], (unsigned long long)totals[i]);
	fprintf(stderr, "\n");
	if (totals[STAT_KSTACK_COLLIDE] || totals[STAT_USTACK_COLLIDE])
		fprintf(stderr, "warning: %llu stacks lost to stack map hash "
			"collisions; a larger --stack-storage-size reduces them\n",
			(unsigned long long)(totals[STAT_KSTACK_COLLIDE] +
					     totals[STAT_USTACK_COLLIDE]));
	if (totals[STAT_CHAINS_FULL])
		fprintf(stderr, "warning: chains map full, %llu sleeps dropped; try a "
			"larger --max-chains or a shorter duration\n",
			(unsigned long long)totals[STAT_CHAINS_FULL]);
	if (totals[STAT_CHAINS_ERR])
		fprintf(stderr, "warning: %llu sleeps dropped by chains map update "
			"errors (allocation or lock contention)\n",
			(unsigned long long)totals[STAT_CHAINS_ERR]);
	if (totals[STAT_STORAGE_FAIL])
		fprintf(stderr, "warning: task storage unavailable for %llu "
			"events; their chains are missing\n",
			(unsigned long long)totals[STAT_STORAGE_FAIL]);
}

static size_t roundup_pow2(size_t v)
{
	size_t r = 1;

	while (r < v)
		r <<= 1;
	return r;
}

static int read_sysctl_int(const char *path, int dflt)
{
	FILE *f = fopen(path, "r");
	int v;

	if (!f)
		return dflt;
	if (fscanf(f, "%d", &v) != 1)
		v = dflt;
	fclose(f);
	return v;
}

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

static void irq_hook_progs(struct chaingraph_bpf *obj, int i,
			   struct bpf_program **enter, struct bpf_program **leave)
{
	*enter = i == 0 ? obj->progs.on_irq_exit_rcu_enter : obj->progs.on_irq_exit_enter;
	*leave = i == 0 ? obj->progs.on_irq_exit_rcu_leave : obj->progs.on_irq_exit_leave;
}

static struct chaingraph_bpf *open_and_load(bool with_irq_hooks, int *errp)
{
	struct chaingraph_bpf *obj;
	struct bpf_program *enter, *leave;
	int err;

	obj = chaingraph_bpf__open();
	if (!obj) {
		*errp = -errno;
		fprintf(stderr, "failed to open BPF object: %s\n", strerror(errno));
		return NULL;
	}

	obj->rodata->targ_tgid = env.tgid;
	obj->rodata->targ_pid = env.pid;
	obj->rodata->filter_comm = env.filter_comm;
	memcpy(obj->rodata->targ_comm, env.comm, TASK_COMM_LEN);
	obj->rodata->user_threads_only = env.user_threads_only;
	obj->rodata->kernel_threads_only = env.kernel_threads_only;
	obj->rodata->state_mask = env.state_mask;
	obj->rodata->min_block_ns = env.min_block_us * 1000;
	obj->rodata->max_block_ns = env.max_block_us > (__u64)-1 / 1000 ?
				    (__u64)-1 : env.max_block_us * 1000;
	obj->rodata->kernel_stacks = !env.user_stacks_only;
	obj->rodata->user_stacks = !env.kernel_stacks_only;
	obj->rodata->per_thread = env.per_thread;
	obj->rodata->max_depth = env.depth;
	obj->rodata->softirq_mode = env.softirq;

	bpf_map__set_value_size(obj->maps.kstacks,
				env.perf_max_stack_depth * sizeof(__u64));
	bpf_map__set_value_size(obj->maps.ustacks,
				env.perf_max_stack_depth * sizeof(__u64));
	bpf_map__set_max_entries(obj->maps.kstacks, env.stack_storage_size);
	bpf_map__set_max_entries(obj->maps.ustacks, env.stack_storage_size);
	bpf_map__set_max_entries(obj->maps.chains, env.max_chains);
	if (env.user_stacks_only)
		bpf_map__set_max_entries(obj->maps.kstacks, 1);
	if (env.kernel_stacks_only)
		bpf_map__set_max_entries(obj->maps.ustacks, 1);

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

	err = chaingraph_bpf__load(obj);
	if (err) {
		chaingraph_bpf__destroy(obj);
		*errp = err;
		return NULL;
	}
	return obj;
}

int main(int argc, char **argv)
{
	static const struct argp argp = {
		.options = opts,
		.parser = parse_arg,
		.doc = argp_program_doc,
	};
	struct chaingraph_bpf *obj = NULL;
	double deadline;
	sigset_t sigs;
	struct bpf_program *enter, *leave;
	struct entry *entries = NULL;
	struct btf *vmlinux_btf;
	bool any_hooks = false;
	size_t n = 0;
	int err, max_stack;

	err = argp_parse(&argp, argc, argv, 0, NULL, NULL);
	if (err)
		return err;
	if (env.user_threads_only && env.kernel_threads_only) {
		fprintf(stderr, "-u and -k are mutually exclusive\n");
		return 1;
	}
	if (env.user_stacks_only && env.kernel_stacks_only) {
		fprintf(stderr, "-U and -K are mutually exclusive\n");
		return 1;
	}
	if (env.min_block_us > env.max_block_us) {
		fprintf(stderr, "min block time exceeds max block time\n");
		return 1;
	}
	max_stack = read_sysctl_int("/proc/sys/kernel/perf_event_max_stack",
				    PERF_MAX_STACK_DEPTH);
	if (max_stack > 0 && env.perf_max_stack_depth > max_stack)
		env.perf_max_stack_depth = max_stack;

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
		return 1;
	}

	kstacks.fd = bpf_map__fd(obj->maps.kstacks);
	ustacks.fd = bpf_map__fd(obj->maps.ustacks);
	kstacks.sz = roundup_pow2(bpf_map__max_entries(obj->maps.kstacks));
	ustacks.sz = roundup_pow2(bpf_map__max_entries(obj->maps.ustacks));
	kstacks.cache = calloc(kstacks.sz, sizeof(*kstacks.cache));
	ustacks.cache = calloc(ustacks.sz, sizeof(*ustacks.cache));
	if (!kstacks.cache || !ustacks.cache) {
		fprintf(stderr, "out of memory\n");
		err = -ENOMEM;
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
					"wakeups detected from stacks instead\n",
					h->func);
			continue;
		}
		if (i == 0)
			irq_exit_hooked = true;
	}

	/* kallsyms problems should show up now, not after a long trace */
	ksyms = ksyms_load();
	if (!ksyms && !env.user_stacks_only)
		fprintf(stderr, "warning: cannot read kernel symbols from "
			"/proc/kallsyms; kernel frames will be [unknown]\n");

	/* signals are taken synchronously below; a second one kills us later */
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGINT);
	sigaddset(&sigs, SIGTERM);
	sigprocmask(SIG_BLOCK, &sigs, NULL);

	err = chaingraph_bpf__attach(obj);
	if (err) {
		fprintf(stderr, "failed to attach BPF programs: %s\n", strerror(-err));
		goto cleanup;
	}

	fprintf(stderr, "Tracing off-CPU wakeup chains (depth %d)... ", env.depth);
	if (env.duration)
		fprintf(stderr, "for %ld seconds, or ", env.duration);
	fprintf(stderr, "hit Ctrl-C to end.\n");

	if (env.duration) {
		struct timespec now, left;

		clock_gettime(CLOCK_MONOTONIC, &now);
		deadline = now.tv_sec + env.duration + now.tv_nsec / 1e9;
		for (;;) {
			double rem;

			clock_gettime(CLOCK_MONOTONIC, &now);
			rem = deadline - (now.tv_sec + now.tv_nsec / 1e9);
			if (rem <= 0)
				break;
			left.tv_sec = (time_t)rem;
			left.tv_nsec = (long)((rem - left.tv_sec) * 1e9);
			if (sigtimedwait(&sigs, NULL, &left) >= 0 || errno == EAGAIN)
				break;
		}
	} else {
		while (sigwaitinfo(&sigs, NULL) < 0 && errno == EINTR)
			;
	}
	sigprocmask(SIG_UNBLOCK, &sigs, NULL);

	/*
	 * Freeze the maps before walking them. sched_switch goes first: with
	 * sched_waking detached but sched_switch still live, sleeps ending in
	 * that window would be counted as [unknown waker].
	 */
	bpf_link__destroy(obj->links.on_switch);
	obj->links.on_switch = NULL;
	chaingraph_bpf__detach(obj);
	for (size_t i = 0; i < sizeof(irq_hooks) / sizeof(irq_hooks[0]); i++) {
		bpf_link__destroy(irq_hooks[i].enter_link);
		bpf_link__destroy(irq_hooks[i].leave_link);
		irq_hooks[i].enter_link = irq_hooks[i].leave_link = NULL;
	}

	/* pick up modules and BPF programs loaded while tracing */
	if (ksyms) {
		struct ksyms *fresh = ksyms_load();

		if (fresh) {
			ksyms_free(ksyms);
			ksyms = fresh;
		}
	}
	usyms = usyms_new();

	n = collect(bpf_map__fd(obj->maps.chains), &entries);
	for (size_t i = 0; i < n; i++) {
		if (env.folded) {
			__u64 us = entries[i].total_ns / 1000;

			if (us)
				printf("%s %llu\n", entries[i].folded,
				       (unsigned long long)us);
		} else {
			print_entry(&entries[i]);
		}
	}
	fflush(stdout);
	print_stats(obj, n);
	err = 0;

cleanup:
	for (size_t i = 0; i < n; i++)
		free(entries[i].folded);
	free(entries);
	for (struct stack_map *m = &kstacks; m; m = (m == &kstacks) ? &ustacks : NULL) {
		if (!m->cache)
			continue;
		for (size_t i = 0; i < m->sz; i++)
			free(m->cache[i].ips);
		free(m->cache);
	}
	for (size_t i = 0; i < sizeof(irq_hooks) / sizeof(irq_hooks[0]); i++) {
		bpf_link__destroy(irq_hooks[i].leave_link);
		bpf_link__destroy(irq_hooks[i].enter_link);
	}
	if (usyms)
		usyms_free(usyms);
	if (ksyms)
		ksyms_free(ksyms);
	chaingraph_bpf__destroy(obj);
	return err != 0;
}
