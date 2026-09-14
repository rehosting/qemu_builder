/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * See ram-snapshot.c. The RAM half of the reset; pairs with the device block.
 * All of these need the BQL held with the vCPUs stopped.
 */
#ifndef FASTSNAP_RAM_SNAPSHOT_H
#define FASTSNAP_RAM_SNAPSHOT_H

#include "qemu/osdep.h"

/* Copy every RAM block AND arm dirty tracking, in one call -- see the comment
 * on the definition for why these cannot be separate ops. 0 on success. */
int fastsnap_ram_snapshot_take(void);

/* Copy back every page dirtied since the snapshot, invalidate translated code
 * for the restored ranges, and re-arm. Returns pages restored, or -1. */
int64_t fastsnap_ram_restore(void);

void fastsnap_ram_snapshot_release(void);
bool fastsnap_ram_snapshot_present(void);
uint64_t fastsnap_ram_snapshot_bytes(void);
uint64_t fastsnap_ram_restored_pages(void);

/*
 * How overbroad the restore is, cumulative since process start.
 *
 * `unchanged` counts pages that were in the dirty set and whose bytes already
 * matched the snapshot -- copied and invalidated for nothing. It is only
 * counted when FASTSNAP_COUNT_UNCHANGED is set, because it costs a memcmp per
 * restored page; it reads 0 otherwise, which is indistinguishable from "none
 * were unchanged", so read it together with the env var you set.
 */
uint64_t fastsnap_ram_pages_unchanged(void);
uint64_t fastsnap_ram_pages_invalidated(void);
uint64_t fastsnap_ram_pages_skipped_nocode(void);

/*
 * TCG counters QEMU already maintains. Cumulative; the caller takes deltas.
 * All read 0 in a build without TCG, and 0 when TCG is not the accelerator.
 */
#define FASTSNAP_TCG_TB_FLUSH            0
#define FASTSNAP_TCG_TB_PHYS_INVALIDATE  1
#define FASTSNAP_TCG_TLB_FULL_FLUSH      2
#define FASTSNAP_TCG_TLB_PART_FLUSH      3
#define FASTSNAP_TCG_TLB_ELIDE_FLUSH     4
uint64_t fastsnap_tcg_stat(int which);

#endif /* FASTSNAP_RAM_SNAPSHOT_H */
