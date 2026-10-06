// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/*
 * bgload: a background application for hitchtrace case studies.
 *
 * Stands in for the launcher, web helper, indexer or monitoring daemon that
 * shares a gaming machine's cores: mostly asleep, and every PERIOD it wakes
 * THREADS threads that each burn BURST of CPU. It runs at normal priority,
 * as such processes do.
 *
 * Every burst is logged as [start, end] on CLOCK_MONOTONIC (the clock behind
 * bpf_ktime_get_ns()), so a case study can tell afterwards which frames a
 * burst overlapped without trusting the tool under test.
 *
 * usage: bgload [-t THREADS] [-p PERIOD_MS] [-b BURST_MS] [-d SECONDS]
 *               [-o LOG]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NSEC	1000000000ULL
#define MAX_THREADS 64

static int opt_threads = 4, opt_period_ms = 700, opt_burst_ms = 20;
static int opt_duration = 20;
static const char *opt_log;
static uint64_t t_start, t_stop;
static volatile sig_atomic_t stop;

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * NSEC + ts.tv_nsec;
}

static void sleep_until(uint64_t t)
{
	struct timespec ts = { .tv_sec = t / NSEC, .tv_nsec = t % NSEC };

	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
		if (stop)
			return;
}

static void on_signal(int sig)
{
	stop = 1;
}

struct burst {
	uint64_t start, end;
};

static struct burst *bursts;
static size_t nbursts, capbursts;

/* All threads share one grid, so they burst together. */
static void *burner(void *arg)
{
	long id = (long)arg;
	uint64_t period = (uint64_t)opt_period_ms * 1000000;
	uint64_t burst = (uint64_t)opt_burst_ms * 1000000;
	uint64_t next = t_start + period;
	volatile uint64_t sink = 0;
	char name[16];

	snprintf(name, sizeof(name), "bgload%ld", id);
	pthread_setname_np(pthread_self(), name);

	while (!stop && next < t_stop) {
		uint64_t t0, t;

		sleep_until(next);
		if (stop)
			break;
		t0 = now_ns();
		/* burn BURST of CPU time, however long that takes on the wall */
		{
			struct timespec c0, c;

			clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c0);
			do {
				int k;

				for (k = 0; k < 20000; k++)
					sink += k * 2654435761u;
				clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c);
			} while (!stop &&
				 (uint64_t)(c.tv_sec - c0.tv_sec) * NSEC +
				 c.tv_nsec - c0.tv_nsec < burst);
		}
		t = now_ns();
		if (id == 0 && nbursts < capbursts) {
			bursts[nbursts].start = t0;
			bursts[nbursts].end = t;
			nbursts++;
		}
		next += period;
		while (next < t)
			next += period;
	}
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t th[MAX_THREADS];
	struct sigaction sa = { .sa_handler = on_signal };
	long i;
	int c;

	while ((c = getopt(argc, argv, "t:p:b:d:o:h")) != -1) {
		switch (c) {
		case 't': opt_threads = atoi(optarg); break;
		case 'p': opt_period_ms = atoi(optarg); break;
		case 'b': opt_burst_ms = atoi(optarg); break;
		case 'd': opt_duration = atoi(optarg); break;
		case 'o': opt_log = optarg; break;
		default:
			fprintf(stderr, "usage: bgload [-t THREADS] [-p PERIOD_MS] "
				"[-b BURST_MS] [-d SECONDS] [-o LOG]\n");
			return 2;
		}
	}
	if (opt_threads < 1 || opt_threads > MAX_THREADS || opt_period_ms < 1 ||
	    opt_burst_ms < 1 || opt_duration < 1)
		return 2;

	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	capbursts = (size_t)opt_duration * 1000 / opt_period_ms + 16;
	bursts = calloc(capbursts, sizeof(*bursts));
	if (!bursts)
		return 1;
	t_start = now_ns();
	t_stop = t_start + (uint64_t)opt_duration * NSEC;

	fprintf(stderr, "bgload pid=%d threads=%d period=%d ms burst=%d ms\n",
		getpid(), opt_threads, opt_period_ms, opt_burst_ms);
	for (i = 0; i < opt_threads; i++)
		if (pthread_create(&th[i], NULL, burner, (void *)i))
			return 1;
	for (i = 0; i < opt_threads; i++)
		pthread_join(th[i], NULL);

	if (opt_log) {
		FILE *f = fopen(opt_log, "w");
		size_t k;

		if (!f) {
			fprintf(stderr, "bgload: cannot write %s: %s\n", opt_log,
				strerror(errno));
			return 1;
		}
		for (k = 0; k < nbursts; k++)
			fprintf(f, "{\"burst\":%zu,\"start_ns\":%llu,\"end_ns\":%llu}\n",
				k, (unsigned long long)bursts[k].start,
				(unsigned long long)bursts[k].end);
		fclose(f);
	}
	return 0;
}
