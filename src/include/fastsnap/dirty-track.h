/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * How many guest pages a stretch of guest execution writes to.
 *
 * This is the number the whole fast-reset design rests on and nobody had
 * measured: a dirty-page reset costs O(dirty pages), and the host-side
 * measurements in penguin/analysis/fastsnap/RESET.md span 79 us at 1 MB to
 * 2,136 us at 16 MB for the same 256 MB guest. Which column applies is not a
 * host property -- it is a property of what the guest writes between two
 * resets, which only the guest can answer.
 *
 * See dirty-track.c for the mechanism and for what it does and does not count.
 */
#ifndef FASTSNAP_DIRTY_TRACK_H
#define FASTSNAP_DIRTY_TRACK_H

#include "qemu/osdep.h"

/* Arm tracking and zero the accumulated set. BQL, vCPUs stopped. */
int fastsnap_dirty_arm(void);

/*
 * Read the set dirtied since the last arm/count, then clear it so the next
 * call reports the next interval. Returns the page count, or -1.
 */
int64_t fastsnap_dirty_count(void);

/* Disarm. Safe to call when not armed. */
void fastsnap_dirty_stop(void);

uint64_t fastsnap_dirty_pages(void);
uint64_t fastsnap_dirty_pages_scanned(void);
uint64_t fastsnap_dirty_page_size(void);
const char *fastsnap_dirty_report(void);
const char *fastsnap_dirty_blocks(void);

#endif /* FASTSNAP_DIRTY_TRACK_H */
