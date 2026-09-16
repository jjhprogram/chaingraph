// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/*
 * chainload: a deterministic wakeup-chain workload for testing chaingraph.
 *
 *   hrtimer --> cg_source --pipe1--> cg_relay1 --pipe2--> cg_relay2
 *                                                            |
 *                                   cg_sink <----pipe3-------+
 *
 * cg_source sleeps on an absolute CLOCK_MONOTONIC timeline (its wakeups come
 * from hrtimer_wakeup in hardirq context) and writes one byte per tick. Each
 * relay blocks reading its input pipe and forwards what it read, so every
 * sleep of cg_sink is ended by the chain
 *
 *   cg_relay2 <- cg_relay1 <- cg_source <- [hardirq]
 *
 * The blocking system calls are issued from noinline functions named
 * cg_stage_source, cg_stage_relay and cg_stage_sink, through a tiny syscall
 * trampoline that keeps a frame pointer. (glibc's single-threaded fast path
 * for read/write enters the kernel without setting up a frame, which would
 * make a frame-pointer unwinder skip the calling cg_stage_* function.)
 *
 * After the duration the source closes pipe1; EOF cascades through the
 * relays to the sink, everyone exits and the parent reaps them.
 *
 * usage: chainload [-d DURATION_SEC] [-i INTERVAL_MS]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#if defined(__GNUC__) && !defined(__clang__)
#define CG_STAGE __attribute__((noinline, noclone, used))
#else
#define CG_STAGE __attribute__((noinline, used))
#endif

#define NSEC_PER_SEC	1000000000LL
#define NSEC_PER_MSEC	1000000LL

/*
 * long cg_raw_syscall(long nr, long a1, long a2, long a3, long a4)
 *
 * Returns the raw kernel result (-errno on failure). The trampoline pushes a
 * proper frame record, so the user stack captured while blocked in the
 * kernel reads: cg_raw_syscall, cg_stage_*, <its caller>, ...
 */
#if defined(__x86_64__)
long cg_raw_syscall(long nr, long a1, long a2, long a3, long a4)
	__attribute__((visibility("hidden")));
__asm__(
	"	.text\n"
	"	.globl	cg_raw_syscall\n"
	"	.hidden	cg_raw_syscall\n"
	"	.type	cg_raw_syscall, @function\n"
	"	.p2align 4\n"
	"cg_raw_syscall:\n"
	"	.cfi_startproc\n"
	"	endbr64\n"
	"	pushq	%rbp\n"
	"	.cfi_def_cfa_offset 16\n"
	"	.cfi_offset %rbp, -16\n"
	"	movq	%rsp, %rbp\n"
	"	.cfi_def_cfa_register %rbp\n"
	"	movq	%rdi, %rax\n"		/* nr */
	"	movq	%rsi, %rdi\n"		/* a1 */
	"	movq	%rdx, %rsi\n"		/* a2 */
	"	movq	%rcx, %rdx\n"		/* a3 */
	"	movq	%r8, %r10\n"		/* a4 */
	"	syscall\n"
	"	popq	%rbp\n"
	"	.cfi_def_cfa %rsp, 8\n"
	"	ret\n"
	"	.cfi_endproc\n"
	"	.size	cg_raw_syscall, .-cg_raw_syscall\n"
);
#elif defined(__aarch64__)
long cg_raw_syscall(long nr, long a1, long a2, long a3, long a4)
	__attribute__((visibility("hidden")));
__asm__(
	"	.text\n"
	"	.globl	cg_raw_syscall\n"
	"	.hidden	cg_raw_syscall\n"
	"	.type	cg_raw_syscall, %function\n"
	"	.p2align 2\n"
	"cg_raw_syscall:\n"
	"	hint	#34\n"			/* bti c */
	"	stp	x29, x30, [sp, #-16]!\n"
	"	mov	x29, sp\n"
	"	mov	x8, x0\n"
	"	mov	x0, x1\n"
	"	mov	x1, x2\n"
	"	mov	x2, x3\n"
	"	mov	x3, x4\n"
	"	svc	#0\n"
	"	ldp	x29, x30, [sp], #16\n"
	"	ret\n"
	"	.size	cg_raw_syscall, .-cg_raw_syscall\n"
);
#else
/* Portable fallback: stacks may lose the cg_stage_* frame here. */
static long __attribute__((noinline)) cg_raw_syscall(long nr, long a1, long a2,
						      long a3, long a4)
{
	long ret = syscall(nr, a1, a2, a3, a4);

	return ret < 0 ? -errno : ret;
}
#endif

enum stage_id { SOURCE, RELAY1, RELAY2, SINK, NSTAGES };

static const char *const stage_names[NSTAGES] = {
	[SOURCE] = "cg_source",
	[RELAY1] = "cg_relay1",
	[RELAY2] = "cg_relay2",
	[SINK]   = "cg_sink",
};

/* pipes[i] connects stage i (writes pipes[i][1]) to stage i+1 (reads [0]) */
static int pipes[NSTAGES - 1][2];
static pid_t children[NSTAGES];
static volatile sig_atomic_t got_signal;

static long cg_read(int fd, char *buf, size_t len)
{
	long ret;

	do
		ret = cg_raw_syscall(SYS_read, fd, (long)buf, (long)len, 0);
	while (ret == -EINTR);
	return ret;
}

static long cg_write_all(int fd, const char *buf, size_t len)
{
	size_t done = 0;
	long ret;

	while (done < len) {
		ret = cg_raw_syscall(SYS_write, fd, (long)(buf + done),
				     (long)(len - done), 0);
		if (ret == -EINTR)
			continue;
		if (ret <= 0)
			return ret < 0 ? ret : -EIO;
		done += (size_t)ret;
	}
	return (long)done;
}

static long long ts_ns(const struct timespec *ts)
{
	return ts->tv_sec * NSEC_PER_SEC + ts->tv_nsec;
}

static void ns_ts(long long ns, struct timespec *ts)
{
	ts->tv_sec = ns / NSEC_PER_SEC;
	ts->tv_nsec = ns % NSEC_PER_SEC;
}

static long long mono_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts_ns(&ts);
}

/* Tick every interval until the end time, writing one byte per tick. */
CG_STAGE static int cg_stage_source(int out, long long interval_ns,
				    long long end_ns)
{
	struct timespec deadline;
	long long next = mono_now();
	const char tick = '.';
	long ret;

	for (;;) {
		next += interval_ns;
		/* fell behind (e.g. stopped): restart the timeline from now */
		if (next < mono_now())
			next = mono_now() + interval_ns;
		if (next > end_ns)
			return 0;
		ns_ts(next, &deadline);
		do
			ret = cg_raw_syscall(SYS_clock_nanosleep,
					     CLOCK_MONOTONIC, TIMER_ABSTIME,
					     (long)&deadline, 0);
		while (ret == -EINTR);
		if (ret) {
			fprintf(stderr, "cg_source: clock_nanosleep: %s\n",
				strerror((int)-ret));
			return 1;
		}
		ret = cg_write_all(out, &tick, 1);
		if (ret < 0) {
			fprintf(stderr, "cg_source: write: %s\n",
				strerror((int)-ret));
			return 1;
		}
	}
}

/* Forward everything read from @in to @out until EOF. */
CG_STAGE static int cg_stage_relay(const char *name, int in, int out)
{
	char buf[64];
	long n, ret;

	for (;;) {
		n = cg_read(in, buf, sizeof(buf));
		if (n == 0)
			return 0;
		if (n < 0) {
			fprintf(stderr, "%s: read: %s\n", name,
				strerror((int)-n));
			return 1;
		}
		ret = cg_write_all(out, buf, (size_t)n);
		if (ret < 0) {
			fprintf(stderr, "%s: write: %s\n", name,
				strerror((int)-ret));
			return 1;
		}
	}
}

/* Drain @in until EOF. */
CG_STAGE static int cg_stage_sink(int in)
{
	char buf[64];
	long n;

	for (;;) {
		n = cg_read(in, buf, sizeof(buf));
		if (n == 0)
			return 0;
		if (n < 0) {
			fprintf(stderr, "cg_sink: read: %s\n",
				strerror((int)-n));
			return 1;
		}
	}
}

static void close_pipes_except(int keep_a, int keep_b)
{
	for (int i = 0; i < NSTAGES - 1; i++) {
		for (int j = 0; j < 2; j++) {
			int fd = pipes[i][j];

			if (fd >= 0 && fd != keep_a && fd != keep_b) {
				close(fd);
				pipes[i][j] = -1;
			}
		}
	}
}

static int run_stage(enum stage_id id, pid_t parent, long long interval_ns,
		     long long end_ns)
{
	int in = id > SOURCE ? pipes[id - 1][0] : -1;
	int out = id < SINK ? pipes[id][1] : -1;

	signal(SIGINT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	/* never outlive the parent, even if it is SIGKILLed */
	prctl(PR_SET_PDEATHSIG, SIGKILL);
	if (getppid() != parent)
		return 1;
	prctl(PR_SET_NAME, stage_names[id]);
	close_pipes_except(in, out);

	switch (id) {
	case SOURCE:
		return cg_stage_source(out, interval_ns, end_ns);
	case RELAY1:
	case RELAY2:
		return cg_stage_relay(stage_names[id], in, out);
	case SINK:
		return cg_stage_sink(in);
	default:
		return 1;
	}
}

static void on_signal(int sig)
{
	got_signal = sig;
	for (int i = 0; i < NSTAGES; i++)
		if (children[i] > 0)
			kill(children[i], SIGTERM);
}

static void kill_children(void)
{
	for (int i = 0; i < NSTAGES; i++)
		if (children[i] > 0)
			kill(children[i], SIGTERM);
}

static void usage(FILE *f, const char *prog)
{
	fprintf(f,
		"usage: %s [-d DURATION_SEC] [-i INTERVAL_MS]\n"
		"\n"
		"Runs the wakeup chain cg_source -> cg_relay1 -> cg_relay2 -> cg_sink\n"
		"(connected by pipes) for DURATION_SEC seconds (default 10). cg_source\n"
		"sends one byte every INTERVAL_MS milliseconds (default 10).\n"
		"Prints NAME=PID for each stage on stdout.\n", prog);
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

int main(int argc, char **argv)
{
	long duration_s = 10, interval_ms = 10;
	long long interval_ns, end_ns;
	struct sigaction sa;
	pid_t parent = getpid();
	int opt, status, failed = 0, remaining = 0;

	while ((opt = getopt(argc, argv, "d:i:h")) != -1) {
		switch (opt) {
		case 'd':
			if (parse_long(optarg, 0, 24 * 3600, &duration_s)) {
				fprintf(stderr, "invalid duration: %s\n", optarg);
				return 2;
			}
			break;
		case 'i':
			if (parse_long(optarg, 1, 60 * 1000, &interval_ms)) {
				fprintf(stderr, "invalid interval: %s\n", optarg);
				return 2;
			}
			break;
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

	interval_ns = interval_ms * NSEC_PER_MSEC;
	end_ns = mono_now() + duration_s * NSEC_PER_SEC;

	for (int i = 0; i < NSTAGES - 1; i++)
		pipes[i][0] = pipes[i][1] = -1;
	for (int i = 0; i < NSTAGES - 1; i++) {
		if (pipe(pipes[i])) {
			perror("pipe");
			return 1;
		}
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	fflush(NULL);
	for (int i = 0; i < NSTAGES && !got_signal; i++) {
		pid_t pid = fork();

		if (pid < 0) {
			perror("fork");
			close_pipes_except(-1, -1);
			kill_children();
			failed = 1;
			break;
		}
		if (pid == 0)
			_exit(run_stage(i, parent, interval_ns, end_ns));
		children[i] = pid;
		remaining++;
	}

	/* the parent holds no pipe ends, so EOF can cascade */
	close_pipes_except(-1, -1);

	/* a signal that raced with fork() may have missed a child */
	if (got_signal)
		kill_children();

	if (!failed && !got_signal) {
		for (int i = 0; i < NSTAGES; i++)
			printf("%s=%d\n", stage_names[i], (int)children[i]);
		fflush(stdout);
	}

	while (remaining > 0) {
		pid_t pid = waitpid(-1, &status, 0);

		if (pid < 0) {
			if (errno == EINTR)
				continue;
			perror("waitpid");
			failed = 1;
			break;
		}
		for (int i = 0; i < NSTAGES; i++) {
			if (children[i] != pid)
				continue;
			children[i] = 0;
			remaining--;
			if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
				break;
			failed = 1;
			if (got_signal)
				break;
			if (WIFSIGNALED(status))
				fprintf(stderr, "%s (pid %d) killed by signal %d\n",
					stage_names[i], (int)pid,
					WTERMSIG(status));
			else
				fprintf(stderr, "%s (pid %d) exited with status %d\n",
					stage_names[i], (int)pid,
					WEXITSTATUS(status));
			/* a broken link stalls the chain: stop the rest */
			kill_children();
			break;
		}
	}

	if (got_signal) {
		/* report the signal to our own parent the conventional way */
		signal(got_signal, SIG_DFL);
		raise(got_signal);
		return 128 + got_signal;
	}
	return failed ? 1 : 0;
}
