/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */
/*
 * Symbol resolution for kernel and user stack addresses.
 */
#ifndef __CG_SYMS_H
#define __CG_SYMS_H

#include <stdint.h>

/* ---- kernel ------------------------------------------------------------ */

struct ksyms;

/*
 * Load /proc/kallsyms. Addresses are only visible to root (or with
 * kptr_restrict=0); returns NULL on failure or if every address reads as 0.
 */
struct ksyms *ksyms_load(void);
void ksyms_free(struct ksyms *ks);

/*
 * Map a kernel address to the symbol containing it. Returns the symbol name
 * (owned by @ks) or NULL. On success *offset is addr - symbol start and
 * *module is the module name (e.g. "nf_tables") or NULL for vmlinux; either
 * out pointer may be NULL.
 */
const char *ksyms_lookup(const struct ksyms *ks, uint64_t addr,
			 uint64_t *offset, const char **module);

/* ---- user -------------------------------------------------------------- */

struct usyms;

struct usyms *usyms_new(void);
void usyms_free(struct usyms *us);

/*
 * Map a user address in process @tgid to a function symbol. Returns the
 * symbol name (owned by @us, valid until usyms_free) or NULL. On return,
 * *dso (if non-NULL) is the path of the mapping containing addr or NULL if
 * none, and *offset (if non-NULL) is addr - symbol start when a symbol was
 * found, else the file offset of addr within *dso.
 *
 * The process's memory map is read on first use of each tgid and cached;
 * a process that has exited resolves to NULL.
 */
const char *usyms_lookup(struct usyms *us, int tgid, uint64_t addr,
			 uint64_t *offset, const char **dso);

#endif /* __CG_SYMS_H */
