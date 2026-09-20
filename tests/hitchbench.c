// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/*
 * hitchbench: a synthetic "game" whose frames can be made to hitch in known
 * ways, with a ground-truth log saying what was injected into which frame.
 *
 *	hb_root --job--> hb_worker --read(pipe)--> hb_feeder --> [hrtimer]
 *	   |  \
 *	   |   +-- hb_hogN (pinned busy threads, woken by the preempt injector)
 *	   |
 *	   +-- hitch_frame_mark(frame_id)	<- the uprobe target
 *
 * hb_root runs a paced frame loop. Every frame it burns a fixed slice of CPU
 * (a calibrated, work-bounded busy loop, so preemption stretches it in wall
 * time), hands a small job to hb_worker and waits for it, calls the frame
 * marker exactly once, and then sleeps to pace the loop. On the frames picked
 * by the schedule it also runs one injector, each of which stalls the frame
 * through a different kernel mechanism:
 *
 *	sleep		root nanosleeps ~25 ms		-> HT_BLOCK_TIMER
 *	worker_block	worker blocks on a pipe the	-> HT_BLOCK_FUTEX,
 *			feeder writes after ~20 ms	   inherited, chain
 *			   				   root <- worker
 *			   				   <- feeder <- timer
 *	worker_cpu	worker burns ~20 ms of CPU	-> HT_BLOCK_FUTEX,
 *			(negative control)		   HT_RESOLVED_WAIT_ONCPU
 *	cpu_spike	root burns ~25 ms of CPU	-> HT_ONCPU
 *			(negative control)
 *	preempt		root and N hogs pinned to one	-> HT_RUNNABLE
 *			CPU for ~20 ms			   (and/or HT_RUNQUEUE)
 *	thousand_cuts	~40 x ~0.5 ms nanosleeps	-> over budget with no
 *							   single stall dominating
 *	io		8 MB read after fadvise		-> HT_BLOCK_IO
 *			POSIX_FADV_DONTNEED (best effort)
 *	poll		root waits in epoll_wait for	-> HT_BLOCK_POLL
 *			the byte the feeder writes
 *			after ~20 ms
 *	fault		root touches a burst of fresh	-> HT_ONCPU_FAULT
 *			anonymous pages: ~20 ms of
 *			minor faults, on-CPU
 *
 * A frame is the interval between two marks, so the pace sleep that follows
 * mark N belongs to frame N+1: a healthy frame is mostly that timer sleep.
 * The loop paces to 1/8 under the budget (14.6 ms, ~69 fps at the defaults)
 * rather than to the budget itself: with marks on an absolute grid a healthy
 * frame is exactly one period long, so pacing at the budget would put half
 * of them over it. The headroom keeps roughly the 99th percentile of healthy
 * frames under budget on an idle machine, while injected frames overrun by
 * 15 ms or more. After an overrun the pace sleep is skipped and the grid
 * restarts, which makes exactly one short frame follow each injected one.
 *
 * The ground truth is JSONL on stdout's sibling file (-o), one line per frame,
 * flushed as it goes, always with the same keys:
 *
 *   {"frame_id":42,"injected":"sleep","expect":"HT_BLOCK_TIMER",
 *    "frame_us":40123,"t_end_ns":9876543210,"root_tid":1234,
 *    "expect_resolved":null,"note":null}
 *
 * injected/expect/expect_resolved/note are null when they do not apply;
 * expect_resolved is set for the two worker classes, where what distinguishes
 * them is not the bucket but how the wait resolves through the waker. t_end_ns
 * is CLOCK_MONOTONIC (the clock behind bpf_ktime_get_ns()) read immediately
 * before the marker call, so frame_us is mark-to-mark, measured the same way
 * at both ends. The line is written just after the mark, so its write(2)
 * lands at the very start of the next frame.
 *
 * usage: hitchbench [-d SECONDS] [-b BUDGET_US] [-s SEED] [-o FILE]
 *                   [-c CLASS] [-i INTERVAL_FRAMES] [-l] [-h]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#define NSEC_PER_SEC		1000000000LL
#define NSEC_PER_MSEC		1000000LL
#define NSEC_PER_USEC		1000LL

/* the steady-state frame */
#define FRAME_CPU_NS		(4 * NSEC_PER_MSEC)	/* root's own slice */
#define WORKER_JOB_NS		(2 * NSEC_PER_MSEC)	/* the small job */

/* the injectors */
#define INJECT_SLEEP_NS		(25 * NSEC_PER_MSEC)
#define INJECT_CPU_NS		(25 * NSEC_PER_MSEC)
#define INJECT_FEED_NS		(20 * NSEC_PER_MSEC)
#define INJECT_WORKER_CPU_NS	(20 * NSEC_PER_MSEC)
#define PREEMPT_WINDOW_NS	(20 * NSEC_PER_MSEC)	/* hogs run this long */
#define PREEMPT_WORK_NS		(5 * NSEC_PER_MSEC)	/* root's work meanwhile */
#define NHOGS			3
#define CUTS_COUNT		40
#define CUTS_NS			(500 * NSEC_PER_USEC)
#define IO_BYTES		(8u << 20)
#define IO_CHUNK		(256u << 10)
#define IO_BLOCKED_MIN_NS	(2 * NSEC_PER_MSEC)	/* below this: no I/O */
#define POLL_TIMEOUT_MS		250		/* bound on the epoll wait */
#define FAULT_TARGET_NS		(20 * NSEC_PER_MSEC)	/* time spent faulting */
#define FAULT_CAL_PAGES		4096		/* the calibration region */
#define FAULT_CAL_RUNS		3
#define FAULT_MIN_PAGES		512
#define FAULT_MAX_BYTES		(256u << 20)	/* cap on the burst region */

#define DEFAULT_DURATION_S	20
#define DEFAULT_BUDGET_US	16667		/* 60 fps */
#define DEFAULT_INTERVAL	20
#define DEFAULT_SEED		1
#define DEFAULT_OUTPUT		"tests/out/hitchbench.jsonl"
#define SCRATCH_FMT		"hitchbench.%d.scratch"	/* pid */

/* ------------------------------------------------------------------ */
/* the frame marker: the uprobe target                                 */
/* ------------------------------------------------------------------ */

static volatile unsigned long hitch_frame_seen;

/*
 * void hitch_frame_mark(unsigned long frame_id)
 *
 * Deliberately dull and deliberately unoptimizable: external linkage so the
 * name is in .symtab, noinline/noclone so no caller gets a specialized copy,
 * used so it survives even if nothing calls it, and an asm barrier plus a
 * store to a volatile so GCC cannot infer it is pure and drop the calls.
 * At the uprobe (function entry) the frame id is simply the first argument
 * in the ABI's first argument register.
 */
#if defined(__GNUC__) && !defined(__clang__)
#define HB_MARKER __attribute__((noinline, noclone, used))
#else
#define HB_MARKER __attribute__((noinline, used))
#endif

HB_MARKER void hitch_frame_mark(unsigned long frame_id)
{
	__asm__ __volatile__("" : : "r"(frame_id) : "memory");
	hitch_frame_seen = frame_id;
}

/* ------------------------------------------------------------------ */
/* time                                                                */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t stop;

static long long mono_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

static void ns_to_ts(long long ns, struct timespec *ts)
{
	ts->tv_sec = (time_t)(ns / NSEC_PER_SEC);
	ts->tv_nsec = (long)(ns % NSEC_PER_SEC);
}

/* Sleep to an absolute CLOCK_MONOTONIC deadline, giving up early on stop. */
static void sleep_until(long long deadline_ns)
{
	struct timespec ts;
	int ret;

	ns_to_ts(deadline_ns, &ts);
	do {
		ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
	} while (ret == EINTR && !stop);
}

static void sleep_ns(long long ns)
{
	sleep_until(mono_now() + ns);
}

/* ------------------------------------------------------------------ */
/* the busy loop                                                       */
/* ------------------------------------------------------------------ */

/*
 * burn_iters() is work-bounded, not wall-clock-bounded: a busy loop that
 * checked the clock would shrink its work under preemption, and the preempt
 * injector needs the opposite. The asm barrier fixes the cost of one
 * iteration and keeps the loop from being unrolled away.
 */
static unsigned long burn_state = 0x243f6a8885a308d3UL;

static void burn_iters(unsigned long iters)
{
	unsigned long x = burn_state;

	while (iters--) {
		x = x * 6364136223846793005UL + 1442695040888963407UL;
		__asm__ __volatile__("" : "+r"(x));
	}
	burn_state = x;
}

static double burn_iters_per_ns;

static void burn_ns(long long ns)
{
	if (ns > 0)
		burn_iters((unsigned long)((double)ns * burn_iters_per_ns));
}

/*
 * Time a loop long enough to swamp the clock calls, then keep the fastest of
 * a few runs: the fastest one is the one that was disturbed least.
 */
static void calibrate_burn(void)
{
	unsigned long iters = 1UL << 16;
	double best = 0;
	long long dt = 0;
	int i;

	while (dt < 2 * NSEC_PER_MSEC && iters < (1UL << 32)) {
		long long t0 = mono_now();

		burn_iters(iters);
		dt = mono_now() - t0;
		if (dt < 2 * NSEC_PER_MSEC)
			iters *= 2;
	}
	for (i = 0; i < 5; i++) {
		long long t0 = mono_now();
		double rate;

		burn_iters(iters);
		dt = mono_now() - t0;
		rate = dt > 0 ? (double)iters / (double)dt : 0;
		if (rate > best)
			best = rate;
	}
	/* a plausible fallback if the clock misbehaved: ~1 iteration per ns */
	burn_iters_per_ns = best > 0 ? best : 1.0;
}

/* ------------------------------------------------------------------ */
/* injector classes                                                    */
/* ------------------------------------------------------------------ */

enum inject_class {
	INJ_SLEEP,
	INJ_WORKER_BLOCK,
	INJ_WORKER_CPU,
	INJ_CPU_SPIKE,
	INJ_PREEMPT,
	INJ_THOUSAND_CUTS,
	INJ_IO,
	INJ_POLL,
	INJ_FAULT,
	INJ_MAX,
	INJ_NONE = INJ_MAX,	/* also the index of the "not injected" stats */
};

static const struct inject_info {
	const char *name;
	const char *expect;
	const char *expect_resolved;
	const char *desc;
} classes[INJ_MAX] = {
	[INJ_SLEEP] = {
		"sleep", "HT_BLOCK_TIMER", NULL,
		"root nanosleeps ~25 ms",
	},
	[INJ_WORKER_BLOCK] = {
		"worker_block", "HT_BLOCK_FUTEX", "HT_RESOLVED_INHERITED",
		"worker blocks on a pipe the feeder writes after ~20 ms "
		"(chain root <- worker <- feeder <- timer)",
	},
	[INJ_WORKER_CPU] = {
		"worker_cpu", "HT_BLOCK_FUTEX", "HT_RESOLVED_WAIT_ONCPU",
		"negative control: worker burns ~20 ms of CPU, so the root "
		"is waiting for work, not for the kernel",
	},
	[INJ_CPU_SPIKE] = {
		"cpu_spike", "HT_ONCPU", NULL,
		"negative control: root burns ~25 ms of CPU",
	},
	[INJ_PREEMPT] = {
		"preempt", "HT_RUNNABLE", NULL,
		"root and 3 busy hogs pinned to one CPU for ~20 ms",
	},
	[INJ_THOUSAND_CUTS] = {
		"thousand_cuts", "HT_BLOCK_TIMER", NULL,
		"~40 nanosleeps of ~0.5 ms: over budget, no single stall "
		"dominating",
	},
	[INJ_IO] = {
		"io", "HT_BLOCK_IO", NULL,
		"root reads 8 MB after posix_fadvise(POSIX_FADV_DONTNEED) "
		"(best effort: notes in the log when the read did not block)",
	},
	[INJ_POLL] = {
		"poll", "HT_BLOCK_POLL", NULL,
		"root waits in epoll_wait() for the byte the feeder writes "
		"after ~20 ms (chain root <- feeder <- timer)",
	},
	[INJ_FAULT] = {
		"fault", "HT_ONCPU_FAULT", NULL,
		"root touches a burst of fresh anonymous pages: ~20 ms of "
		"minor faults on-CPU, the page count calibrated at startup",
	},
};

/* ------------------------------------------------------------------ */
/* configuration and state                                             */
/* ------------------------------------------------------------------ */

static struct env {
	long duration_s;
	long budget_us;
	long interval;
	unsigned long long seed;
	const char *output;
	enum inject_class only;
} env = {
	.duration_s = DEFAULT_DURATION_S,
	.budget_us = DEFAULT_BUDGET_US,
	.interval = DEFAULT_INTERVAL,
	.seed = DEFAULT_SEED,
	.output = DEFAULT_OUTPUT,
	.only = INJ_NONE,
};

static long long budget_ns;
static long long period_ns;
static FILE *gt_file;
static pid_t root_tid;
static char scratch_path[PATH_MAX];
static int io_fd = -1;
static char *io_buf;
static int poll_epfd = -1;
static long fault_page_size;
static size_t fault_pages;		/* 0 when the fault class is unusable */
static double fault_ns_per_page;

/* the root thread publishes its tid, then waits to be released */
static pthread_mutex_t start_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cv = PTHREAD_COND_INITIALIZER;
static bool start_go;

/* ------------------------------------------------------------------ */
/* the deterministic schedule                                          */
/* ------------------------------------------------------------------ */

static unsigned long long rng_state;

static unsigned long long rng_next(void)
{
	unsigned long long z = (rng_state += 0x9e3779b97f4a7c15ULL);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

static enum inject_class sched_order[INJ_MAX];
static int sched_pos;

/* a reshuffled cycle, so every class appears once per INJ_MAX injections */
static void shuffle_schedule(void)
{
	int i;

	for (i = 0; i < INJ_MAX; i++)
		sched_order[i] = (enum inject_class)i;
	for (i = INJ_MAX - 1; i > 0; i--) {
		int j = (int)(rng_next() % (unsigned long long)(i + 1));
		enum inject_class tmp = sched_order[i];

		sched_order[i] = sched_order[j];
		sched_order[j] = tmp;
	}
	sched_pos = 0;
}

static enum inject_class next_class(void)
{
	if (env.only != INJ_NONE)
		return env.only;
	if (sched_pos >= INJ_MAX)
		shuffle_schedule();
	return sched_order[sched_pos++];
}

/* ------------------------------------------------------------------ */
/* worker and feeder                                                   */
/* ------------------------------------------------------------------ */

enum job_kind {
	JOB_BURN_SMALL,		/* the steady-state job */
	JOB_BURN_BIG,		/* worker_cpu */
	JOB_PIPE_WAIT,		/* worker_block */
};

static struct {
	pthread_mutex_t lock;
	pthread_cond_t req_cv;
	pthread_cond_t done_cv;
	unsigned long req;
	unsigned long done;
	enum job_kind kind;
	bool quit;
} job = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.req_cv = PTHREAD_COND_INITIALIZER,
	.done_cv = PTHREAD_COND_INITIALIZER,
};

static struct {
	pthread_mutex_t lock;
	pthread_cond_t req_cv;
	unsigned long req;
	unsigned long done;
	bool quit;
} feeder = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.req_cv = PTHREAD_COND_INITIALIZER,
};

static int feed_pipe[2] = { -1, -1 };

static void ask_feeder(void)
{
	pthread_mutex_lock(&feeder.lock);
	feeder.req++;
	pthread_cond_signal(&feeder.req_cv);
	pthread_mutex_unlock(&feeder.lock);
}

/*
 * Take the feeder's byte off the pipe. The feeder writes exactly one byte per
 * request, so this always completes once ask_feeder() has been called.
 */
static void feed_read_byte(void)
{
	for (;;) {
		char byte;
		ssize_t n = read(feed_pipe[0], &byte, 1);

		if (n >= 0 || errno != EINTR)
			break;
	}
}

static void *feeder_thread(void *arg)
{
	(void)arg;
	prctl(PR_SET_NAME, "hb_feeder");

	for (;;) {
		pthread_mutex_lock(&feeder.lock);
		while (!feeder.quit && feeder.done == feeder.req)
			pthread_cond_wait(&feeder.req_cv, &feeder.lock);
		if (feeder.quit) {
			pthread_mutex_unlock(&feeder.lock);
			break;
		}
		feeder.done = feeder.req;
		pthread_mutex_unlock(&feeder.lock);

		/* the hrtimer at the end of the chain */
		sleep_ns(INJECT_FEED_NS);
		for (;;) {
			char byte = '.';
			ssize_t n = write(feed_pipe[1], &byte, 1);

			if (n == 1 || (n < 0 && errno != EINTR))
				break;
		}
	}
	return NULL;
}

static void *worker_thread(void *arg)
{
	(void)arg;
	prctl(PR_SET_NAME, "hb_worker");

	for (;;) {
		enum job_kind kind;

		pthread_mutex_lock(&job.lock);
		while (!job.quit && job.done == job.req)
			pthread_cond_wait(&job.req_cv, &job.lock);
		if (job.quit) {
			pthread_mutex_unlock(&job.lock);
			break;
		}
		kind = job.kind;
		pthread_mutex_unlock(&job.lock);

		switch (kind) {
		case JOB_BURN_SMALL:
			burn_ns(WORKER_JOB_NS);
			break;
		case JOB_BURN_BIG:
			burn_ns(INJECT_WORKER_CPU_NS);
			break;
		case JOB_PIPE_WAIT:
			ask_feeder();
			for (;;) {
				char byte;
				ssize_t n = read(feed_pipe[0], &byte, 1);

				if (n == 1 || (n < 0 && errno != EINTR))
					break;
				if (n == 0)
					break;
			}
			break;
		}

		pthread_mutex_lock(&job.lock);
		job.done = job.req;
		pthread_cond_signal(&job.done_cv);
		pthread_mutex_unlock(&job.lock);
	}
	return NULL;
}

/* Hand a job to the worker and block until it reports back. */
static void run_job(enum job_kind kind)
{
	unsigned long seq;

	pthread_mutex_lock(&job.lock);
	job.kind = kind;
	seq = ++job.req;
	pthread_cond_signal(&job.req_cv);
	while (!job.quit && job.done != seq)
		pthread_cond_wait(&job.done_cv, &job.lock);
	pthread_mutex_unlock(&job.lock);
}

/* ------------------------------------------------------------------ */
/* the CPU hogs behind the preempt injector                            */
/* ------------------------------------------------------------------ */

static struct {
	pthread_mutex_t lock;
	pthread_cond_t cv;
	unsigned long gen;
	long long deadline_ns;
	bool quit;
} hogs = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.cv = PTHREAD_COND_INITIALIZER,
};

static int pin_cpu = -1;
static cpu_set_t root_affinity;

static void pin_to(int cpu)
{
	cpu_set_t set;

	if (cpu < 0)
		return;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		fprintf(stderr, "sched_setaffinity(cpu %d): %s\n", cpu,
			strerror(errno));
}

static void *hog_thread(void *arg)
{
	unsigned long seen = 0;
	char name[16];

	snprintf(name, sizeof(name), "hb_hog%d", (int)(intptr_t)arg);
	prctl(PR_SET_NAME, name);
	pin_to(pin_cpu);

	for (;;) {
		long long deadline;

		pthread_mutex_lock(&hogs.lock);
		while (!hogs.quit && hogs.gen == seen)
			pthread_cond_wait(&hogs.cv, &hogs.lock);
		if (hogs.quit) {
			pthread_mutex_unlock(&hogs.lock);
			break;
		}
		seen = hogs.gen;
		deadline = hogs.deadline_ns;
		pthread_mutex_unlock(&hogs.lock);

		while (!stop && mono_now() < deadline)
			burn_ns(50 * NSEC_PER_USEC);
	}
	return NULL;
}

static void hogs_run(long long ns)
{
	pthread_mutex_lock(&hogs.lock);
	hogs.deadline_ns = mono_now() + ns;
	hogs.gen++;
	pthread_cond_broadcast(&hogs.cv);
	pthread_mutex_unlock(&hogs.lock);
}

/* ------------------------------------------------------------------ */
/* the I/O scratch file                                                */
/* ------------------------------------------------------------------ */

static int io_setup(const char *path)
{
	unsigned int done = 0;

	io_buf = malloc(IO_CHUNK);
	if (!io_buf) {
		fprintf(stderr, "io: out of memory\n");
		return -1;
	}
	memset(io_buf, 0xa5, IO_CHUNK);

	io_fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (io_fd < 0) {
		fprintf(stderr, "io: open %s: %s\n", path, strerror(errno));
		return -1;
	}
	while (done < IO_BYTES) {
		ssize_t n = write(io_fd, io_buf, IO_CHUNK);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "io: write %s: %s\n", path,
				strerror(errno));
			return -1;
		}
		done += (unsigned int)n;
	}
	if (fsync(io_fd)) {
		fprintf(stderr, "io: fsync %s: %s\n", path, strerror(errno));
		return -1;
	}
	return 0;
}

static void io_teardown(void)
{
	if (io_fd >= 0) {
		close(io_fd);
		io_fd = -1;
		unlink(scratch_path);
	}
	free(io_buf);
	io_buf = NULL;
}

/* ------------------------------------------------------------------ */
/* the epoll set behind the poll injector                              */
/* ------------------------------------------------------------------ */

/*
 * The root waits on the read end of the feeder's pipe. Level-triggered and
 * only ever waited on inside the injector, so registering the fd for good
 * does not disturb the worker, which reads the same pipe in worker_block.
 */
static int poll_setup(void)
{
	struct epoll_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.fd = feed_pipe[0];

	poll_epfd = epoll_create1(EPOLL_CLOEXEC);
	if (poll_epfd < 0) {
		fprintf(stderr, "poll: epoll_create1: %s\n", strerror(errno));
		return -1;
	}
	if (epoll_ctl(poll_epfd, EPOLL_CTL_ADD, feed_pipe[0], &ev)) {
		fprintf(stderr, "poll: epoll_ctl: %s\n", strerror(errno));
		close(poll_epfd);
		poll_epfd = -1;
		return -1;
	}
	return 0;
}

static void poll_teardown(void)
{
	if (poll_epfd >= 0) {
		close(poll_epfd);
		poll_epfd = -1;
	}
}

/* ------------------------------------------------------------------ */
/* the page-fault burst                                                */
/* ------------------------------------------------------------------ */

/*
 * Map @pages of fresh anonymous memory, touch one byte in each and unmap it
 * again. MAP_POPULATE is deliberately off, so every touch is a minor fault:
 * the kernel allocates and zeroes the page inside handle_mm_fault() with the
 * thread on-CPU. Returns how long the touch loop took, or -1 if the mapping
 * failed (errno is then the mmap's).
 */
static long long touch_fresh_pages(size_t pages)
{
	size_t len = pages * (size_t)fault_page_size;
	volatile char *p;
	long long t0, dt;
	size_t off;

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if ((void *)p == MAP_FAILED)
		return -1;
	t0 = mono_now();
	for (off = 0; off < len; off += (size_t)fault_page_size)
		p[off] = 1;
	dt = mono_now() - t0;
	munmap((void *)p, len);
	return dt;
}

/*
 * Size the burst for this machine: touch a small region a few times, keep the
 * fastest run (the least disturbed one, as calibrate_burn() does) and scale it
 * up to FAULT_TARGET_NS. The region is capped, so a box where a fault is slow
 * asks for a bounded amount of memory rather than an absurd one.
 */
static void calibrate_faults(void)
{
	double per_ns = 0;
	size_t max_pages;
	int i;

	fault_page_size = sysconf(_SC_PAGESIZE);
	if (fault_page_size <= 0)
		fault_page_size = 4096;

	for (i = 0; i < FAULT_CAL_RUNS; i++) {
		long long dt = touch_fresh_pages(FAULT_CAL_PAGES);
		double per;

		if (dt < 0) {
			fprintf(stderr, "fault: mmap of %d pages: %s\n",
				FAULT_CAL_PAGES, strerror(errno));
			return;
		}
		per = (double)dt / FAULT_CAL_PAGES;
		if (per > 0 && (per_ns == 0 || per < per_ns))
			per_ns = per;
	}
	/* a plausible fallback if the clock misbehaved: ~1 us per fault */
	fault_ns_per_page = per_ns > 0 ? per_ns : 1000.0;

	max_pages = FAULT_MAX_BYTES / (size_t)fault_page_size;
	fault_pages = (size_t)((double)FAULT_TARGET_NS / fault_ns_per_page);
	if (fault_pages < FAULT_MIN_PAGES)
		fault_pages = FAULT_MIN_PAGES;
	if (fault_pages > max_pages)
		fault_pages = max_pages;
}

/* ------------------------------------------------------------------ */
/* the injectors                                                       */
/* ------------------------------------------------------------------ */

/*
 * Everything a class does on the root thread. The two worker classes do their
 * work through the job handed to the worker below, so they do nothing here.
 * @note is filled in only when something worth reporting happened.
 */
static void inject(enum inject_class cls, char *note, size_t notelen)
{
	switch (cls) {
	case INJ_SLEEP:
		sleep_ns(INJECT_SLEEP_NS);
		break;
	case INJ_CPU_SPIKE:
		burn_ns(INJECT_CPU_NS);
		break;
	case INJ_THOUSAND_CUTS: {
		int i;

		for (i = 0; i < CUTS_COUNT && !stop; i++)
			sleep_ns(CUTS_NS);
		break;
	}
	case INJ_PREEMPT:
		/*
		 * Root and NHOGS hogs on one CPU: the root's work is
		 * work-bounded, so the ~4x contention stretches PREEMPT_WORK_NS
		 * of CPU over about the hogs' window in wall time.
		 */
		pin_to(pin_cpu);
		hogs_run(PREEMPT_WINDOW_NS);
		burn_ns(PREEMPT_WORK_NS);
		if (sched_setaffinity(0, sizeof(root_affinity), &root_affinity))
			fprintf(stderr, "sched_setaffinity(restore): %s\n",
				strerror(errno));
		break;
	case INJ_IO: {
		long long t0, dt;
		unsigned int done = 0;

		if (io_fd < 0) {
			snprintf(note, notelen, "io scratch file unavailable");
			break;
		}
		posix_fadvise(io_fd, 0, 0, POSIX_FADV_DONTNEED);
		lseek(io_fd, 0, SEEK_SET);
		t0 = mono_now();
		while (done < IO_BYTES) {
			ssize_t n = read(io_fd, io_buf, IO_CHUNK);

			if (n < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			if (n == 0)
				break;
			done += (unsigned int)n;
		}
		dt = mono_now() - t0;
		if (dt < IO_BLOCKED_MIN_NS)
			snprintf(note, notelen,
				 "io read did not block: %u bytes in %lld us",
				 done, dt / NSEC_PER_USEC);
		break;
	}
	case INJ_POLL: {
		struct epoll_event ev;
		long long t0, dt;
		int n, err;

		if (poll_epfd < 0) {
			snprintf(note, notelen, "poll: no epoll instance");
			break;
		}
		ask_feeder();
		t0 = mono_now();
		do {
			n = epoll_wait(poll_epfd, &ev, 1, POLL_TIMEOUT_MS);
		} while (n < 0 && errno == EINTR && !stop);
		err = n < 0 ? errno : 0;
		dt = mono_now() - t0;
		/*
		 * Take the byte whatever happened: the feeder writes one per
		 * request, and leaving it on the pipe would short-circuit the
		 * next frame that waits for it.
		 */
		feed_read_byte();
		if (n != 1)
			snprintf(note, notelen,
				 "poll: epoll_wait returned %d (%s) after %lld us",
				 n, err ? strerror(err) : "timeout",
				 dt / NSEC_PER_USEC);
		break;
	}
	case INJ_FAULT: {
		long long dt;

		if (!fault_pages) {
			snprintf(note, notelen, "fault: region unavailable");
			break;
		}
		dt = touch_fresh_pages(fault_pages);
		if (dt < 0)
			snprintf(note, notelen, "fault: mmap of %zu pages: %s",
				 fault_pages, strerror(errno));
		else
			snprintf(note, notelen, "%zu pages touched in %lld us",
				 fault_pages, dt / NSEC_PER_USEC);
		break;
	}
	case INJ_WORKER_BLOCK:
	case INJ_WORKER_CPU:
	case INJ_MAX:
		break;
	}
}

static enum job_kind job_for(enum inject_class cls)
{
	switch (cls) {
	case INJ_WORKER_BLOCK:
		return JOB_PIPE_WAIT;
	case INJ_WORKER_CPU:
		return JOB_BURN_BIG;
	default:
		return JOB_BURN_SMALL;
	}
}

/* ------------------------------------------------------------------ */
/* ground truth                                                        */
/* ------------------------------------------------------------------ */

static const char *jstr(char *buf, size_t len, const char *s)
{
	if (!s || !*s)
		return "null";
	snprintf(buf, len, "\"%s\"", s);
	return buf;
}

static struct class_stats {
	unsigned long n;
	unsigned long over;
	long long min_us;
	long long max_us;
	long long sum_us;
} stats[INJ_MAX + 1];

static void log_frame(unsigned long frame_id, enum inject_class cls,
		      long long frame_ns, long long t_end_ns, const char *note)
{
	const struct inject_info *ci = cls == INJ_NONE ? NULL : &classes[cls];
	long long frame_us = frame_ns / NSEC_PER_USEC;
	struct class_stats *st = &stats[cls];
	char b1[32], b2[48], b3[48], b4[256];

	fprintf(gt_file,
		"{\"frame_id\":%lu,\"injected\":%s,\"expect\":%s,"
		"\"frame_us\":%lld,\"t_end_ns\":%lld,\"root_tid\":%d,"
		"\"expect_resolved\":%s,\"note\":%s}\n",
		frame_id,
		jstr(b1, sizeof(b1), ci ? ci->name : NULL),
		jstr(b2, sizeof(b2), ci ? ci->expect : NULL),
		frame_us, t_end_ns, (int)root_tid,
		jstr(b3, sizeof(b3), ci ? ci->expect_resolved : NULL),
		jstr(b4, sizeof(b4), note));
	fflush(gt_file);

	if (!st->n || frame_us < st->min_us)
		st->min_us = frame_us;
	if (!st->n || frame_us > st->max_us)
		st->max_us = frame_us;
	st->sum_us += frame_us;
	st->n++;
	if (frame_us > env.budget_us)
		st->over++;
}

static void print_summary(void)
{
	unsigned long frames = 0, over = 0;
	int i;

	for (i = 0; i <= INJ_MAX; i++) {
		frames += stats[i].n;
		over += stats[i].over;
	}
	printf("\nsummary: frames=%lu over_budget=%lu budget_us=%ld\n",
	       frames, over, env.budget_us);
	printf("%-14s %6s %9s %9s %9s %6s\n",
	       "CLASS", "N", "MIN_US", "MEAN_US", "MAX_US", "OVER");
	for (i = 0; i <= INJ_MAX; i++) {
		const struct class_stats *st = &stats[i];

		if (!st->n)
			continue;
		printf("%-14s %6lu %9lld %9lld %9lld %6lu\n",
		       i == INJ_MAX ? "(none)" : classes[i].name,
		       st->n, st->min_us, st->sum_us / (long long)st->n,
		       st->max_us, st->over);
	}
	fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* the frame loop                                                      */
/* ------------------------------------------------------------------ */

static void *root_thread(void *arg)
{
	long long end_ns, next_pace_ns, prev_mark_ns;
	unsigned long frame_id = 0;

	(void)arg;
	prctl(PR_SET_NAME, "hb_root");
	sched_getaffinity(0, sizeof(root_affinity), &root_affinity);

	pthread_mutex_lock(&start_lock);
	root_tid = (pid_t)gettid();
	pthread_cond_broadcast(&start_cv);
	while (!start_go)
		pthread_cond_wait(&start_cv, &start_lock);
	pthread_mutex_unlock(&start_lock);

	end_ns = mono_now() + (long long)env.duration_s * NSEC_PER_SEC;
	next_pace_ns = mono_now();

	/* frame 0 only opens the first frame; it is never logged */
	prev_mark_ns = mono_now();
	hitch_frame_mark(frame_id);

	while (!stop && mono_now() < end_ns) {
		enum inject_class cls = INJ_NONE;
		char note[192] = "";
		long long t_end_ns, now;

		frame_id++;
		if (env.interval > 0 && frame_id % (unsigned long)env.interval == 0)
			cls = next_class();

		burn_ns(FRAME_CPU_NS);
		inject(cls, note, sizeof(note));
		run_job(job_for(cls));

		t_end_ns = mono_now();
		hitch_frame_mark(frame_id);

		log_frame(frame_id, cls, t_end_ns - prev_mark_ns, t_end_ns,
			  note);
		prev_mark_ns = t_end_ns;

		now = mono_now();
		next_pace_ns += period_ns;
		if (next_pace_ns <= now)
			next_pace_ns = now;	/* overran: no sleep, regrid */
		else
			sleep_until(next_pace_ns);
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* setup and teardown                                                  */
/* ------------------------------------------------------------------ */

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static int mkdir_p(const char *path)
{
	char buf[PATH_MAX];
	size_t i, len;

	len = strlen(path);
	if (len == 0 || len >= sizeof(buf))
		return -1;
	memcpy(buf, path, len + 1);
	for (i = 1; i <= len; i++) {
		if (buf[i] != '/' && buf[i])
			continue;
		buf[i] = '\0';
		if (mkdir(buf, 0755) && errno != EEXIST)
			return -1;
		buf[i] = i < len ? '/' : '\0';
	}
	return 0;
}

/* The scratch file lives next to the ground truth, one per process. */
static void scratch_path_for(const char *output)
{
	const char *slash = strrchr(output, '/');
	char name[64];

	snprintf(name, sizeof(name), SCRATCH_FMT, (int)getpid());
	if (slash)
		snprintf(scratch_path, sizeof(scratch_path), "%.*s/%s",
			 (int)(slash - output), output, name);
	else
		snprintf(scratch_path, sizeof(scratch_path), "%s", name);
}

static int output_dir(const char *output, char *dir, size_t len)
{
	const char *slash = strrchr(output, '/');

	if (!slash)
		return -1;
	if ((size_t)(slash - output) >= len)
		return -1;
	snprintf(dir, len, "%.*s", (int)(slash - output), output);
	return 0;
}

/* The highest CPU we are allowed on: less likely to be the interrupt CPU. */
static int pick_pin_cpu(void)
{
	cpu_set_t set;
	int i, cpu = -1;

	if (sched_getaffinity(0, sizeof(set), &set))
		return -1;
	for (i = 0; i < CPU_SETSIZE; i++)
		if (CPU_ISSET(i, &set))
			cpu = i;
	return cpu;
}

static void usage(FILE *f, const char *prog)
{
	fprintf(f,
"usage: %s [-d SECONDS] [-b BUDGET_US] [-s SEED] [-o GROUND_TRUTH.jsonl]\n"
"       [-c CLASS] [-i INTERVAL_FRAMES] [-l] [-h]\n"
"\n"
"A synthetic frame loop that hitches on purpose, for testing hitchtrace.\n"
"Attach a uprobe to hitch_frame_mark(); its only argument is the frame id.\n"
"\n"
"  -d SECONDS   run this long (default %d)\n"
"  -b BUDGET_US frame budget; the loop paces to 1/8 under it (default %d)\n"
"  -s SEED      seed of the injection schedule (default %d)\n"
"  -o FILE      ground-truth JSONL, one line per frame (default %s)\n"
"  -c CLASS     inject only this class (default: all, in shuffled cycles)\n"
"  -i N         inject into every Nth frame, 0 for never (default %d)\n"
"  -l           list the injector classes and exit\n"
"  -h           this help\n",
		prog, DEFAULT_DURATION_S, DEFAULT_BUDGET_US, DEFAULT_SEED,
		DEFAULT_OUTPUT, DEFAULT_INTERVAL);
}

static void list_classes(void)
{
	int i;

	printf("%-14s %-15s %-22s %s\n",
	       "CLASS", "EXPECT", "EXPECT_RESOLVED", "WHAT IT DOES");
	for (i = 0; i < INJ_MAX; i++)
		printf("%-14s %-15s %-22s %s\n", classes[i].name,
		       classes[i].expect,
		       classes[i].expect_resolved ? classes[i].expect_resolved : "-",
		       classes[i].desc);
}

static int parse_long(const char *s, long min, long max, long *out)
{
	char *end;
	long v;

	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || end == s || *end || v < min || v > max)
		return -1;
	*out = v;
	return 0;
}

static enum inject_class parse_class(const char *s)
{
	int i;

	for (i = 0; i < INJ_MAX; i++)
		if (!strcmp(s, classes[i].name))
			return (enum inject_class)i;
	return INJ_NONE;
}

int main(int argc, char **argv)
{
	pthread_t root, worker, feeder_tid, hog[NHOGS];
	bool have_worker = false, have_feeder = false;
	char exe[PATH_MAX], dir[PATH_MAX];
	struct sigaction sa;
	int nhogs = 0, opt, i, ret = 1;
	long v;
	ssize_t n;

	while ((opt = getopt(argc, argv, "d:b:s:o:c:i:lh")) != -1) {
		switch (opt) {
		case 'd':
			if (parse_long(optarg, 0, 24 * 3600, &env.duration_s)) {
				fprintf(stderr, "invalid duration: %s\n", optarg);
				return 2;
			}
			break;
		case 'b':
			if (parse_long(optarg, 100, 10 * 1000 * 1000,
				       &env.budget_us)) {
				fprintf(stderr, "invalid budget: %s\n", optarg);
				return 2;
			}
			break;
		case 's':
			if (parse_long(optarg, 0, LONG_MAX, &v)) {
				fprintf(stderr, "invalid seed: %s\n", optarg);
				return 2;
			}
			env.seed = (unsigned long long)v;
			break;
		case 'o':
			env.output = optarg;
			break;
		case 'c':
			env.only = parse_class(optarg);
			if (env.only == INJ_NONE) {
				fprintf(stderr, "unknown class: %s\n", optarg);
				list_classes();
				return 2;
			}
			break;
		case 'i':
			if (parse_long(optarg, 0, 1000000, &env.interval)) {
				fprintf(stderr, "invalid interval: %s\n", optarg);
				return 2;
			}
			break;
		case 'l':
			list_classes();
			return 0;
		case 'h':
			usage(stdout, argv[0]);
			return 0;
		default:
			usage(stderr, argv[0]);
			return 2;
		}
	}
	if (optind != argc) {
		usage(stderr, argv[0]);
		return 2;
	}

	budget_ns = (long long)env.budget_us * NSEC_PER_USEC;
	period_ns = budget_ns - budget_ns / 8;	/* headroom under the budget */
	rng_state = env.seed;
	shuffle_schedule();

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	if (!output_dir(env.output, dir, sizeof(dir)) && mkdir_p(dir))
		fprintf(stderr, "mkdir %s: %s\n", dir, strerror(errno));
	gt_file = fopen(env.output, "w");
	if (!gt_file) {
		fprintf(stderr, "open %s: %s\n", env.output, strerror(errno));
		return 1;
	}

	if (pipe(feed_pipe)) {
		perror("pipe");
		goto out;
	}

	scratch_path_for(env.output);
	if (env.interval > 0 && (env.only == INJ_NONE || env.only == INJ_IO)) {
		if (io_setup(scratch_path)) {
			fprintf(stderr, "io class degraded: no scratch file\n");
			io_teardown();
		}
	}
	if (env.interval > 0 && (env.only == INJ_NONE || env.only == INJ_POLL)) {
		if (poll_setup())
			fprintf(stderr, "poll class degraded: no epoll set\n");
	}
	if (env.interval > 0 && (env.only == INJ_NONE || env.only == INJ_FAULT)) {
		calibrate_faults();
		if (!fault_pages)
			fprintf(stderr, "fault class degraded: no region\n");
	}

	pin_cpu = pick_pin_cpu();
	calibrate_burn();

	if (pthread_create(&worker, NULL, worker_thread, NULL)) {
		perror("pthread_create worker");
		goto out;
	}
	have_worker = true;
	if (pthread_create(&feeder_tid, NULL, feeder_thread, NULL)) {
		perror("pthread_create feeder");
		goto out;
	}
	have_feeder = true;
	for (i = 0; i < NHOGS; i++) {
		if (pthread_create(&hog[i], NULL, hog_thread,
				   (void *)(intptr_t)i)) {
			perror("pthread_create hog");
			goto out;
		}
		nhogs++;
	}
	if (pthread_create(&root, NULL, root_thread, NULL)) {
		perror("pthread_create root");
		goto out;
	}

	pthread_mutex_lock(&start_lock);
	while (!root_tid)
		pthread_cond_wait(&start_cv, &start_lock);
	pthread_mutex_unlock(&start_lock);

	n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	exe[n > 0 ? n : 0] = '\0';

	printf("hitchbench pid=%d root_tid=%d binary=%s marker=hitch_frame_mark budget_us=%ld\n",
	       (int)getpid(), (int)root_tid, exe, env.budget_us);
	printf("period_us=%lld (%.1f fps) duration_s=%ld seed=%llu interval=%ld output=%s\n",
	       period_ns / NSEC_PER_USEC,
	       (double)NSEC_PER_SEC / (double)period_ns, env.duration_s,
	       env.seed, env.interval, env.output);
	printf("busy loop %.1f Miter/s, pin_cpu=%d hogs=%d scratch=%s\n",
	       burn_iters_per_ns * 1000.0, pin_cpu, NHOGS,
	       io_fd >= 0 ? scratch_path : "(none)");
	if (fault_pages)
		printf("fault burst %zu pages (%.0f MB) at %.0f ns/page\n",
		       fault_pages,
		       (double)fault_pages * (double)fault_page_size / (1 << 20),
		       fault_ns_per_page);
	if (env.interval <= 0) {
		printf("schedule: no injection (-i 0)\n");
	} else if (env.only != INJ_NONE) {
		printf("schedule: %s only, every %ld frames\n",
		       classes[env.only].name, env.interval);
	} else {
		printf("schedule: every %ld frames, cycle:", env.interval);
		for (i = 0; i < INJ_MAX; i++)
			printf(" %s", classes[sched_order[i]].name);
		printf("\n");
	}
	fflush(stdout);

	pthread_mutex_lock(&start_lock);
	start_go = true;
	pthread_cond_broadcast(&start_cv);
	pthread_mutex_unlock(&start_lock);

	pthread_join(root, NULL);
	ret = 0;
out:
	stop = 1;
	pthread_mutex_lock(&hogs.lock);
	hogs.quit = true;
	pthread_cond_broadcast(&hogs.cv);
	pthread_mutex_unlock(&hogs.lock);
	pthread_mutex_lock(&job.lock);
	job.quit = true;
	pthread_cond_broadcast(&job.req_cv);
	pthread_cond_broadcast(&job.done_cv);
	pthread_mutex_unlock(&job.lock);
	pthread_mutex_lock(&feeder.lock);
	feeder.quit = true;
	pthread_cond_broadcast(&feeder.req_cv);
	pthread_mutex_unlock(&feeder.lock);

	for (i = 0; i < nhogs; i++)
		pthread_join(hog[i], NULL);
	if (have_worker)
		pthread_join(worker, NULL);
	if (have_feeder)
		pthread_join(feeder_tid, NULL);

	poll_teardown();
	if (feed_pipe[0] >= 0)
		close(feed_pipe[0]);
	if (feed_pipe[1] >= 0)
		close(feed_pipe[1]);
	io_teardown();
	if (gt_file) {
		fflush(gt_file);
		fclose(gt_file);
	}
	if (!ret)
		print_summary();
	return ret;
}
