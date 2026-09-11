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

void penguin_fastsnap_set_denylist(const char *csv);
const char *penguin_fastsnap_section_names(void);
void penguin_fastsnap_schedule(int op);
uint64_t penguin_fastsnap_seq(void);
int penguin_fastsnap_last_rc(void);
int64_t penguin_fastsnap_last_us(void);
uint64_t penguin_fastsnap_block_size(void);
int penguin_fastsnap_section_count(void);

#endif /* FASTSNAP_PENGUIN_FASTSNAP_H */
