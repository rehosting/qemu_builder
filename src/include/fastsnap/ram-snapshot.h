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

#endif /* FASTSNAP_RAM_SNAPSHOT_H */
