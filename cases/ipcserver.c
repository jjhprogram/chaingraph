// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/*
 * ipcserver: a helper process a game makes blocking calls to.
 *
 * Stands in for wineserver, or any broker a game thread asks and waits for:
 * it listens on a Unix socket, and answers each request of its one client
 * after a little work of its own. `minigame -i SOCKET` is the client. What
 * the case study varies is not this program but what happens to it: a
 * request is only as fast as the server gets to run.
 *
 * usage: ipcserver SOCKET [WORK_US]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static void burn_us(long us)
{
	struct timespec c0, c;
	volatile uint64_t sink = 0;
	int k;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c0);
	do {
		for (k = 0; k < 2000; k++)
			sink += k * 2654435761u;
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c);
	} while ((c.tv_sec - c0.tv_sec) * 1000000000ll + c.tv_nsec - c0.tv_nsec <
		 us * 1000ll);
}

int main(int argc, char **argv)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	long work_us = argc > 2 ? atol(argv[2]) : 20;
	char msg[64];
	int lfd, fd;

	if (argc < 2 || strlen(argv[1]) >= sizeof(addr.sun_path)) {
		fprintf(stderr, "usage: ipcserver SOCKET [WORK_US]\n");
		return 2;
	}
	pthread_setname_np(pthread_self(), "mg_server");
	strcpy(addr.sun_path, argv[1]);
	unlink(argv[1]);
	lfd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (lfd < 0 || bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) ||
	    listen(lfd, 1)) {
		fprintf(stderr, "ipcserver: %s: %s\n", argv[1], strerror(errno));
		return 1;
	}
	fprintf(stderr, "ipcserver pid=%d socket=%s work=%ld us\n", getpid(),
		argv[1], work_us);
	fd = accept(lfd, NULL, NULL);
	if (fd < 0)
		return 1;
	while (read(fd, msg, sizeof(msg)) == sizeof(msg)) {
		if (work_us)
			burn_us(work_us);
		if (write(fd, msg, sizeof(msg)) != sizeof(msg))
			break;
	}
	unlink(argv[1]);
	return 0;
}
