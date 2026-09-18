/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The Penguin-facing C ABI for the device block. See penguin-fastsnap.c for
 * why these exist rather than exporting device_save_all() directly -- briefly:
 * they schedule onto the main loop, and they deliberately do not go through
 * vm_stop(RUN_STATE_RESTORE_VM), which is what makes accel/tcg flush every
 * translation block.
 *
 * Fire-and-forget. Poll penguin_fastsnap_seq() for completion, then read
 * penguin_fastsnap_last_rc() and the accessors.
 */
#ifndef FASTSNAP_PENGUIN_FASTSNAP_H
#define FASTSNAP_PENGUIN_FASTSNAP_H

#include "qemu/osdep.h"

#define PENGUIN_FASTSNAP_TAKE    0
#define PENGUIN_FASTSNAP_RESTORE 1
#define PENGUIN_FASTSNAP_RELEASE 2
#define PENGUIN_FASTSNAP_PROBE   3
/*
 * Restore, then digest the result WITHOUT letting the guest run in between.
 *
 * PROBE alone cannot verify a restore. A caller schedules RESTORE, and the
 * earliest it can schedule PROBE is from a later guest event -- by which time
 * the guest has executed and cpu/timer state has moved again, so the digest
 * can never match the one the block was taken at. The comparison only means
 * anything if both sides are sampled with the vCPUs stopped, which is what
 * this op is for: restore and re-serialise in the same bottom half.
 *
 * TAKE leaves last_digest() as the digest OF THE BLOCK, so the pair is:
 *     take  -> A = digest of what was captured
 *     ...guest runs...
 *     probe -> B, must differ from A or the probe is blind
 *     restore_verify -> C, must equal A
 * last_us() still reports the restore alone; the verifying save happens after
 * the clock is stopped, so it does not inflate the timing.
 */
#define PENGUIN_FASTSNAP_RESTORE_VERIFY 4

void penguin_fastsnap_set_denylist(const char *csv);
const char *penguin_fastsnap_section_names(void);
void penguin_fastsnap_schedule(int op);
uint64_t penguin_fastsnap_seq(void);
int penguin_fastsnap_last_rc(void);
int64_t penguin_fastsnap_last_us(void);
uint64_t penguin_fastsnap_last_digest(void);
uint64_t penguin_fastsnap_block_size(void);
int penguin_fastsnap_section_count(void);

#endif /* FASTSNAP_PENGUIN_FASTSNAP_H */
