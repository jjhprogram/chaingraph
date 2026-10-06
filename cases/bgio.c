// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/*
 * bgio: background disk load for hitchtrace case studies.
 *
 * Stands in for a download or an update being written while a game runs:
 * mostly idle, and every PERIOD it spends BURST writing BLOCK-sized blocks
 * to a scratch file from THREADS threads at once, bypassing the page cache
 * so that every write is a request to the device, queued with the game's.
 *
 * Every burst is logged as [start, end] on CLOCK_MONOTONIC (the clock behind
 * bpf_ktime_get_ns()), so a case study can tell afterwards which frames a
 * burst overlapped without trusting the tool under test.
 *
 * The scratch file must exist and be fully written beforehand, so that a
 * burst overwrites blocks instead of allocating them.
 *
 * usage: bgio -f FILE [-t THREADS] [-p PERIOD_MS] [-b BURST_MS] [-k BLOCK_KB]
 *             [-d SECONDS] [-o LOG]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define NSEC	1000000000ULL
#define MAX_THREADS 64

static int opt_threads = 8, opt_period_ms = 1500, opt_burst_ms = 300;
static int opt_block_kb = 1024, opt_duration = 20;
static const char *opt_file, *opt_log;
static uint64_t t_start, t_stop;
static off_t file_blocks;
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

/* All threads share one grid, so they write together. */
static void *writer(void *arg)
{
	long id = (long)arg;
	uint64_t period = (uint64_t)opt_period_ms * 1000000;
	uint64_t burst = (uint64_t)opt_burst_ms * 1000000;
	uint64_t next = t_start + period;
	size_t block = (size_t)opt_block_kb * 1024;
	uint32_t rng = 0x9e3779b9u * (uint32_t)(id + 1);
	unsigned char *buf;
	char name[16];
	int fd;

	snprintf(name, sizeof(name), "bgio%ld", id);
	pthread_setname_np(pthread_self(), name);
	fd = open(opt_file, O_WRONLY | O_DIRECT);
	if (fd < 0 || posix_memalign((void **)&buf, 4096, block)) {
		fprintf(stderr, "bgio: %s: %s\n", opt_file, strerror(errno));
		exit(1);
	}
	memset(buf, 0xa5, block);

	while (!stop && next < t_stop) {
		uint64_t t0, t;

		sleep_until(next);
		if (stop)
			break;
		t0 = now_ns();
		do {
			rng ^= rng << 13;
			rng ^= rng >> 17;
			rng ^= rng << 5;
			if (pwrite(fd, buf, block,
				   (off_t)(rng % file_blocks) * block) < 0) {
				fprintf(stderr, "bgio: write: %s\n", strerror(errno));
				exit(1);
			}
			t = now_ns();
		} while (!stop && t - t0 < burst);
		if (id == 0 && nbursts < capbursts) {
			bursts[nbursts].start = t0;
			bursts[nbursts].end = t;
			nbursts++;
		}
		next += period;
		while (next < t)
			next += period;
	}
	close(fd);
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t th[MAX_THREADS];
	struct sigaction sa = { .sa_handler = on_signal };
	struct stat st;
	long i;
	int c;

	while ((c = getopt(argc, argv, "f:t:p:b:k:d:o:h")) != -1) {
		switch (c) {
		case 'f': opt_file = optarg; break;
		case 't': opt_threads = atoi(optarg); break;
		case 'p': opt_period_ms = atoi(optarg); break;
		case 'b': opt_burst_ms = atoi(optarg); break;
		case 'k': opt_block_kb = atoi(optarg); break;
		case 'd': opt_duration = atoi(optarg); break;
		case 'o': opt_log = optarg; break;
		default:
			fprintf(stderr, "usage: bgio -f FILE [-t THREADS] [-p PERIOD_MS] "
				"[-b BURST_MS] [-k BLOCK_KB] [-d SECONDS] [-o LOG]\n");
			return 2;
		}
	}
	if (!opt_file || opt_threads < 1 || opt_threads > MAX_THREADS ||
	    opt_period_ms < 1 || opt_burst_ms < 1 || opt_block_kb < 4 ||
	    opt_duration < 1)
		return 2;
	if (stat(opt_file, &st) || st.st_size < (off_t)opt_block_kb * 1024 * 16) {
		fprintf(stderr, "bgio: %s must exist and hold at least 16 blocks\n",
			opt_file);
		return 2;
	}
	file_blocks = st.st_size / ((off_t)opt_block_kb * 1024);

	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	capbursts = (size_t)opt_duration * 1000 / opt_period_ms + 16;
	bursts = calloc(capbursts, sizeof(*bursts));
	if (!bursts)
		return 1;
	t_start = now_ns();
	t_stop = t_start + (uint64_t)opt_duration * NSEC;

	fprintf(stderr, "bgio pid=%d threads=%d period=%d ms burst=%d ms block=%d KB\n",
		getpid(), opt_threads, opt_period_ms, opt_burst_ms, opt_block_kb);
	for (i = 0; i < opt_threads; i++)
		if (pthread_create(&th[i], NULL, writer, (void *)i))
			return 1;
	for (i = 0; i < opt_threads; i++)
		pthread_join(th[i], NULL);

	if (opt_log) {
		FILE *f = fopen(opt_log, "w");
		size_t k;

		if (!f) {
			fprintf(stderr, "bgio: cannot write %s: %s\n", opt_log,
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
