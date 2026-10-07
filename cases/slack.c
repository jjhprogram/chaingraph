// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/*
 * slack.so: make a program's timed sleeps imprecise, in bursts.
 *
 * Preloaded into an unmodified game (LD_PRELOAD), it wraps clock_nanosleep
 * and, for SLACK_BURST_MS in every SLACK_PERIOD_MS, raises the calling
 * thread's timer slack to SLACK_NS before the sleep. The kernel may then
 * deliver the wakeup that much late, which is what a frame limiter sees
 * when timers are being deferred under it: the Linux 7.0 clockevents
 * regression in cases.md, or a power policy that coalesces timers.
 *
 * Another process cannot set a thread's timer slack without privilege, so
 * this runs inside the game; the game's own code is not changed.
 *
 * Every sleep made with the slack raised is logged as [start, end] on
 * CLOCK_MONOTONIC to SLACK_LOG at exit: the ground truth for which frames
 * were exposed.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <time.h>

#define NSEC	1000000000ULL
#define MAX_LOG	65536

static int (*real_nanosleep)(clockid_t, int, const struct timespec *,
			     struct timespec *);
static uint64_t slack_ns = 8000000, period_ns = 1500 * 1000000ULL,
		burst_ns = 400 * 1000000ULL, t_first;
static struct { uint64_t start, end; } *log_buf;
static size_t nlog;

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * NSEC + ts.tv_nsec;
}

static uint64_t env_u64(const char *name, uint64_t def)
{
	const char *v = getenv(name);

	return v ? strtoull(v, NULL, 10) : def;
}

static void dump(void)
{
	const char *path = getenv("SLACK_LOG");
	FILE *f = path ? fopen(path, "w") : NULL;
	size_t k;

	if (!f)
		return;
	for (k = 0; k < nlog; k++)
		fprintf(f, "{\"burst\":%zu,\"start_ns\":%llu,\"end_ns\":%llu}\n", k,
			(unsigned long long)log_buf[k].start,
			(unsigned long long)log_buf[k].end);
	fclose(f);
}

__attribute__((constructor)) static void init(void)
{
	real_nanosleep = dlsym(RTLD_NEXT, "clock_nanosleep");
	slack_ns = env_u64("SLACK_NS", slack_ns);
	period_ns = env_u64("SLACK_PERIOD_MS", period_ns / 1000000) * 1000000;
	burst_ns = env_u64("SLACK_BURST_MS", burst_ns / 1000000) * 1000000;
	log_buf = calloc(MAX_LOG, sizeof(*log_buf));
	t_first = now_ns() + 3 * NSEC;		/* let the game start first */
	atexit(dump);
}

int clock_nanosleep(clockid_t clock, int flags, const struct timespec *req,
		    struct timespec *rem)
{
	uint64_t t0 = now_ns();
	int in_burst = t0 >= t_first && (t0 - t_first) % period_ns < burst_ns;
	int ret;

	if (!in_burst)
		return real_nanosleep(clock, flags, req, rem);
	prctl(PR_SET_TIMERSLACK, slack_ns);
	ret = real_nanosleep(clock, flags, req, rem);
	prctl(PR_SET_TIMERSLACK, 0);		/* back to the default */
	if (log_buf && nlog < MAX_LOG) {
		log_buf[nlog].start = t0;
		log_buf[nlog].end = now_ns();
		nlog++;
	}
	return ret;
}
