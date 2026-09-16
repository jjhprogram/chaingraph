// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/*
 * Symbol resolution for kernel and user stack addresses.
 *
 * Kernel: text symbols from /proc/kallsyms, sorted by address, looked up by
 * binary search for the greatest start <= address.
 *
 * User: /proc/<pid>/maps is read once per process and cached. Executable
 * file mappings are resolved through the ELF symbol table of the mapped
 * file, parsed once per (st_dev, st_ino) and shared by every process that
 * maps it. Stripped files fall back to separate debuginfo (build-id, then
 * .gnu_debuglink) and finally .dynsym. Anonymous executable memory (JIT
 * code) is resolved through /tmp/perf-<pid>.map when one exists.
 *
 * Every string handed out lives in an arena owned by the ksyms/usyms
 * object and stays valid until it is freed.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <gelf.h>
#include <inttypes.h>
#include <libelf.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "syms.h"

#define KALLSYMS_BUF		(1U << 20)
#define KSYMS_STRS_MAX		(1UL << 31)	/* offsets must fit 31 bits */
#define MAPS_READ_LIMIT		(64UL << 20)
#define PERFMAP_READ_LIMIT	(512UL << 20)
#define MAPS_REFRESH_NS		1000000000ULL	/* re-read maps on a miss */
#define ENCLOSE_SCAN		16	/* earlier symbols checked for nesting */
#define BUILD_ID_MAX		64
#define DELETED_SUFFIX		" (deleted)"
#define DELETED_LEN		(sizeof(DELETED_SUFFIX) - 1)

/* ---- small helpers ----------------------------------------------------- */

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t mix64(uint64_t v)
{
	v ^= v >> 33;
	v *= 0xff51afd7ed558ccdULL;
	v ^= v >> 33;
	v *= 0xc4ceb9fe1a85ec53ULL;
	v ^= v >> 33;
	return v;
}

static uint64_t hash_bytes(const char *s, size_t len)
{
	uint64_t h = 0xcbf29ce484222325ULL;

	while (len--) {
		h ^= (unsigned char)*s++;
		h *= 0x100000001b3ULL;
	}
	return h;
}

static int hexval(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c |= 0x20;
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

/* Parse hex digits at p; returns the first unparsed char or NULL. */
static const char *parse_hex(const char *p, const char *end, uint64_t *out)
{
	const char *start = p;
	uint64_t v = 0;
	int d;

	while (p < end && (d = hexval((unsigned char)*p)) >= 0) {
		if (v >> 60)
			return NULL;
		v = v << 4 | (uint64_t)d;
		p++;
	}
	if (p == start)
		return NULL;
	*out = v;
	return p;
}

static const char *parse_dec(const char *p, const char *end, uint64_t *out)
{
	const char *start = p;
	uint64_t v = 0;

	while (p < end && *p >= '0' && *p <= '9') {
		unsigned d = (unsigned)(*p - '0');

		if (v > (UINT64_MAX - d) / 10)
			return NULL;
		v = v * 10 + d;
		p++;
	}
	if (p == start)
		return NULL;
	*out = v;
	return p;
}

/* Read an entire file (sizes of /proc files are not known up front). */
static int read_fd_all(int fd, size_t limit, char **out, size_t *out_len)
{
	char *buf = NULL, *nbuf;
	size_t len = 0, cap = 0, ncap;
	ssize_t r;
	int err;

	for (;;) {
		if (len == cap) {
			if (cap >= limit) {
				free(buf);
				return -EFBIG;
			}
			ncap = cap ? cap * 2 : 16384;
			if (ncap > limit)
				ncap = limit;
			nbuf = realloc(buf, ncap + 1);
			if (!nbuf) {
				free(buf);
				return -ENOMEM;
			}
			buf = nbuf;
			cap = ncap;
		}
		r = read(fd, buf + len, cap - len);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			err = -errno;
			free(buf);
			return err;
		}
		if (r == 0)
			break;
		len += (size_t)r;
	}
	if (!buf) {
		buf = malloc(1);
		if (!buf)
			return -ENOMEM;
	}
	buf[len] = '\0';
	*out = buf;
	*out_len = len;
	return 0;
}

/*
 * Open a path for reading only if it is a regular file. O_NONBLOCK keeps a
 * FIFO planted where a library or perf map is expected from hanging us.
 */
static int open_regular(const char *path, int extra_flags, struct stat *st)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY |
			    extra_flags);

	if (fd < 0)
		return -1;
	if (fstat(fd, st) || !S_ISREG(st->st_mode)) {
		close(fd);
		return -1;
	}
	return fd;
}

/* ---- string arena ------------------------------------------------------ */

#define ARENA_CHUNK	(64U << 10)

struct arena_chunk {
	struct arena_chunk *next;
	size_t used, cap;
	char data[];
};

struct arena {
	struct arena_chunk *head;
};

static char *arena_alloc(struct arena *a, size_t n)
{
	struct arena_chunk *c = a->head;
	size_t cap;
	char *p;

	if (n > SIZE_MAX / 2)
		return NULL;
	if (!c || c->cap - c->used < n) {
		cap = n > ARENA_CHUNK / 4 ? n : ARENA_CHUNK;
		c = malloc(sizeof(*c) + cap);
		if (!c)
			return NULL;
		c->used = 0;
		c->cap = cap;
		if (cap != ARENA_CHUNK && a->head) {
			/* dedicated big block; keep filling the current one */
			c->next = a->head->next;
			a->head->next = c;
		} else {
			c->next = a->head;
			a->head = c;
		}
	}
	p = c->data + c->used;
	c->used += n;
	return p;
}

static char *arena_strndup(struct arena *a, const char *s, size_t len)
{
	char *p = arena_alloc(a, len + 1);

	if (!p)
		return NULL;
	memcpy(p, s, len);
	p[len] = '\0';
	return p;
}

static void arena_free(struct arena *a)
{
	struct arena_chunk *c = a->head, *next;

	for (; c; c = next) {
		next = c->next;
		free(c);
	}
	a->head = NULL;
}

/* Interned strings: equal inputs return the same arena pointer. */
struct strset {
	const char **slots;
	size_t cap, n;
};

static bool str_eq(const char *interned, const char *s, size_t len)
{
	return strncmp(interned, s, len) == 0 && interned[len] == '\0';
}

static const char *strset_intern(struct strset *set, struct arena *a,
				 const char *s, size_t len)
{
	const char **slots;
	size_t mask, i, j;
	char *p;

	len = strnlen(s, len);
	if ((set->n + 1) * 2 > set->cap) {
		size_t ncap = set->cap ? set->cap * 2 : 256;

		slots = calloc(ncap, sizeof(*slots));
		if (!slots)
			return NULL;
		for (j = 0; j < set->cap; j++) {
			const char *old = set->slots[j];

			if (!old)
				continue;
			i = mix64(hash_bytes(old, strlen(old))) & (ncap - 1);
			while (slots[i])
				i = (i + 1) & (ncap - 1);
			slots[i] = old;
		}
		free(set->slots);
		set->slots = slots;
		set->cap = ncap;
	}
	mask = set->cap - 1;
	for (i = mix64(hash_bytes(s, len)) & mask; set->slots[i];
	     i = (i + 1) & mask) {
		if (str_eq(set->slots[i], s, len))
			return set->slots[i];
	}
	p = arena_strndup(a, s, len);
	if (!p)
		return NULL;
	set->slots[i] = p;
	set->n++;
	return p;
}

/* ======================================================================== */
/* ---- kernel ------------------------------------------------------------ */

struct ksym {
	uint64_t addr;
	uint32_t name;		/* offset into strs */
	uint32_t module : 31;	/* offset into strs; 0 = vmlinux */
	uint32_t local : 1;	/* 't'/'w' rather than 'T'/'W' */
};

struct ksyms {
	struct ksym *syms;
	size_t nsyms;
	char *strs;		/* strs[0] is "" so offset 0 means none */
};

struct kbuilder {
	struct ksym *syms;
	size_t nsyms, syms_cap;
	char *strs;
	size_t strs_len, strs_cap;
	uint32_t *mods;		/* open-addressed set of module offsets */
	size_t mods_cap, nmods;
	uint32_t last_mod;
	bool nonzero;
	bool failed;
};

static uint32_t kb_add_str(struct kbuilder *kb, const char *s, size_t len)
{
	uint32_t off;

	if (kb->strs_cap - kb->strs_len < len + 1) {
		size_t ncap = kb->strs_cap * 2;
		char *n;

		while (ncap - kb->strs_len < len + 1)
			ncap *= 2;
		if (ncap > KSYMS_STRS_MAX) {
			kb->failed = true;
			return 0;
		}
		n = realloc(kb->strs, ncap);
		if (!n) {
			kb->failed = true;
			return 0;
		}
		kb->strs = n;
		kb->strs_cap = ncap;
	}
	off = (uint32_t)kb->strs_len;
	memcpy(kb->strs + off, s, len);
	kb->strs[off + len] = '\0';
	kb->strs_len += len + 1;
	return off;
}

static uint32_t kb_module(struct kbuilder *kb, const char *s, size_t len)
{
	size_t mask, i, j;
	uint32_t off;

	/* consecutive lines almost always name the same module */
	if (kb->last_mod && str_eq(kb->strs + kb->last_mod, s, len))
		return kb->last_mod;

	if ((kb->nmods + 1) * 2 > kb->mods_cap) {
		size_t ncap = kb->mods_cap * 2;
		uint32_t *mods = calloc(ncap, sizeof(*mods));

		if (!mods) {
			kb->failed = true;
			return 0;
		}
		for (j = 0; j < kb->mods_cap; j++) {
			const char *m;

			if (!kb->mods[j])
				continue;
			m = kb->strs + kb->mods[j];
			i = mix64(hash_bytes(m, strlen(m))) & (ncap - 1);
			while (mods[i])
				i = (i + 1) & (ncap - 1);
			mods[i] = kb->mods[j];
		}
		free(kb->mods);
		kb->mods = mods;
		kb->mods_cap = ncap;
	}
	mask = kb->mods_cap - 1;
	for (i = mix64(hash_bytes(s, len)) & mask; kb->mods[i];
	     i = (i + 1) & mask) {
		if (str_eq(kb->strs + kb->mods[i], s, len))
			return kb->last_mod = kb->mods[i];
	}
	off = kb_add_str(kb, s, len);
	if (!off)
		return 0;
	kb->mods[i] = off;
	kb->nmods++;
	return kb->last_mod = off;
}

/*
 * "<hex addr> <type> <name>[\t[<module>]]". Keeps text symbols (t T w W).
 * JITed BPF programs ("bpf_prog_<tag>_<name>\t[bpf]") and other dynamic
 * text ("[__builtin__ftrace]", "[__builtin__kprobes]") arrive as 't' with a
 * pseudo-module; anything in [bpf] is kept whatever its type letter, but
 * vmlinux data such as bpf_prog_active (D) is not.
 */
static void kb_line(struct kbuilder *kb, const char *p, const char *end)
{
	const char *q, *name, *ne, *ms = NULL, *me = NULL;
	uint32_t noff, mod = 0;
	struct ksym *s;
	uint64_t addr;
	char type;

	q = parse_hex(p, end, &addr);
	if (!q || end - q < 4 || q[0] != ' ' || q[2] != ' ')
		return;
	type = q[1];
	name = q + 3;
	for (ne = name; ne < end && (unsigned char)*ne > ' '; ne++)
		;
	if (ne == name)
		return;

	for (q = ne; q < end && (*q == '\t' || *q == ' '); q++)
		;
	if (q < end && *q == '[') {
		for (me = ms = q + 1; me < end && *me != ']' &&
				      (unsigned char)*me > ' '; me++)
			;
		if (me == end || *me != ']' || me == ms)
			ms = me = NULL;
	}

	if (type != 't' && type != 'T' && type != 'w' && type != 'W' &&
	    !(ms && me - ms == 3 && !memcmp(ms, "bpf", 3)))
		return;
	if (ms) {
		mod = kb_module(kb, ms, (size_t)(me - ms));
		if (!mod)
			return;
	}

	if (kb->nsyms == kb->syms_cap) {
		size_t ncap = kb->syms_cap * 2;
		struct ksym *n = realloc(kb->syms, ncap * sizeof(*n));

		if (!n) {
			kb->failed = true;
			return;
		}
		kb->syms = n;
		kb->syms_cap = ncap;
	}
	noff = kb_add_str(kb, name, (size_t)(ne - name));
	if (!noff)
		return;
	s = &kb->syms[kb->nsyms++];
	s->addr = addr;
	s->name = noff;
	s->module = mod;
	s->local = type == 't' || type == 'w';
	if (addr)
		kb->nonzero = true;
}

/* by address; aliases: global before local, then file order */
static int ksym_cmp(const void *a, const void *b)
{
	const struct ksym *x = a, *y = b;

	if (x->addr != y->addr)
		return x->addr < y->addr ? -1 : 1;
	if (x->local != y->local)
		return x->local ? 1 : -1;
	return x->name < y->name ? -1 : x->name > y->name;
}

/*
 * Load a kallsyms-format file. With keep_zero, a table whose addresses are
 * all zero is kept (used by the self-test to time parsing unprivileged).
 * Not part of syms.h.
 */
struct ksyms *ksyms__load_path(const char *path, int keep_zero);

struct ksyms *ksyms__load_path(const char *path, int keep_zero)
{
	struct kbuilder kb = {};
	struct ksyms *ks = NULL;
	bool skipping = false;
	size_t have = 0, i, n;
	char *buf = NULL;
	const char *p, *end, *nl;
	struct ksym *shrunk;
	ssize_t r;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;

	buf = malloc(KALLSYMS_BUF);
	kb.strs_cap = 4U << 20;
	kb.strs = malloc(kb.strs_cap);
	kb.syms_cap = 1U << 16;
	kb.syms = malloc(kb.syms_cap * sizeof(*kb.syms));
	kb.mods_cap = 256;
	kb.mods = calloc(kb.mods_cap, sizeof(*kb.mods));
	if (!buf || !kb.strs || !kb.syms || !kb.mods)
		goto out;
	kb.strs[0] = '\0';
	kb.strs_len = 1;

	for (;;) {
		r = read(fd, buf + have, KALLSYMS_BUF - have);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			goto out;
		}
		if (r == 0)
			break;
		p = buf;
		end = buf + have + (size_t)r;
		while ((nl = memchr(p, '\n', (size_t)(end - p)))) {
			if (skipping)
				skipping = false;
			else
				kb_line(&kb, p, nl);
			p = nl + 1;
		}
		if (kb.failed)
			goto out;
		have = (size_t)(end - p);
		if (have == KALLSYMS_BUF) {
			/* absurdly long line: drop it */
			skipping = true;
			have = 0;
		} else if (have && p != buf) {
			memmove(buf, p, have);
		}
	}
	if (have && !skipping)
		kb_line(&kb, buf, buf + have);
	if (kb.failed || !kb.nsyms || (!kb.nonzero && !keep_zero))
		goto out;

	for (i = 1; i < kb.nsyms; i++) {
		if (ksym_cmp(&kb.syms[i - 1], &kb.syms[i]) > 0) {
			qsort(kb.syms, kb.nsyms, sizeof(*kb.syms), ksym_cmp);
			break;
		}
	}
	for (i = 1, n = 1; i < kb.nsyms; i++) {
		if (kb.syms[i].addr != kb.syms[n - 1].addr)
			kb.syms[n++] = kb.syms[i];
	}

	ks = calloc(1, sizeof(*ks));
	if (!ks)
		goto out;
	shrunk = realloc(kb.syms, n * sizeof(*shrunk));
	ks->syms = shrunk ? shrunk : kb.syms;
	ks->nsyms = n;
	ks->strs = realloc(kb.strs, kb.strs_len);
	if (!ks->strs)
		ks->strs = kb.strs;
	kb.syms = NULL;
	kb.strs = NULL;
out:
	close(fd);
	free(buf);
	free(kb.syms);
	free(kb.strs);
	free(kb.mods);
	return ks;
}

struct ksyms *ksyms_load(void)
{
	return ksyms__load_path("/proc/kallsyms", 0);
}

void ksyms_free(struct ksyms *ks)
{
	if (!ks)
		return;
	free(ks->syms);
	free(ks->strs);
	free(ks);
}

const char *ksyms_lookup(const struct ksyms *ks, uint64_t addr,
			 uint64_t *offset, const char **module)
{
	const struct ksym *s;
	size_t lo, hi, mid;

	if (offset)
		*offset = 0;
	if (module)
		*module = NULL;
	if (!ks || !ks->nsyms || addr < ks->syms[0].addr)
		return NULL;
	/* invariant: syms[lo].addr <= addr, syms[hi].addr > addr (or hi == n) */
	lo = 0;
	hi = ks->nsyms;
	while (hi - lo > 1) {
		mid = lo + (hi - lo) / 2;
		if (ks->syms[mid].addr <= addr)
			lo = mid;
		else
			hi = mid;
	}
	s = &ks->syms[lo];
	if (offset)
		*offset = addr - s->addr;
	if (module)
		*module = s->module ? ks->strs + s->module : NULL;
	return ks->strs + s->name;
}

/* ======================================================================== */
/* ---- user: ELF symbol tables ------------------------------------------- */

struct elf_seg {
	uint64_t offset, vaddr, filesz;
};

struct elf_sym {
	uint64_t addr, size;
	const char *name;
};

/* One parsed file. A failed parse is cached too (no segs/syms). */
struct elf_dso {
	struct elf_dso *next;
	dev_t dev;
	ino_t ino;
	off_t size;
	struct timespec mtime;
	struct elf_seg *segs;
	size_t nsegs;
	struct elf_sym *syms;
	size_t nsyms;
};

enum { UMAP_ANON, UMAP_FILE, UMAP_SPECIAL };

#define PERM_R	1
#define PERM_W	2
#define PERM_X	4

struct umap {
	uint64_t start, end, pgoff, inode;
	uint32_t dev_major, dev_minor;
	const char *path;	/* interned; NULL for plain anonymous memory */
	struct elf_dso *dso;
	uint8_t perms;
	uint8_t kind;
	bool dso_tried;
};

struct psym {
	uint64_t start, size;
	const char *name;
};

struct uproc {
	int tgid;
	bool gone;		/* maps never readable: exited or not ours */
	bool exited;		/* maps readable once, no longer */
	bool perfmap_tried;
	signed char root_differs;	/* -1 unknown, else bool */
	uint64_t maps_ns;
	struct umap *maps;
	size_t nmaps;
	struct psym *psyms;
	size_t npsyms;
};

struct usyms {
	struct arena arena;
	struct strset paths;
	struct uproc **procs;	/* open addressing on tgid */
	size_t procs_cap, nprocs;
	struct elf_dso **dsos;	/* chained on (dev, ino) */
	size_t dsos_cap, ndsos;
};

struct sym_cand {
	uint64_t addr, size;
	const char *name;
	uint32_t rank, idx;
};

/* Lower is better when several names share one address. */
static uint32_t sym_rank(unsigned bind, const char *name, uint64_t size)
{
	uint32_t rank = 0, underscores = 0;

	if (bind == STB_LOCAL)
		rank += 1000;
	while (name[underscores] == '_' && underscores < 16)
		underscores++;
	rank += underscores * 10;
	if (bind == STB_WEAK)
		rank += 2;
	if (!size)
		rank += 1;
	return rank;
}

static int sym_cand_cmp(const void *a, const void *b)
{
	const struct sym_cand *x = a, *y = b;

	if (x->addr != y->addr)
		return x->addr < y->addr ? -1 : 1;
	if (x->rank != y->rank)
		return x->rank < y->rank ? -1 : 1;
	return x->idx < y->idx ? -1 : x->idx > y->idx;
}

static Elf_Scn *find_section_type(Elf *elf, Elf64_Word type, GElf_Shdr *sh)
{
	Elf_Scn *scn = NULL;

	while ((scn = elf_nextscn(elf, scn))) {
		if (gelf_getshdr(scn, sh) && sh->sh_type == type)
			return scn;
	}
	return NULL;
}

/*
 * Fill d->syms from the SHT_SYMTAB or SHT_DYNSYM of elf: defined functions
 * with a non-zero value, sorted, one entry per address.
 */
static size_t dso_load_symtab(struct usyms *us, struct elf_dso *d, Elf *elf,
			      Elf64_Word type)
{
	struct sym_cand *cands = NULL;
	struct elf_sym *syms = NULL;
	Elf_Data *sd, *nd;
	GElf_Shdr sh, strsh;
	Elf_Scn *scn, *strscn;
	size_t n, k = 0, i, out = 0, entsz, maxlen, len;
	const char *nm;
	GElf_Sym sym;
	unsigned st;

	scn = find_section_type(elf, type, &sh);
	if (!scn)
		return 0;
	strscn = elf_getscn(elf, sh.sh_link);
	if (!strscn || !gelf_getshdr(strscn, &strsh) ||
	    strsh.sh_type != SHT_STRTAB)
		return 0;
	sd = elf_getdata(scn, NULL);
	nd = elf_getdata(strscn, NULL);
	if (!sd || !sd->d_buf || !nd || !nd->d_buf || !nd->d_size)
		return 0;
	entsz = gelf_fsize(elf, ELF_T_SYM, 1, EV_CURRENT);
	if (!entsz)
		return 0;
	n = sd->d_size / entsz;
	if (n > INT_MAX)
		n = INT_MAX;
	if (n < 2)
		return 0;

	cands = malloc(n * sizeof(*cands));
	if (!cands)
		return 0;
	for (i = 1; i < n; i++) {
		if (!gelf_getsym(sd, (int)i, &sym))
			break;
		st = GELF_ST_TYPE(sym.st_info);
		if ((st != STT_FUNC && st != STT_GNU_IFUNC) ||
		    sym.st_shndx == SHN_UNDEF || !sym.st_value ||
		    sym.st_name >= nd->d_size)
			continue;
		nm = (const char *)nd->d_buf + sym.st_name;
		maxlen = nd->d_size - sym.st_name;
		len = strnlen(nm, maxlen);
		if (!len || len == maxlen)
			continue;
		cands[k].addr = sym.st_value;
		cands[k].size = sym.st_size;
		cands[k].name = nm;
		cands[k].rank = sym_rank(GELF_ST_BIND(sym.st_info), nm,
					 sym.st_size);
		cands[k].idx = (uint32_t)i;
		k++;
	}
	if (!k)
		goto out;
	qsort(cands, k, sizeof(*cands), sym_cand_cmp);

	syms = malloc(k * sizeof(*syms));
	if (!syms)
		goto out;
	for (i = 0; i < k; i++) {
		if (out && syms[out - 1].addr == cands[i].addr) {
			/* alias of the preferred name; keep the widest size */
			if (cands[i].size > syms[out - 1].size)
				syms[out - 1].size = cands[i].size;
			continue;
		}
		syms[out].name = arena_strndup(&us->arena, cands[i].name,
					       strlen(cands[i].name));
		if (!syms[out].name)
			break;
		syms[out].addr = cands[i].addr;
		syms[out].size = cands[i].size;
		out++;
	}
	if (!out) {
		free(syms);
		goto out;
	}
	free(d->syms);
	d->syms = syms;
	d->nsyms = out;
out:
	free(cands);
	return out;
}

static int dso_load_segments(struct elf_dso *d, Elf *elf)
{
	struct elf_seg *segs = NULL, *ns;
	size_t n, i, cnt = 0, cap = 0;
	GElf_Phdr ph;

	if (elf_getphdrnum(elf, &n))
		return -1;
	for (i = 0; i < n; i++) {
		if (!gelf_getphdr(elf, (int)i, &ph))
			break;
		if (ph.p_type != PT_LOAD || !ph.p_filesz ||
		    ph.p_offset + ph.p_filesz < ph.p_offset)
			continue;
		if (cnt == cap) {
			cap = cap ? cap * 2 : 4;
			ns = realloc(segs, cap * sizeof(*segs));
			if (!ns) {
				free(segs);
				return -1;
			}
			segs = ns;
		}
		segs[cnt].offset = ph.p_offset;
		segs[cnt].vaddr = ph.p_vaddr;
		segs[cnt].filesz = ph.p_filesz;
		cnt++;
	}
	d->segs = segs;
	d->nsegs = cnt;
	return 0;
}

static size_t notes_build_id(Elf_Data *data, unsigned char *out)
{
	size_t off = 0, next, name_off, desc_off;
	GElf_Nhdr nh;

	if (!data || !data->d_buf)
		return 0;
	while (off < data->d_size &&
	       (next = gelf_getnote(data, off, &nh, &name_off, &desc_off))) {
		if (nh.n_type == NT_GNU_BUILD_ID && nh.n_namesz == 4 &&
		    !memcmp((char *)data->d_buf + name_off, "GNU", 4) &&
		    nh.n_descsz >= 2 && nh.n_descsz <= BUILD_ID_MAX) {
			memcpy(out, (char *)data->d_buf + desc_off,
			       nh.n_descsz);
			return nh.n_descsz;
		}
		off = next;
	}
	return 0;
}

/* NT_GNU_BUILD_ID from note sections, else from PT_NOTE segments. */
static size_t elf_build_id(Elf *elf, unsigned char *out)
{
	Elf_Scn *scn = NULL;
	GElf_Shdr sh;
	GElf_Phdr ph;
	size_t len, n, i;

	while ((scn = elf_nextscn(elf, scn))) {
		if (!gelf_getshdr(scn, &sh) || sh.sh_type != SHT_NOTE)
			continue;
		len = notes_build_id(elf_getdata(scn, NULL), out);
		if (len)
			return len;
	}
	if (elf_getphdrnum(elf, &n))
		return 0;
	for (i = 0; i < n; i++) {
		if (!gelf_getphdr(elf, (int)i, &ph))
			break;
		if (ph.p_type != PT_NOTE || !ph.p_filesz)
			continue;
		len = notes_build_id(elf_getdata_rawchunk(elf,
					(int64_t)ph.p_offset, ph.p_filesz,
					ph.p_align == 8 ? ELF_T_NHDR8 :
							  ELF_T_NHDR), out);
		if (len)
			return len;
	}
	return 0;
}

/* .gnu_debuglink: file name (no directories), padding, CRC32. */
static bool elf_debuglink(Elf *elf, char *name, size_t cap, uint32_t *crc,
			  bool *has_crc)
{
	Elf_Scn *scn = NULL;
	GElf_Ehdr eh;
	GElf_Shdr sh;
	Elf_Data *data;
	const char *s, *sname;
	const unsigned char *b;
	size_t shstrndx, len, crc_off;

	*has_crc = false;
	if (!gelf_getehdr(elf, &eh) || elf_getshdrstrndx(elf, &shstrndx))
		return false;
	while ((scn = elf_nextscn(elf, scn))) {
		if (!gelf_getshdr(scn, &sh) || sh.sh_type == SHT_NOBITS)
			continue;
		sname = elf_strptr(elf, shstrndx, sh.sh_name);
		if (sname && !strcmp(sname, ".gnu_debuglink"))
			break;
	}
	if (!scn)
		return false;
	data = elf_getdata(scn, NULL);
	if (!data || !data->d_buf || !data->d_size)
		return false;
	s = data->d_buf;
	len = strnlen(s, data->d_size);
	if (!len || len == data->d_size || len >= cap || memchr(s, '/', len) ||
	    !strcmp(s, ".") || !strcmp(s, ".."))
		return false;
	memcpy(name, s, len + 1);
	crc_off = (len + 4) & ~(size_t)3;
	if (data->d_size >= 4 && crc_off <= data->d_size - 4) {
		b = (const unsigned char *)s + crc_off;
		if (eh.e_ident[EI_DATA] == ELFDATA2MSB)
			*crc = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 |
			       (uint32_t)b[2] << 8 | b[3];
		else
			*crc = (uint32_t)b[3] << 24 | (uint32_t)b[2] << 16 |
			       (uint32_t)b[1] << 8 | b[0];
		*has_crc = true;
	}
	return true;
}

static bool file_crc32(int fd, uint32_t *out)
{
	enum { CHUNK = 1 << 20 };
	unsigned char *buf = malloc(CHUNK);
	unsigned long crc = crc32(0L, Z_NULL, 0);
	off_t off = 0;
	ssize_t r;

	if (!buf)
		return false;
	for (;;) {
		r = pread(fd, buf, CHUNK, off);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			free(buf);
			return false;
		}
		if (r == 0)
			break;
		crc = crc32(crc, buf, (uInt)r);
		off += r;
	}
	free(buf);
	*out = (uint32_t)crc;
	return true;
}

struct debug_want {
	const struct stat *main_st;
	unsigned char build_id[BUILD_ID_MAX];
	size_t build_id_len;
	uint32_t crc;
	bool has_crc;
	unsigned char elf_class;
	uint16_t machine;
};

static Elf *try_debug_file(const char *path, const struct debug_want *w,
			   int *out_fd)
{
	unsigned char bid[BUILD_ID_MAX];
	struct stat st;
	Elf *elf = NULL;
	GElf_Ehdr eh;
	GElf_Shdr sh;
	uint32_t crc;
	size_t blen;
	int fd;

	fd = open_regular(path, 0, &st);
	if (fd < 0)
		return NULL;
	if (st.st_dev == w->main_st->st_dev && st.st_ino == w->main_st->st_ino)
		goto fail;
	elf = elf_begin(fd, ELF_C_READ, NULL);
	if (!elf || elf_kind(elf) != ELF_K_ELF || !gelf_getehdr(elf, &eh) ||
	    eh.e_ident[EI_CLASS] != w->elf_class || eh.e_machine != w->machine ||
	    !find_section_type(elf, SHT_SYMTAB, &sh))
		goto fail;

	blen = elf_build_id(elf, bid);
	if (w->build_id_len && blen) {
		if (blen != w->build_id_len || memcmp(bid, w->build_id, blen))
			goto fail;
	} else if (w->has_crc) {
		if (!file_crc32(fd, &crc) || crc != w->crc)
			goto fail;
	}
	*out_fd = fd;
	return elf;
fail:
	if (elf)
		elf_end(elf);
	close(fd);
	return NULL;
}

/* Is /proc/<tgid>/root somewhere other than our own "/"? */
static bool proc_root_differs(struct uproc *p)
{
	struct stat a, b;
	char path[64];

	if (p->root_differs < 0) {
		snprintf(path, sizeof(path), "/proc/%d/root", p->tgid);
		p->root_differs = !stat(path, &a) && !stat("/", &b) &&
				  (a.st_dev != b.st_dev || a.st_ino != b.st_ino);
	}
	return p->root_differs;
}

/*
 * Locate separate debuginfo for a file without .symtab: first by build-id
 * under /usr/lib/debug/.build-id, then by .gnu_debuglink next to the file,
 * in its .debug/ directory, or under /usr/lib/debug. Paths are tried inside
 * the process's root first when that differs from ours.
 */
static Elf *find_debuginfo(struct uproc *p, const struct umap *m, Elf *elf,
			   const struct stat *main_st, int *out_fd)
{
	struct debug_want w = { .main_st = main_st };
	char path[PATH_MAX], rootpfx[64], link[256];
	char hex[2 * BUILD_ID_MAX + 1];
	const char *pfx[2], *slash;
	int npfx = 0, i, n, dirlen;
	bool has_link;
	GElf_Ehdr eh;
	size_t j;
	Elf *dbg;

	if (!gelf_getehdr(elf, &eh))
		return NULL;
	w.elf_class = eh.e_ident[EI_CLASS];
	w.machine = eh.e_machine;
	w.build_id_len = elf_build_id(elf, w.build_id);
	has_link = elf_debuglink(elf, link, sizeof(link), &w.crc, &w.has_crc);

	if (proc_root_differs(p)) {
		snprintf(rootpfx, sizeof(rootpfx), "/proc/%d/root", p->tgid);
		pfx[npfx++] = rootpfx;
	}
	pfx[npfx++] = "";

	if (w.build_id_len) {
		for (j = 0; j < w.build_id_len; j++)
			snprintf(hex + 2 * j, 3, "%02x", w.build_id[j]);
		for (i = 0; i < npfx; i++) {
			n = snprintf(path, sizeof(path),
				     "%s/usr/lib/debug/.build-id/%.2s/%s.debug",
				     pfx[i], hex, hex + 2);
			if (n < 0 || (size_t)n >= sizeof(path))
				continue;
			dbg = try_debug_file(path, &w, out_fd);
			if (dbg)
				return dbg;
		}
	}

	if (!has_link || !m->path || m->path[0] != '/')
		return NULL;
	slash = strrchr(m->path, '/');
	dirlen = (int)(slash - m->path);	/* "" for files in "/" */
	for (i = 0; i < npfx; i++) {
		for (j = 0; j < 3; j++) {
			if (j == 0)
				n = snprintf(path, sizeof(path),
					     "%s/usr/lib/debug%.*s/%s", pfx[i],
					     dirlen, m->path, link);
			else if (j == 1)
				n = snprintf(path, sizeof(path),
					     "%s%.*s/.debug/%s", pfx[i],
					     dirlen, m->path, link);
			else
				n = snprintf(path, sizeof(path), "%s%.*s/%s",
					     pfx[i], dirlen, m->path, link);
			if (n < 0 || (size_t)n >= sizeof(path))
				continue;
			/* try_debug_file() skips the main file itself */
			dbg = try_debug_file(path, &w, out_fd);
			if (dbg)
				return dbg;
		}
	}
	return NULL;
}

static struct elf_dso *dso_find(struct usyms *us, const struct stat *st)
{
	size_t b = mix64((uint64_t)st->st_dev * 0x9e3779b97f4a7c15ULL ^
			 (uint64_t)st->st_ino) & (us->dsos_cap - 1);
	struct elf_dso *d;

	for (d = us->dsos[b]; d; d = d->next) {
		/* size and mtime guard against a recycled inode number */
		if (d->dev == st->st_dev && d->ino == st->st_ino &&
		    d->size == st->st_size &&
		    d->mtime.tv_sec == st->st_mtim.tv_sec &&
		    d->mtime.tv_nsec == st->st_mtim.tv_nsec)
			return d;
	}
	return NULL;
}

static void dso_insert(struct usyms *us, struct elf_dso *d)
{
	struct elf_dso **nb, *e, *next;
	size_t ncap, i, b;

	if (us->ndsos + 1 > us->dsos_cap) {
		ncap = us->dsos_cap * 2;
		nb = calloc(ncap, sizeof(*nb));
		if (nb) {
			for (i = 0; i < us->dsos_cap; i++) {
				for (e = us->dsos[i]; e; e = next) {
					next = e->next;
					b = mix64((uint64_t)e->dev *
						  0x9e3779b97f4a7c15ULL ^
						  (uint64_t)e->ino) & (ncap - 1);
					e->next = nb[b];
					nb[b] = e;
				}
			}
			free(us->dsos);
			us->dsos = nb;
			us->dsos_cap = ncap;
		}
	}
	b = mix64((uint64_t)d->dev * 0x9e3779b97f4a7c15ULL ^ (uint64_t)d->ino) &
	    (us->dsos_cap - 1);
	d->next = us->dsos[b];
	us->dsos[b] = d;
	us->ndsos++;
}

static struct elf_dso *dso_load(struct usyms *us, struct uproc *p,
				const struct umap *m, int fd,
				const struct stat *st)
{
	struct elf_dso *d = calloc(1, sizeof(*d));
	Elf *elf, *dbg;
	int dfd = -1;

	if (!d)
		return NULL;
	d->dev = st->st_dev;
	d->ino = st->st_ino;
	d->size = st->st_size;
	d->mtime = st->st_mtim;

	elf = elf_begin(fd, ELF_C_READ, NULL);
	if (elf && elf_kind(elf) == ELF_K_ELF &&
	    !dso_load_segments(d, elf) && d->nsegs) {
		if (!dso_load_symtab(us, d, elf, SHT_SYMTAB)) {
			dbg = find_debuginfo(p, m, elf, st, &dfd);
			if (dbg) {
				dso_load_symtab(us, d, dbg, SHT_SYMTAB);
				elf_end(dbg);
				close(dfd);
			}
			if (!d->nsyms)
				dso_load_symtab(us, d, elf, SHT_DYNSYM);
		}
	}
	if (elf)
		elf_end(elf);
	dso_insert(us, d);	/* failures are cached as empty entries */
	return d;
}

static bool has_deleted_suffix(const char *path, size_t len)
{
	return len > DELETED_LEN &&
	       !memcmp(path + len - DELETED_LEN, DELETED_SUFFIX, DELETED_LEN);
}

/*
 * Open the file behind a mapping. map_files/ reaches deleted files and other
 * mount namespaces but needs CAP_SYS_ADMIN (or CAP_CHECKPOINT_RESTORE);
 * otherwise go by name, inside the process's root first. A name-based open
 * that obviously is a different file (same device, other inode, or a
 * replacement for a deleted one) is rejected.
 */
static int open_map_file(const struct uproc *p, const struct umap *m,
			 struct stat *st)
{
	size_t len = strlen(m->path), plen;
	bool deleted = has_deleted_suffix(m->path, len);
	char path[PATH_MAX + 64];
	int fd, variant, root, n;
	bool same_dev;

	snprintf(path, sizeof(path), "/proc/%d/map_files/%" PRIx64 "-%" PRIx64,
		 p->tgid, m->start, m->end);
	fd = open_regular(path, 0, st);
	if (fd >= 0)
		return fd;

	for (variant = 0; variant < (deleted ? 2 : 1); variant++) {
		plen = deleted && variant == 0 ? len - DELETED_LEN : len;
		if (plen > INT_MAX)
			return -1;
		for (root = 1; root >= 0; root--) {
			if (root)
				n = snprintf(path, sizeof(path),
					     "/proc/%d/root%.*s", p->tgid,
					     (int)plen, m->path);
			else
				n = snprintf(path, sizeof(path), "%.*s",
					     (int)plen, m->path);
			if (n < 0 || (size_t)n >= sizeof(path))
				continue;
			fd = open_regular(path, 0, st);
			if (fd < 0)
				continue;
			same_dev = major(st->st_dev) == m->dev_major &&
				   minor(st->st_dev) == m->dev_minor;
			if ((uint64_t)st->st_ino != m->inode &&
			    (deleted || same_dev)) {
				close(fd);
				continue;
			}
			return fd;
		}
	}
	return -1;
}

static struct elf_dso *map_dso(struct usyms *us, struct uproc *p,
			       struct umap *m)
{
	struct elf_dso *d;
	struct stat st;
	int fd;

	if (m->dso_tried)
		return m->dso;
	m->dso_tried = true;
	/* never open device nodes that happen to be mapped executable */
	if (!strncmp(m->path, "/dev/", 5) && strncmp(m->path, "/dev/shm/", 9))
		return NULL;
	fd = open_map_file(p, m, &st);
	if (fd < 0)
		return NULL;
	d = dso_find(us, &st);
	if (!d)
		d = dso_load(us, p, m, fd, &st);
	close(fd);
	m->dso = d;
	return d;
}

static bool dso_file_to_vaddr(const struct elf_dso *d, uint64_t file_off,
			      uint64_t *vaddr)
{
	size_t i;

	for (i = 0; i < d->nsegs; i++) {
		const struct elf_seg *s = &d->segs[i];

		if (file_off >= s->offset && file_off - s->offset < s->filesz) {
			*vaddr = file_off - s->offset + s->vaddr;
			return true;
		}
	}
	return false;
}

static const struct elf_sym *dso_find_sym(const struct elf_dso *d,
					  uint64_t vaddr)
{
	const struct elf_sym *s;
	size_t lo = 0, hi = d->nsyms, mid, i, j;

	while (lo < hi) {
		mid = lo + (hi - lo) / 2;
		if (d->syms[mid].addr <= vaddr)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (!lo)
		return NULL;
	i = lo - 1;
	s = &d->syms[i];
	if (!s->size || vaddr - s->addr < s->size)
		return s;
	/* past the end of the nearest symbol: maybe nested in a larger one */
	for (j = i; j-- > 0 && i - j <= ENCLOSE_SCAN;) {
		s = &d->syms[j];
		if (s->size && vaddr - s->addr < s->size)
			return s;
	}
	return NULL;
}

/* ---- user: perf map files (JIT) ---------------------------------------- */

struct pcand {
	uint64_t start, size;
	const char *name;
	size_t idx;
};

static int pcand_cmp(const void *a, const void *b)
{
	const struct pcand *x = a, *y = b;

	if (x->start != y->start)
		return x->start < y->start ? -1 : 1;
	/* newest entry for a reused address first */
	return x->idx > y->idx ? -1 : x->idx < y->idx;
}

static const char *skip_ws(const char *p, const char *end)
{
	while (p < end && (*p == ' ' || *p == '\t'))
		p++;
	return p;
}

static const char *parse_hex_0x(const char *p, const char *end, uint64_t *v)
{
	if (end - p > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
	    hexval((unsigned char)p[2]) >= 0)
		p += 2;
	return parse_hex(p, end, v);
}

static int proc_owner(int tgid, uid_t *uid)
{
	char path[64];
	struct stat st;

	snprintf(path, sizeof(path), "/proc/%d", tgid);
	if (stat(path, &st))
		return -1;
	*uid = st.st_uid;
	return 0;
}

static bool perfmap_parse(struct usyms *us, struct uproc *p, const char *path)
{
	struct pcand *cands = NULL, *nc;
	struct psym *syms = NULL;
	size_t len, n = 0, cap = 0, i, out = 0, idx = 0;
	const char *line, *end, *le, *q, *ne;
	uint64_t start, size;
	char *buf = NULL;
	struct stat st;
	uid_t owner;
	bool ok = false;
	int fd;

	fd = open_regular(path, O_NOFOLLOW, &st);
	if (fd < 0)
		return false;
	/* /tmp is shared: only trust files from root, us, or the process owner */
	if (st.st_uid != 0 && st.st_uid != geteuid() &&
	    (proc_owner(p->tgid, &owner) || st.st_uid != owner)) {
		close(fd);
		return false;
	}
	if (read_fd_all(fd, PERFMAP_READ_LIMIT, &buf, &len)) {
		close(fd);
		return false;
	}
	close(fd);

	end = buf + len;
	for (line = buf; line < end; line = le + 1) {
		le = memchr(line, '\n', (size_t)(end - line));
		if (!le)
			le = end;
		idx++;
		q = skip_ws(line, le);
		q = parse_hex_0x(q, le, &start);
		if (!q || q == le || (*q != ' ' && *q != '\t'))
			continue;
		q = parse_hex_0x(skip_ws(q, le), le, &size);
		if (!q || q == le || (*q != ' ' && *q != '\t'))
			continue;
		q = skip_ws(q, le);
		for (ne = le; ne > q && (unsigned char)ne[-1] <= ' '; ne--)
			;
		if (ne == q || memchr(q, '\0', (size_t)(ne - q)))
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 256;
			nc = realloc(cands, cap * sizeof(*cands));
			if (!nc)
				goto out;
			cands = nc;
		}
		cands[n].start = start;
		cands[n].size = size;
		cands[n].name = q;
		cands[n].idx = idx;
		buf[ne - buf] = '\0';	/* le/ne point into buf */
		n++;
	}
	if (!n)
		goto out;
	qsort(cands, n, sizeof(*cands), pcand_cmp);
	syms = malloc(n * sizeof(*syms));
	if (!syms)
		goto out;
	for (i = 0; i < n; i++) {
		if (out && syms[out - 1].start == cands[i].start)
			continue;
		syms[out].name = arena_strndup(&us->arena, cands[i].name,
					       strlen(cands[i].name));
		if (!syms[out].name)
			break;
		syms[out].start = cands[i].start;
		syms[out].size = cands[i].size;
		out++;
	}
	if (out) {
		p->psyms = syms;
		p->npsyms = out;
		syms = NULL;
		ok = true;
	}
out:
	free(syms);
	free(cands);
	free(buf);
	return ok;
}

/* Innermost pid of tgid (the name a containerized JIT uses), or tgid. */
static int proc_nspid(int tgid)
{
	char path[64], *buf, *s, *e;
	size_t len;
	long v = tgid;
	int fd;

	snprintf(path, sizeof(path), "/proc/%d/status", tgid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return tgid;
	if (read_fd_all(fd, 1U << 20, &buf, &len)) {
		close(fd);
		return tgid;
	}
	close(fd);
	s = strstr(buf, "\nNSpid:");
	if (s) {
		s += 7;
		e = strchr(s, '\n');
		if (e)
			*e = '\0';
		while ((s = strpbrk(s, "0123456789"))) {
			v = strtol(s, &s, 10);
		}
	}
	free(buf);
	return v > 0 && v <= INT_MAX ? (int)v : tgid;
}

static const struct psym *perfmap_find(struct usyms *us, struct uproc *p,
				       uint64_t addr)
{
	const struct psym *s;
	size_t lo, hi, mid, i, j;
	char path[96];
	int nspid;

	if (!p->perfmap_tried) {
		p->perfmap_tried = true;
		snprintf(path, sizeof(path), "/tmp/perf-%d.map", p->tgid);
		if (!perfmap_parse(us, p, path)) {
			snprintf(path, sizeof(path),
				 "/proc/%d/root/tmp/perf-%d.map", p->tgid,
				 p->tgid);
			if (!perfmap_parse(us, p, path)) {
				nspid = proc_nspid(p->tgid);
				snprintf(path, sizeof(path),
					 "/proc/%d/root/tmp/perf-%d.map",
					 p->tgid, nspid);
				if (nspid != p->tgid)
					perfmap_parse(us, p, path);
			}
		}
	}
	lo = 0;
	hi = p->npsyms;
	while (lo < hi) {
		mid = lo + (hi - lo) / 2;
		if (p->psyms[mid].start <= addr)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (!lo)
		return NULL;
	for (i = lo - 1, j = 0; j <= ENCLOSE_SCAN; j++, i--) {
		s = &p->psyms[i];
		if (addr - s->start < (s->size ? s->size : 1))
			return s;
		if (!i)
			break;
	}
	return NULL;
}

/* ---- user: process maps ------------------------------------------------ */

/* "start-end perms offset major:minor inode   path" */
static int parse_maps_line(struct usyms *us, const char *p, const char *end,
			   struct umap *m)
{
	uint64_t major, minor;
	size_t plen;

	memset(m, 0, sizeof(*m));
	if (!(p = parse_hex(p, end, &m->start)) || p == end || *p++ != '-')
		return 1;
	if (!(p = parse_hex(p, end, &m->end)) || p == end || *p++ != ' ')
		return 1;
	if (end - p < 5 || p[4] != ' ')
		return 1;
	m->perms = (p[0] == 'r' ? PERM_R : 0) | (p[1] == 'w' ? PERM_W : 0) |
		   (p[2] == 'x' ? PERM_X : 0);
	p += 5;
	if (!(p = parse_hex(p, end, &m->pgoff)) || p == end || *p++ != ' ')
		return 1;
	if (!(p = parse_hex(p, end, &major)) || p == end || *p++ != ':')
		return 1;
	if (!(p = parse_hex(p, end, &minor)) || p == end || *p++ != ' ')
		return 1;
	if (!(p = parse_dec(p, end, &m->inode)))
		return 1;
	if (m->end <= m->start)
		return 1;
	m->dev_major = (uint32_t)major;
	m->dev_minor = (uint32_t)minor;
	p = skip_ws(p, end);
	plen = (size_t)(end - p);
	if (!plen) {
		m->kind = UMAP_ANON;
		return 0;
	}
	m->path = strset_intern(&us->paths, &us->arena, p, plen);
	if (!m->path)
		return -ENOMEM;
	if (m->path[0] == '/')
		m->kind = UMAP_FILE;
	else if (!strncmp(m->path, "[anon:", 6) ||
		 !strncmp(m->path, "[anon_shmem:", 12))
		m->kind = UMAP_ANON;
	else
		m->kind = UMAP_SPECIAL;
	return 0;
}

static int umap_cmp(const void *a, const void *b)
{
	const struct umap *x = a, *y = b;

	return x->start < y->start ? -1 : x->start > y->start;
}

static int proc_load_maps(struct usyms *us, struct uproc *p)
{
	struct umap *maps = NULL, *nm, m;
	size_t len, n = 0, cap = 0;
	const char *line, *end, *le;
	bool sorted = true;
	char path[64], *buf;
	int fd, err;

	snprintf(path, sizeof(path), "/proc/%d/maps", p->tgid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	err = read_fd_all(fd, MAPS_READ_LIMIT, &buf, &len);
	close(fd);
	if (err)
		return err;

	end = buf + len;
	for (line = buf; line < end; line = le + 1) {
		le = memchr(line, '\n', (size_t)(end - line));
		if (!le)
			le = end;
		err = parse_maps_line(us, line, le, &m);
		if (err > 0)
			continue;
		if (err < 0)
			goto fail;
		if (n == cap) {
			cap = cap ? cap * 2 : 64;
			nm = realloc(maps, cap * sizeof(*maps));
			if (!nm) {
				err = -ENOMEM;
				goto fail;
			}
			maps = nm;
		}
		if (n && maps[n - 1].start > m.start)
			sorted = false;
		maps[n++] = m;
	}
	free(buf);
	if (!sorted)
		qsort(maps, n, sizeof(*maps), umap_cmp);
	free(p->maps);
	p->maps = maps;
	p->nmaps = n;
	p->maps_ns = now_ns();
	return 0;
fail:
	free(maps);
	free(buf);
	return err;
}

static struct umap *proc_find_map(struct uproc *p, uint64_t addr)
{
	size_t lo = 0, hi = p->nmaps, mid;
	struct umap *m;

	while (lo < hi) {
		mid = lo + (hi - lo) / 2;
		if (p->maps[mid].start <= addr)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (!lo)
		return NULL;
	m = &p->maps[lo - 1];
	return addr < m->end ? m : NULL;
}

/* After a miss, re-read maps (at most once a second) to catch dlopen(). */
static bool proc_refresh(struct usyms *us, struct uproc *p)
{
	uint64_t now;
	int err;

	if (p->exited)
		return false;
	now = now_ns();
	if (now - p->maps_ns < MAPS_REFRESH_NS)
		return false;
	err = proc_load_maps(us, p);
	if (err) {
		if (err != -ENOMEM)
			p->exited = true;
		p->maps_ns = now;
		return false;
	}
	return true;
}

static size_t proc_slot(const struct usyms *us, int tgid)
{
	size_t mask = us->procs_cap - 1;
	size_t i = mix64((uint64_t)(unsigned)tgid) & mask;

	while (us->procs[i] && us->procs[i]->tgid != tgid)
		i = (i + 1) & mask;
	return i;
}

static void proc_free(struct uproc *p)
{
	if (!p)
		return;
	free(p->maps);
	free(p->psyms);
	free(p);
}

static struct uproc *proc_get(struct usyms *us, int tgid)
{
	struct uproc *p, **np;
	size_t i, ncap, j, k;

	i = proc_slot(us, tgid);
	if (us->procs[i])
		return us->procs[i];

	if ((us->nprocs + 1) * 2 > us->procs_cap) {
		ncap = us->procs_cap * 2;
		np = calloc(ncap, sizeof(*np));
		if (!np)
			return NULL;
		for (j = 0; j < us->procs_cap; j++) {
			if (!us->procs[j])
				continue;
			k = mix64((uint64_t)(unsigned)us->procs[j]->tgid) &
			    (ncap - 1);
			while (np[k])
				k = (k + 1) & (ncap - 1);
			np[k] = us->procs[j];
		}
		free(us->procs);
		us->procs = np;
		us->procs_cap = ncap;
		i = proc_slot(us, tgid);
	}

	p = calloc(1, sizeof(*p));
	if (!p)
		return NULL;
	p->tgid = tgid;
	p->root_differs = -1;
	if (proc_load_maps(us, p)) {
		/* exited, a kernel-only pid, or not ours to read */
		p->gone = true;
		p->exited = true;
	}
	us->procs[i] = p;
	us->nprocs++;
	return p;
}

/* ---- user: public API -------------------------------------------------- */

struct usyms *usyms_new(void)
{
	struct usyms *us;

	if (elf_version(EV_CURRENT) == EV_NONE)
		return NULL;
	us = calloc(1, sizeof(*us));
	if (!us)
		return NULL;
	us->procs_cap = 64;
	us->procs = calloc(us->procs_cap, sizeof(*us->procs));
	us->dsos_cap = 64;
	us->dsos = calloc(us->dsos_cap, sizeof(*us->dsos));
	if (!us->procs || !us->dsos) {
		free(us->procs);
		free(us->dsos);
		free(us);
		return NULL;
	}
	return us;
}

void usyms_free(struct usyms *us)
{
	struct elf_dso *d, *next;
	size_t i;

	if (!us)
		return;
	for (i = 0; i < us->procs_cap; i++)
		proc_free(us->procs[i]);
	for (i = 0; i < us->dsos_cap; i++) {
		for (d = us->dsos[i]; d; d = next) {
			next = d->next;
			free(d->segs);
			free(d->syms);
			free(d);
		}
	}
	free(us->procs);
	free(us->dsos);
	free(us->paths.slots);
	arena_free(&us->arena);
	free(us);
}

const char *usyms_lookup(struct usyms *us, int tgid, uint64_t addr,
			 uint64_t *offset, const char **dso)
{
	const struct elf_sym *sym;
	const struct psym *ps;
	struct elf_dso *d;
	const char *dso_dummy;
	uint64_t off_dummy, vaddr;
	struct uproc *p;
	struct umap *m;

	if (!offset)
		offset = &off_dummy;
	if (!dso)
		dso = &dso_dummy;
	*offset = 0;
	*dso = NULL;
	if (!us || tgid <= 0)
		return NULL;

	p = proc_get(us, tgid);
	if (!p || p->gone)
		return NULL;
	m = proc_find_map(p, addr);
	if (!m && proc_refresh(us, p))
		m = proc_find_map(p, addr);
	if (!m)
		return NULL;

	*dso = m->path ? m->path : "[anon]";
	*offset = addr - m->start + m->pgoff;
	if (!(m->perms & PERM_X) || m->kind == UMAP_SPECIAL)
		return NULL;

	if (m->kind == UMAP_FILE) {
		d = map_dso(us, p, m);
		if (d && d->nsyms) {
			if (!dso_file_to_vaddr(d, *offset, &vaddr))
				return NULL;
			sym = dso_find_sym(d, vaddr);
			if (!sym)
				return NULL;
			*offset = vaddr - sym->addr;
			return sym->name;
		}
		/* no ELF symbols (e.g. a memfd holding JIT code): try perf map */
	}

	ps = perfmap_find(us, p, addr);
	if (!ps)
		return NULL;
	*offset = addr - ps->start;
	return ps->name;
}
