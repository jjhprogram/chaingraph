// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/*
 * Unprivileged self-test for src/syms.c.
 *
 *   gcc -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -Isrc \
 *       tests/syms_selftest.c src/syms.c -lelf -lz -o build/syms_selftest
 *
 * Temporary files (mutated ELF copies) are created next to the test binary
 * and removed again; /tmp/perf-<pid>.map is created briefly for the JIT
 * test. Prints PASS/FAIL per check, exits non-zero if anything failed.
 * SYMS_SELFTEST_FUZZ=<n> sets the number of mutated ELF copies (default 300).
 */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <gelf.h>
#include <inttypes.h>
#include <libelf.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "syms.h"

/* test hook exported by syms.c, deliberately not in syms.h */
struct ksyms *ksyms__load_path(const char *path, int keep_zero);

#define PAGE	4096UL

static int n_pass, n_fail;
static char self_exe[PATH_MAX];
static char self_dir[PATH_MAX];
static char perfmap_path[64];

__attribute__((format(printf, 2, 3)))
static void check(bool ok, const char *fmt, ...)
{
	va_list ap;

	printf("%s: ", ok ? "PASS" : "FAIL");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
	if (ok)
		n_pass++;
	else
		n_fail++;
}

static const char *str(const char *s)
{
	return s ? s : "(null)";
}

static double ms_since(const struct timespec *t0)
{
	struct timespec t1;

	clock_gettime(CLOCK_MONOTONIC, &t1);
	return (double)(t1.tv_sec - t0->tv_sec) * 1e3 +
	       (double)(t1.tv_nsec - t0->tv_nsec) / 1e6;
}

/* ---- functions and data to resolve ------------------------------------- */

#ifdef __clang__
#define TEST_FN	__attribute__((noinline, used))
#else
#define TEST_FN	__attribute__((noinline, noclone, used))
#endif

TEST_FN
int cg_selftest_target(int n)
{
	volatile int acc = 0;
	int i;

	for (i = 0; i < n; i++)
		acc += i * 7 + (acc >> 3);
	return acc;
}

TEST_FN
static int cg_selftest_local(int n)
{
	volatile int acc = 1;
	int i;

	for (i = 0; i < n; i++)
		acc ^= acc * 31 + i;
	return acc;
}

int cg_selftest_data = 42;

static uint64_t addr_of_target(void)
{
	int (*volatile fp)(int) = cg_selftest_target;

	return (uint64_t)(uintptr_t)fp;
}

static uint64_t addr_of_local(void)
{
	int (*volatile fp)(int) = cg_selftest_local;

	return (uint64_t)(uintptr_t)fp;
}

static uint64_t addr_of_getpid(void)
{
	pid_t (*volatile fp)(void) = getpid;

	return (uint64_t)(uintptr_t)fp;
}

/* ---- helpers ------------------------------------------------------------ */

struct exec_map {
	uint64_t start, end, pgoff;
	char path[PATH_MAX];
};

/* First executable mapping of pid whose path contains needle. */
static bool find_exec_map(pid_t pid, const char *needle, struct exec_map *out)
{
	char mpath[64], line[PATH_MAX + 128], perms[8], path[PATH_MAX];
	unsigned long start, end, pgoff;
	bool found = false;
	FILE *f;
	int n;

	snprintf(mpath, sizeof(mpath), "/proc/%d/maps", pid);
	f = fopen(mpath, "re");
	if (!f)
		return false;
	while (fgets(line, sizeof(line), f)) {
		path[0] = '\0';
		n = sscanf(line, "%lx-%lx %7s %lx %*s %*s %4095[^\n]", &start,
			   &end, perms, &pgoff, path);
		if (n < 4 || !strchr(perms, 'x') || !strstr(path, needle))
			continue;
		out->start = start;
		out->end = end;
		out->pgoff = pgoff;
		snprintf(out->path, sizeof(out->path), "%s", path);
		found = true;
		break;
	}
	fclose(f);
	return found;
}

static int read_file(const char *path, unsigned char **buf, size_t *len)
{
	struct stat st;
	size_t got = 0;
	ssize_t r;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0 || fstat(fd, &st)) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	*buf = malloc((size_t)st.st_size + 1);
	if (!*buf) {
		close(fd);
		return -1;
	}
	while (got < (size_t)st.st_size) {
		r = read(fd, *buf + got, (size_t)st.st_size - got);
		if (r <= 0)
			break;
		got += (size_t)r;
	}
	close(fd);
	*len = got;
	return 0;
}

static int write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	ssize_t r;

	while (len) {
		r = write(fd, p, len);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += r;
		len -= (size_t)r;
	}
	return 0;
}

/* Create a unique file next to the test binary with the given contents. */
static int make_temp_file(const char *tag, const void *buf, size_t len,
			  char *path, size_t path_sz)
{
	int fd;

	fd = snprintf(path, path_sz, "%s/.syms_selftest_%s_XXXXXX", self_dir,
		      tag);
	if (fd < 0 || (size_t)fd >= path_sz) {
		errno = ENAMETOOLONG;
		return -1;
	}
	fd = mkstemp(path);
	if (fd < 0)
		return -1;
	if (write_all(fd, buf, len)) {
		close(fd);
		unlink(path);
		return -1;
	}
	return fd;
}

static void cleanup_perfmap(void)
{
	if (perfmap_path[0])
		unlink(perfmap_path);
}

/* ---- kernel ------------------------------------------------------------- */

static void test_ksyms_real(void)
{
	char line[1024], name[512], type;
	unsigned long a, sched = 0;
	struct timespec t0;
	struct ksyms *ks;
	size_t lines = 0;
	uint64_t off;
	const char *s, *mod;
	double ms, best = 1e9;
	int i;
	FILE *f;

	f = fopen("/proc/kallsyms", "re");
	if (!f) {
		check(false, "ksyms: cannot open /proc/kallsyms: %s",
		      strerror(errno));
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		lines++;
		if (sscanf(line, "%lx %c %511s", &a, &type, name) == 3 &&
		    (type == 'T' || type == 't') && !strcmp(name, "schedule") &&
		    !strchr(line, '['))
			sched = a;
	}
	fclose(f);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	ks = ksyms_load();
	ms = ms_since(&t0);
	printf("INFO: ksyms_load(): %.1f ms over %zu lines, %s\n", ms, lines,
	       ks ? "addresses visible" : "addresses hidden -> NULL");
	if (!ks) {
		check(sched == 0,
		      "ksyms: ksyms_load() is NULL and kallsyms addresses read as 0 (schedule=%#lx)",
		      sched);
	} else {
		s = ksyms_lookup(ks, sched, &off, &mod);
		check(sched && s && !strcmp(s, "schedule") && off == 0 && !mod,
		      "ksyms: schedule %#lx -> %s+%#" PRIx64 " [%s]", sched,
		      str(s), off, str(mod));
		s = ksyms_lookup(ks, sched + 1, &off, NULL);
		check(s && !strcmp(s, "schedule") && off == 1,
		      "ksyms: schedule+1 -> %s+%#" PRIx64, str(s), off);
		ksyms_free(ks);
	}

	/* Time a full parse with zero addresses kept (unprivileged view). */
	for (i = 0; i < 5; i++) {
		clock_gettime(CLOCK_MONOTONIC, &t0);
		ks = ksyms__load_path("/proc/kallsyms", 1);
		ms = ms_since(&t0);
		if (ms < best)
			best = ms;
		if (i == 4) {
			check(ks != NULL,
			      "ksyms: full parse+sort of /proc/kallsyms kept with keep_zero");
			s = ksyms_lookup(ks, sched, &off, NULL);
			check(s != NULL, "ksyms: lookup in zero-address table -> %s",
			      str(s));
		}
		ksyms_free(ks);
	}
	printf("INFO: parsing /proc/kallsyms (%zu lines, keep_zero): best of 5 = %.1f ms\n",
	       lines, best);
}

static struct ksyms *load_synthetic(const char *text, size_t len, int keep_zero)
{
	struct ksyms *ks = NULL;
	char path[64];
	int fd;

	fd = memfd_create("fake_kallsyms", MFD_CLOEXEC);
	if (fd < 0)
		return NULL;
	if (!write_all(fd, text, len)) {
		snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
		ks = ksyms__load_path(path, keep_zero);
	}
	close(fd);
	return ks;
}

static void kcheck(struct ksyms *ks, uint64_t addr, const char *want,
		   uint64_t want_off, const char *want_mod)
{
	const char *mod = "unset", *s;
	uint64_t off = ~0ULL;

	s = ksyms_lookup(ks, addr, &off, &mod);
	if (!want) {
		check(!s, "ksyms synthetic: %#" PRIx64 " -> %s (want NULL)",
		      addr, str(s));
		return;
	}
	check(s && !strcmp(s, want) && off == want_off &&
	      ((!want_mod && !mod) || (want_mod && mod && !strcmp(mod, want_mod))),
	      "ksyms synthetic: %#" PRIx64 " -> %s+%#" PRIx64 " [%s] (want %s+%#" PRIx64 " [%s])",
	      addr, str(s), off, str(mod), want, want_off, str(want_mod));
}

static void test_ksyms_synthetic(void)
{
	static const char text[] =
		"ffffffffc0002000 t bpf_prog_0123456789abcdef_handle_exec\t[bpf]\n"
		"ffffffff81000100 T schedule\n"
		"ffffffff81000000 t _text_local_alias\n"
		"ffffffff81000000 T _stext\n"
		"ffffffff81000200 D some_data\n"
		"ffffffff81000180 W weak_fn\n"
		"ffffffff81000280 D bpf_prog_active\n"
		"ffffffffc0002200 b bpf_prog_fedcba9876543210_odd_type\t[bpf]\n"
		"ffffffffc0004000 t ftrace_trampoline\t[__builtin__ftrace]\n"
		"ffffffffc0005000 t unterminated_mod\t[oops\n"
		"ffffffffc0001000 t mod_fn\t[nf_tables]\n"
		"ffffffffc0001100 T mod_fn2\t[nf_tables]\n"
		"ffffffffc0003000 t ext4_fn\t[ext4]\n"
		"not a valid line\n"
		"ffffffffZZ t broken\n"
		"ffffffff81000300 t\n"
		"\n"
		"12345678901234567890 T overflow\n"
		"ffffffff81000400 t last_vmlinux_fn";	/* no trailing newline */
	const char *m1, *m2;
	struct ksyms *ks;
	char *big;
	size_t n;

	ks = load_synthetic(text, sizeof(text) - 1, 0);
	check(ks != NULL, "ksyms synthetic: loaded");
	if (ks) {
		kcheck(ks, 0xffffffff80ffffffULL, NULL, 0, NULL);
		kcheck(ks, 0xffffffff81000000ULL, "_stext", 0, NULL);
		kcheck(ks, 0xffffffff81000105ULL, "schedule", 5, NULL);
		kcheck(ks, 0xffffffff81000250ULL, "weak_fn", 0xd0, NULL);
		kcheck(ks, 0xffffffff81000290ULL, "weak_fn", 0x110, NULL);
		kcheck(ks, 0xffffffff81000350ULL, "weak_fn", 0x1d0, NULL);
		kcheck(ks, 0xffffffff81000400ULL, "last_vmlinux_fn", 0, NULL);
		kcheck(ks, 0xffffffffc0001050ULL, "mod_fn", 0x50, "nf_tables");
		kcheck(ks, 0xffffffffc0001100ULL, "mod_fn2", 0, "nf_tables");
		kcheck(ks, 0xffffffffc0002010ULL,
		       "bpf_prog_0123456789abcdef_handle_exec", 0x10, "bpf");
		kcheck(ks, 0xffffffffc0002201ULL,
		       "bpf_prog_fedcba9876543210_odd_type", 1, "bpf");
		kcheck(ks, 0xffffffffc0003001ULL, "ext4_fn", 1, "ext4");
		kcheck(ks, 0xffffffffc0004002ULL, "ftrace_trampoline", 2,
		       "__builtin__ftrace");
		kcheck(ks, 0xffffffffc0005003ULL, "unterminated_mod", 3, NULL);
		ksyms_lookup(ks, 0xffffffffc0001000ULL, NULL, &m1);
		ksyms_lookup(ks, 0xffffffffc0001100ULL, NULL, &m2);
		check(m1 && m1 == m2, "ksyms synthetic: module names interned");
		check(ksyms_lookup(ks, 0xffffffff81000100ULL, NULL, NULL) != NULL,
		      "ksyms synthetic: NULL out pointers accepted");
		ksyms_free(ks);
	}

	ks = load_synthetic("0000000000000000 T foo\n0000000000000000 t bar\n",
			    46, 0);
	check(!ks, "ksyms synthetic: all-zero addresses -> NULL");
	ksyms_free(ks);
	ks = load_synthetic("0000000000000000 T foo\n0000000000000000 t bar\n",
			    46, 1);
	check(ks != NULL, "ksyms synthetic: all-zero kept with keep_zero");
	ksyms_free(ks);
	ks = load_synthetic("", 0, 1);
	check(!ks, "ksyms synthetic: empty file -> NULL");
	check(!ksyms__load_path("/nonexistent/kallsyms", 1),
	      "ksyms synthetic: missing file -> NULL");

	/* A line longer than the read buffer is dropped, the next one kept. */
	n = (3U << 20) / 2;
	big = malloc(n + 128);
	if (big) {
		size_t len = 0;

		len += (size_t)sprintf(big, "ffffffff81000100 T sched\n"
				       "ffffffff81000500 T ");
		memset(big + len, 'a', n);
		len += n;
		len += (size_t)sprintf(big + len,
				       "\nffffffff81000600 T after_long\n");
		ks = load_synthetic(big, len, 0);
		check(ks != NULL, "ksyms synthetic: 1.5 MiB line tolerated");
		if (ks) {
			kcheck(ks, 0xffffffff81000550ULL, "sched", 0x450, NULL);
			kcheck(ks, 0xffffffff81000601ULL, "after_long", 1, NULL);
			ksyms_free(ks);
		}
		free(big);
	}
	check(!ksyms_lookup(NULL, 1, NULL, NULL), "ksyms: NULL table -> NULL");
	ksyms_free(NULL);
}

/* ---- user: own process -------------------------------------------------- */

static void test_self(struct usyms *us)
{
	pid_t pid = getpid();
	uint64_t fn = addr_of_target(), off = ~0ULL, gp;
	const char *dso = NULL, *s;
	unsigned long vdso;
	char *heap;
	void *pg;
	struct usyms *fresh;

	s = usyms_lookup(us, pid, fn, &off, &dso);
	check(s && !strcmp(s, "cg_selftest_target") && off == 0 && dso &&
	      !strcmp(dso, self_exe),
	      "self: %#" PRIx64 " -> %s+%#" PRIx64 " (%s)", fn, str(s), off,
	      str(dso));
	s = usyms_lookup(us, pid, fn + 5, &off, &dso);
	check(s && !strcmp(s, "cg_selftest_target") && off == 5,
	      "self: target+5 -> %s+%#" PRIx64, str(s), off);
	s = usyms_lookup(us, pid, addr_of_local() + 9, &off, &dso);
	check(s && !strcmp(s, "cg_selftest_local") && off == 9,
	      "self: static function+9 -> %s+%#" PRIx64, str(s), off);
	s = usyms_lookup(us, pid, fn + 3, NULL, NULL);
	check(s && !strcmp(s, "cg_selftest_target"),
	      "self: NULL out pointers accepted -> %s", str(s));

	s = usyms_lookup(us, pid, (uint64_t)(uintptr_t)&cg_selftest_data, &off,
			 &dso);
	check(!s && dso && !strcmp(dso, self_exe),
	      "self: data variable -> %s (dso %s)", str(s), str(dso));
	heap = malloc(64);
	s = usyms_lookup(us, pid, (uint64_t)(uintptr_t)heap, &off, &dso);
	check(!s, "self: heap address -> %s (dso %s)", str(s), str(dso));
	free(heap);

	/* libc: function via GOT, resolved through libc6-dbg or .dynsym */
	gp = addr_of_getpid();
	s = usyms_lookup(us, pid, gp, &off, &dso);
	check(s && strstr(s, "getpid") && off == 0 && dso && strstr(dso, "libc"),
	      "libc: getpid %#" PRIx64 " -> %s+%#" PRIx64 " (%s)", gp, str(s),
	      off, str(dso));
	s = usyms_lookup(us, pid, gp + 4, &off, &dso);
	check(s && strstr(s, "getpid") && off == 4,
	      "libc: getpid+4 -> %s+%#" PRIx64, str(s), off);

	/* libelf ships without debuginfo here: exercises .dynsym */
	{
		unsigned (*volatile ev)(unsigned) = elf_version;
		const char *(*volatile zv)(void) = zlibVersion;

		s = usyms_lookup(us, pid, (uint64_t)(uintptr_t)ev + 1, &off,
				 &dso);
		check(s && !strcmp(s, "elf_version") && off == 1 && dso &&
		      strstr(dso, "libelf"),
		      "libelf: elf_version+1 -> %s+%#" PRIx64 " (%s)", str(s),
		      off, str(dso));
		s = usyms_lookup(us, pid, (uint64_t)(uintptr_t)zv, &off, &dso);
		check(s && !strcmp(s, "zlibVersion") && dso &&
		      strstr(dso, "libz"),
		      "libz: zlibVersion -> %s+%#" PRIx64 " (%s)", str(s), off,
		      str(dso));
	}

	vdso = getauxval(AT_SYSINFO_EHDR);
	if (!vdso) {
		check(true, "vdso: not mapped in this process (skipped)");
	} else {
		s = usyms_lookup(us, pid, vdso + 0x10, &off, &dso);
		check(!s && dso && !strcmp(dso, "[vdso]") && off == 0x10,
		      "vdso: %#lx -> %s, dso %s, offset %#" PRIx64, vdso + 0x10,
		      str(s), str(dso), off);
	}
	s = usyms_lookup(us, pid, 0xffffffffff600000ULL, &off, &dso);
	check(!s && (!dso || !strcmp(dso, "[vsyscall]")),
	      "vsyscall: -> %s, dso %s", str(s), str(dso));

	/* unmapped: reserve then release a range before this usyms reads maps */
	fresh = usyms_new();
	pg = mmap(NULL, 4 * PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (pg != MAP_FAILED)
		munmap(pg, 4 * PAGE);
	if (fresh && pg != MAP_FAILED) {
		s = usyms_lookup(fresh, pid, (uint64_t)(uintptr_t)pg + PAGE,
				 &off, &dso);
		check(!s && !dso && off == 0,
		      "unmapped: released page -> %s, dso %s", str(s), str(dso));
	}
	s = usyms_lookup(fresh, pid, 0x1000, &off, &dso);
	check(!s && !dso, "unmapped: 0x1000 -> %s, dso %s", str(s), str(dso));
	s = usyms_lookup(fresh, pid, ~0ULL, &off, &dso);
	check(!s && !dso, "unmapped: ~0 -> %s, dso %s", str(s), str(dso));
	usyms_free(fresh);
	check(!usyms_lookup(NULL, pid, fn, NULL, NULL), "usyms: NULL handle -> NULL");
	usyms_free(NULL);
}

/* ---- user: other processes ---------------------------------------------- */

static void test_bad_tgids(struct usyms *us)
{
	uint64_t fn = addr_of_target(), off = 7;
	const char *dso = "unset", *s;
	pid_t dead;
	int i;

	dead = fork();
	if (dead == 0)
		_exit(0);
	if (dead > 0)
		waitpid(dead, NULL, 0);
	for (i = 0; i < 2; i++) {
		s = usyms_lookup(us, dead, fn, &off, &dso);
		check(dead > 0 && !s && !dso && off == 0,
		      "gone: reaped pid %d (%s) -> %s, dso %s", dead,
		      i ? "negative-cached" : "first", str(s), str(dso));
	}
	check(!usyms_lookup(us, INT_MAX, fn, &off, &dso) && !dso,
	      "gone: tgid INT_MAX -> NULL");
	check(!usyms_lookup(us, 0, fn, &off, &dso) && !dso, "gone: tgid 0 -> NULL");
	check(!usyms_lookup(us, -1, fn, &off, &dso) && !dso,
	      "gone: tgid -1 -> NULL");
	if (geteuid() != 0) {
		s = usyms_lookup(us, 1, fn, &off, &dso);
		check(!s, "unreadable: init's maps as non-root -> %s", str(s));
	}
	s = usyms_lookup(us, 2, 0xffffffff81000000ULL, &off, &dso);
	check(!s, "kthreadd: user lookup -> %s", str(s));
}

#define FIXED_HINT	0x5a5a5a000000UL

static void test_forked_child(void)
{
	uint64_t fn = addr_of_target(), gp = addr_of_getpid(), anon = 0, off;
	struct usyms *uc, *up, *after;
	const char *dso, *s;
	bool fixed = false;
	int pfd[2];
	pid_t child;
	void *m;

	if (pipe2(pfd, O_CLOEXEC)) {
		check(false, "child: pipe: %s", strerror(errno));
		return;
	}
	child = fork();
	if (child < 0) {
		check(false, "child: fork: %s", strerror(errno));
		return;
	}
	if (child == 0) {
		close(pfd[0]);
		m = mmap((void *)FIXED_HINT, 2 * PAGE, PROT_READ | PROT_EXEC,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
			 0);
		if (m == MAP_FAILED)
			m = mmap(NULL, 2 * PAGE, PROT_READ | PROT_EXEC,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		anon = m == MAP_FAILED ? 0 : (uint64_t)(uintptr_t)m;
		if (write(pfd[1], &anon, sizeof(anon)) != sizeof(anon))
			_exit(1);
		for (;;)
			pause();
	}
	close(pfd[1]);
	if (read(pfd[0], &anon, sizeof(anon)) != sizeof(anon))
		anon = 0;
	close(pfd[0]);
	fixed = anon == FIXED_HINT;

	uc = usyms_new();
	s = usyms_lookup(uc, child, fn + 5, &off, &dso);
	check(s && !strcmp(s, "cg_selftest_target") && off == 5 && dso &&
	      !strcmp(dso, self_exe),
	      "child %d: target+5 -> %s+%#" PRIx64 " (%s)", child, str(s), off,
	      str(dso));
	s = usyms_lookup(uc, child, gp, &off, &dso);
	check(s && strstr(s, "getpid") && dso && strstr(dso, "libc"),
	      "child %d: getpid -> %s (%s)", child, str(s), str(dso));
	s = usyms_lookup(uc, child, anon + 8, &off, &dso);
	check(anon && !s && dso && !strcmp(dso, "[anon]") && off == 8,
	      "child %d: its anonymous exec page -> %s, dso %s", child, str(s),
	      str(dso));
	if (fixed) {
		up = usyms_new();
		s = usyms_lookup(up, getpid(), anon + 8, &off, &dso);
		check(!s && !dso,
		      "parent: child's private page is unmapped here -> dso %s",
		      str(dso));
		usyms_free(up);
	}

	kill(child, SIGKILL);
	waitpid(child, NULL, 0);

	s = usyms_lookup(uc, child, fn, &off, &dso);
	check(s && !strcmp(s, "cg_selftest_target"),
	      "child %d exited: maps cached before exit still resolve -> %s",
	      child, str(s));
	after = usyms_new();
	s = usyms_lookup(after, child, fn, &off, &dso);
	check(!s && !dso, "child %d exited: fresh cache -> %s", child, str(s));
	usyms_free(after);
	usyms_free(uc);
}

/* A different program (own ASLR layout) sharing our libc file. */
static void test_exec_child(void)
{
	struct exec_map mine, theirs;
	uint64_t gp = addr_of_getpid(), their_gp, off;
	char exe[PATH_MAX], link[64];
	const char *dso, *s;
	struct usyms *u;
	pid_t child;
	ssize_t n;
	int i;

	if (!find_exec_map(getpid(), "/libc.so", &mine)) {
		check(false, "exec child: own libc mapping not found");
		return;
	}
	child = fork();
	if (child < 0)
		return;
	if (child == 0) {
		execlp("sleep", "sleep", "30", (char *)NULL);
		_exit(127);
	}
	snprintf(link, sizeof(link), "/proc/%d/exe", child);
	for (i = 0; i < 400; i++) {
		n = readlink(link, exe, sizeof(exe) - 1);
		if (n > 0) {
			exe[n] = '\0';
			if (strcmp(exe, self_exe) &&
			    find_exec_map(child, mine.path, &theirs))
				break;
		}
		usleep(5000);
	}
	if (i == 400) {
		check(false, "exec child: sleep did not start");
		goto out;
	}
	their_gp = gp - mine.start + mine.pgoff - theirs.pgoff + theirs.start;
	u = usyms_new();
	s = usyms_lookup(u, child, their_gp + 2, &off, &dso);
	check(s && strstr(s, "getpid") && off == 2 && dso &&
	      !strcmp(dso, mine.path),
	      "exec child %d (%s): libc getpid+2 at %#" PRIx64 " -> %s+%#" PRIx64 " (%s)",
	      child, exe, their_gp + 2, str(s), off, str(dso));
	if (find_exec_map(child, exe, &theirs)) {
		s = usyms_lookup(u, child, theirs.start + 1, &off, &dso);
		check(dso && !strcmp(dso, exe),
		      "exec child %d: main text -> %s (%s)", child, str(s),
		      str(dso));
	}
	usyms_free(u);
out:
	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
}

/* ---- user: JIT perf map ------------------------------------------------- */

static void test_perfmap(void)
{
	uint64_t base, off;
	const char *dso, *s;
	struct usyms *u;
	void *jit;
	int fd;

	jit = mmap(NULL, 3 * PAGE, PROT_READ | PROT_EXEC,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (jit == MAP_FAILED) {
		check(false, "perfmap: mmap: %s", strerror(errno));
		return;
	}
	base = (uint64_t)(uintptr_t)jit;
	snprintf(perfmap_path, sizeof(perfmap_path), "/tmp/perf-%d.map",
		 getpid());
	fd = open(perfmap_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0) {
		printf("SKIP: perfmap: cannot create %s: %s\n", perfmap_path,
		       strerror(errno));
		perfmap_path[0] = '\0';
		munmap(jit, 3 * PAGE);
		return;
	}
	dprintf(fd, "%" PRIx64 " 40 cg_jit_stale\n", base);
	dprintf(fd, "%" PRIx64 " 40 cg_jit_func\n", base);
	dprintf(fd, "0x%" PRIx64 " 0x20 LazyCompile:*cg jit with spaces\r\n",
		base + 0x40);
	dprintf(fd, "garbage line without fields\n");
	dprintf(fd, "%" PRIx64 " zz bad_size\n", base + 0x100);
	dprintf(fd, "%" PRIx64 " 10 cg_jit_page2\n", base + PAGE);
	close(fd);

	u = usyms_new();
	s = usyms_lookup(u, getpid(), base + 0x10, &off, &dso);
	check(s && !strcmp(s, "cg_jit_func") && off == 0x10 && dso &&
	      !strcmp(dso, "[anon]"),
	      "perfmap: jit+0x10 -> %s+%#" PRIx64 " (%s)", str(s), off,
	      str(dso));
	s = usyms_lookup(u, getpid(), base + 0x45, &off, &dso);
	check(s && !strcmp(s, "LazyCompile:*cg jit with spaces") && off == 5,
	      "perfmap: jit+0x45 -> '%s'+%#" PRIx64, str(s), off);
	s = usyms_lookup(u, getpid(), base + 0x80, &off, &dso);
	check(!s && dso && !strcmp(dso, "[anon]"),
	      "perfmap: gap jit+0x80 -> %s (%s)", str(s), str(dso));
	s = usyms_lookup(u, getpid(), base + PAGE + 1, &off, &dso);
	check(s && !strcmp(s, "cg_jit_page2") && off == 1,
	      "perfmap: jit+page+1 -> %s+%#" PRIx64, str(s), off);
	usyms_free(u);

	cleanup_perfmap();
	perfmap_path[0] = '\0';
	munmap(jit, 3 * PAGE);
}

/* ---- user: files that are not what the maps say ------------------------- */

static void test_deleted_file(void)
{
	struct exec_map em;
	unsigned char *img;
	char path[PATH_MAX], want[PATH_MAX + 16];
	uint64_t fn = addr_of_target(), fn_off, addr, off;
	const char *dso, *s;
	struct usyms *u;
	size_t len;
	void *m;
	int fd, fd2;

	if (!find_exec_map(getpid(), self_exe, &em) || fn < em.start ||
	    fn >= em.end || read_file(self_exe, &img, &len)) {
		check(false, "deleted: cannot locate own text mapping");
		return;
	}
	fn_off = fn - em.start + em.pgoff;
	fd = make_temp_file("del", img, len, path, sizeof(path));
	if (fd < 0) {
		check(false, "deleted: cannot create copy: %s", strerror(errno));
		free(img);
		return;
	}
	m = mmap(NULL, em.end - em.start, PROT_READ | PROT_EXEC, MAP_PRIVATE,
		 fd, (off_t)em.pgoff);
	close(fd);
	if (m == MAP_FAILED) {
		check(false, "deleted: mmap copy: %s", strerror(errno));
		unlink(path);
		free(img);
		return;
	}
	addr = (uint64_t)(uintptr_t)m + fn_off - em.pgoff;

	u = usyms_new();
	s = usyms_lookup(u, getpid(), addr, &off, &dso);
	check(s && !strcmp(s, "cg_selftest_target") && off == 0 && dso &&
	      !strcmp(dso, path),
	      "copy: mapped copy of the test binary -> %s (%s)", str(s),
	      str(dso));
	usyms_free(u);

	/* Replace the file under the same name: different inode, same bytes */
	unlink(path);
	fd2 = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd2 >= 0) {
		write_all(fd2, img, len);
		close(fd2);
	}
	snprintf(want, sizeof(want), "%s (deleted)", path);
	u = usyms_new();
	s = usyms_lookup(u, getpid(), addr, &off, &dso);
	if (geteuid() == 0)
		check(s && !strcmp(s, "cg_selftest_target"),
		      "deleted: root reads map_files -> %s", str(s));
	else
		check(!s && dso && !strcmp(dso, want) && off == fn_off,
		      "deleted: replacement file with another inode ignored -> %s (%s)",
		      str(s), str(dso));
	usyms_free(u);

	munmap(m, em.end - em.start);
	unlink(path);
	free(img);
}

/* ---- robustness: mutated ELF files -------------------------------------- */

static uint64_t rng = 0x2545f4914f6cdd1dULL;

static uint64_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

struct region {
	size_t off, len;
};

static void scribble(unsigned char *b, size_t len, struct region r, int count)
{
	if (r.off >= len || !r.len)
		return;
	if (r.len > len - r.off)
		r.len = len - r.off;
	while (count-- > 0)
		b[r.off + rnd() % r.len] = (unsigned char)rnd();
}

static void put_extreme(unsigned char *b, size_t len, size_t off, size_t width)
{
	static const uint64_t vals[] = {
		0, 1, 2, 3, 0x7f, 0x80, 0xff, 0xffff, 0x10000, 0x7fffffff,
		0x80000000, 0xffffffff, 0x100000000ULL, 0x7fffffffffffffffULL,
		0xffffffffffffffffULL, 0xfffffffffffffff0ULL, 0x1000, 0x18,
	};
	uint64_t v = vals[rnd() % (sizeof(vals) / sizeof(vals[0]))];

	if (width > 8 || off > len || len - off < width)
		return;
	if (rnd() % 4 == 0)
		v = rnd();
	memcpy(b + off, &v, width);	/* x86_64: little endian */
}

struct fuzz_target {
	const char *label;
	const char *path;
	uint64_t text_off;	/* file offset of the page-aligned exec PT_LOAD */
	uint64_t text_len;
	uint64_t fn_off;	/* file offset of a function that must resolve */
	const char *fn_name;	/* substring expected for the baseline */
};

static void mutate(unsigned char *b, size_t *len, const Elf64_Ehdr *eh,
		   const struct region *hot, size_t nhot)
{
	static const struct { unsigned off, width; } ehdr_fields[] = {
		{ 4, 1 }, { 5, 1 }, { 16, 2 }, { 18, 2 }, { 32, 8 }, { 40, 8 },
		{ 54, 2 }, { 56, 2 }, { 58, 2 }, { 60, 2 }, { 62, 2 },
	};
	static const struct { unsigned off, width; } shdr_fields[] = {
		{ 0, 4 }, { 4, 4 }, { 8, 8 }, { 24, 8 }, { 32, 8 }, { 40, 4 },
		{ 44, 4 }, { 48, 8 }, { 56, 8 },
	};
	static const struct { unsigned off, width; } phdr_fields[] = {
		{ 0, 4 }, { 8, 8 }, { 16, 8 }, { 32, 8 }, { 40, 8 }, { 48, 8 },
	};
	struct region r;
	size_t i;
	int k;

	switch (rnd() % 7) {
	case 0:
		r.off = 0;
		r.len = sizeof(*eh);
		scribble(b, *len, r, 1 + (int)(rnd() % 4));
		break;
	case 1:
		r.off = eh->e_phoff;
		r.len = (size_t)eh->e_phnum * sizeof(Elf64_Phdr);
		scribble(b, *len, r, 1 + (int)(rnd() % 8));
		break;
	case 2:
		r.off = eh->e_shoff;
		r.len = (size_t)eh->e_shnum * sizeof(Elf64_Shdr);
		scribble(b, *len, r, 1 + (int)(rnd() % 16));
		break;
	case 3:
		if (nhot) {
			r = hot[rnd() % nhot];
			scribble(b, *len, r, 1 + (int)(rnd() % 64));
		}
		break;
	case 4:
		*len = (size_t)(rnd() % *len);
		break;
	case 5:
		for (k = 0; k < 3; k++) {
			i = rnd() % (sizeof(shdr_fields) / sizeof(shdr_fields[0]));
			put_extreme(b, *len, eh->e_shoff +
				    (rnd() % (eh->e_shnum ? eh->e_shnum : 1)) *
				    sizeof(Elf64_Shdr) + shdr_fields[i].off,
				    shdr_fields[i].width);
			i = rnd() % (sizeof(phdr_fields) / sizeof(phdr_fields[0]));
			put_extreme(b, *len, eh->e_phoff +
				    (rnd() % (eh->e_phnum ? eh->e_phnum : 1)) *
				    sizeof(Elf64_Phdr) + phdr_fields[i].off,
				    phdr_fields[i].width);
		}
		break;
	default:
		i = rnd() % (sizeof(ehdr_fields) / sizeof(ehdr_fields[0]));
		put_extreme(b, *len, ehdr_fields[i].off, ehdr_fields[i].width);
		break;
	}
	if (rnd() % 4 == 0 && *len) {
		r.off = 0;
		r.len = *len;
		scribble(b, *len, r, 1 + (int)(rnd() % 32));
	}
}

static void fuzz_elf(const struct fuzz_target *t, int iters)
{
	unsigned char *orig, *buf;
	struct region hot[64];
	size_t len, mlen, nhot = 0, i;
	char path[PATH_MAX];
	const Elf64_Shdr *sh;
	Elf64_Ehdr eh;
	uint64_t base, addrs[6], off;
	const char *dso, *s;
	struct usyms *u;
	bool baseline_ok = false;
	int it, fd, j, named = 0, hits = 0;
	void *m;

	if (read_file(t->path, &orig, &len) || len < sizeof(eh)) {
		check(false, "fuzz %s: cannot read %s", t->label, t->path);
		return;
	}
	memcpy(&eh, orig, sizeof(eh));
	if (eh.e_shoff < len && eh.e_shentsize == sizeof(Elf64_Shdr) &&
	    (len - eh.e_shoff) / sizeof(Elf64_Shdr) >= eh.e_shnum) {
		for (i = 0; i < eh.e_shnum && nhot < 64; i++) {
			sh = (const Elf64_Shdr *)(void *)(orig + eh.e_shoff +
							  i * sizeof(*sh));
			if (sh->sh_type == SHT_SYMTAB || sh->sh_type == SHT_DYNSYM ||
			    sh->sh_type == SHT_STRTAB || sh->sh_type == SHT_NOTE ||
			    sh->sh_type == SHT_PROGBITS) {
				if (sh->sh_type == SHT_PROGBITS &&
				    sh->sh_size > 256)
					continue;	/* small: .gnu_debuglink etc */
				hot[nhot].off = sh->sh_offset;
				hot[nhot].len = sh->sh_size;
				nhot++;
			}
		}
	}
	buf = malloc(len);
	if (!buf) {
		free(orig);
		return;
	}

	for (it = -1; it < iters; it++) {
		size_t cur = len;

		memcpy(buf, orig, len);
		if (it >= 0)
			mutate(buf, &cur, &eh, hot, nhot);
		fd = make_temp_file("fuzz", buf, cur, path, sizeof(path));
		if (fd < 0) {
			check(false, "fuzz %s: temp file: %s", t->label,
			      strerror(errno));
			break;
		}
		mlen = t->text_len;
		m = mmap(NULL, mlen, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd,
			 (off_t)t->text_off);
		close(fd);
		if (m == MAP_FAILED) {
			unlink(path);
			continue;
		}
		base = (uint64_t)(uintptr_t)m;
		addrs[0] = base + t->fn_off - t->text_off;
		addrs[1] = base;
		addrs[2] = base + mlen - 1;
		addrs[3] = base + rnd() % mlen;
		addrs[4] = base + rnd() % mlen;
		addrs[5] = addrs[0] + 1;

		u = usyms_new();
		for (j = 0; j < 6; j++) {
			s = usyms_lookup(u, getpid(), addrs[j], &off, &dso);
			if (s) {
				(void)strlen(s);	/* must be a valid string */
				if (it >= 0)
					hits++;
				if (it >= 0 && j == 0 && strstr(s, t->fn_name))
					named++;
			}
			if (dso)
				(void)strlen(dso);
			if (it < 0 && j == 0)
				baseline_ok = s && strstr(s, t->fn_name) &&
					      off == 0 && dso &&
					      !strcmp(dso, path);
		}
		usyms_free(u);
		munmap(m, mlen);
		unlink(path);
		if (it < 0)
			check(baseline_ok,
			      "fuzz %s: unmutated copy resolves %s via copied path",
			      t->label, t->fn_name);
	}
	printf("INFO: fuzz %s: %d/%d mutated copies still resolved %s, %d names returned\n",
	       t->label, named, iters, t->fn_name, hits);
	check(it == iters, "fuzz %s: %d mutated ELF copies looked up without crashing",
	      t->label, iters);
	free(buf);
	free(orig);
}

static void test_fuzz(void)
{
	struct fuzz_target self = { .label = "selftest", .path = self_exe,
				    .fn_name = "cg_selftest_target" };
	struct fuzz_target libc = { .label = "libc", .fn_name = "getpid" };
	struct exec_map em;
	uint64_t a;
	const char *junk = "this is not an ELF file\n";
	char path[PATH_MAX];
	const char *dso, *s;
	struct usyms *u;
	uint64_t off;
	void *m;
	int fd;

	int iters = 300;
	const char *env = getenv("SYMS_SELFTEST_FUZZ");	/* longer soak runs */

	if (env && atoi(env) > 0)
		iters = atoi(env);
	a = addr_of_target();
	if (find_exec_map(getpid(), self_exe, &em)) {
		self.text_off = em.pgoff;
		self.text_len = em.end - em.start;
		self.fn_off = a - em.start + em.pgoff;
		fuzz_elf(&self, iters);
	}
	a = addr_of_getpid();
	if (find_exec_map(getpid(), "/libc.so", &em)) {
		libc.path = strdup(em.path);
		libc.text_off = em.pgoff;
		libc.text_len = em.end - em.start;
		libc.fn_off = a - em.start + em.pgoff;
		fuzz_elf(&libc, iters / 5 ? iters / 5 : 1);
		free((char *)libc.path);
	}

	/* not an ELF at all */
	fd = make_temp_file("junk", junk, strlen(junk), path, sizeof(path));
	if (fd >= 0) {
		m = mmap(NULL, PAGE, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
		close(fd);
		if (m != MAP_FAILED) {
			u = usyms_new();
			s = usyms_lookup(u, getpid(), (uint64_t)(uintptr_t)m + 3,
					 &off, &dso);
			check(!s && dso && !strcmp(dso, path) && off == 3,
			      "junk: non-ELF executable mapping -> %s (%s+%#" PRIx64 ")",
			      str(s), str(dso), off);
			usyms_free(u);
			munmap(m, PAGE);
		}
		unlink(path);
	}
}

/* ---- throughput --------------------------------------------------------- */

static void bench_lookups(struct usyms *us)
{
	uint64_t addrs[4] = { addr_of_target() + 7, addr_of_getpid() + 1,
			      addr_of_local(),
			      (uint64_t)(uintptr_t)getauxval(AT_SYSINFO_EHDR) };
	struct timespec t0;
	const char *dso;
	uint64_t off;
	int i, n = 400000;
	pid_t pid = getpid();
	double ms;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < n; i++)
		usyms_lookup(us, pid, addrs[i & 3], &off, &dso);
	ms = ms_since(&t0);
	printf("INFO: %d cached user lookups in %.1f ms (%.0f ns each)\n", n, ms,
	       ms * 1e6 / n);
}

int main(void)
{
	struct usyms *us;
	ssize_t n;
	char *slash;

	setvbuf(stdout, NULL, _IOLBF, 0);
	n = readlink("/proc/self/exe", self_exe, sizeof(self_exe) - 1);
	if (n <= 0) {
		fprintf(stderr, "readlink /proc/self/exe: %s\n", strerror(errno));
		return 2;
	}
	self_exe[n] = '\0';
	snprintf(self_dir, sizeof(self_dir), "%s", self_exe);
	slash = strrchr(self_dir, '/');
	if (slash)
		*slash = '\0';
	atexit(cleanup_perfmap);
	printf("INFO: pid %d euid %d exe %s\n", getpid(), geteuid(), self_exe);

	test_ksyms_real();
	test_ksyms_synthetic();

	us = usyms_new();
	check(us != NULL, "usyms_new()");
	if (!us)
		return 1;
	test_self(us);
	test_bad_tgids(us);
	test_forked_child();
	test_exec_child();
	test_perfmap();
	test_deleted_file();
	bench_lookups(us);
	usyms_free(us);
	test_fuzz();

	printf("%s: %d passed, %d failed\n", n_fail ? "FAIL" : "PASS", n_pass,
	       n_fail);
	return n_fail ? 1 : 0;
}
