/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * See fork-oracle.c. A reference copy of the guest produced by fork(), which
 * no code in this tree participates in making faithful -- that is the point.
 *
 * All of these need the BQL held with the vCPUs stopped.
 */
#ifndef FASTSNAP_FORK_ORACLE_H
#define FASTSNAP_FORK_ORACLE_H

#include "qemu/osdep.h"

/* Fork a parked reference child. Refuses if any RAM block is shared, because
 * copy-on-write is what makes the reference a reference. 0 on success. */
int fastsnap_fork_ref_take(void);

/* Compare live guest RAM against the reference. Returns differing page count,
 * or -1 if the reference could not be read -- never conflated with zero. */
int64_t fastsnap_fork_ref_diff(void);

/* SIGKILL and reap. A parked reference pins a full CoW copy of guest RAM. */
void fastsnap_fork_ref_drop(void);

uint64_t fastsnap_fork_diff_pages(void);
uint64_t fastsnap_fork_bytes_checked(void);
const char *fastsnap_fork_report(void);

/* How the last comparison reached its answer. `proved` pages were shown equal
 * by PFN identity (a kernel guarantee) and never read; `read` pages went
 * through process_vm_readv and memcmp. They sum to the pages covered. A caller
 * reporting a clean oracle should report these too: "281 MB identical" means
 * something different when 99.97% of it was proven rather than compared, and a
 * reader is entitled to know which. */
uint64_t fastsnap_fork_pages_proved(void);
uint64_t fastsnap_fork_pages_read(void);

#define FASTSNAP_PAGEMAP_OFF          0   /* disabled by env */
#define FASTSNAP_PAGEMAP_ACTIVE       1   /* PFNs readable, filter engaged */
#define FASTSNAP_PAGEMAP_UNAVAILABLE (-1) /* no CAP_SYS_ADMIN, or no pagemap */
int fastsnap_fork_pagemap_status(void);

#endif /* FASTSNAP_FORK_ORACLE_H */
