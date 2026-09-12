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

/*
 * Digest the WHOLE guest state -- every RAM block, then the device block --
 * with the vCPUs stopped.
 *
 * This exists because neither of the two channels we already have can see a
 * corrupt guest. The device block digest covers devices only, by construction.
 * And crashes.yaml is blind to kernel-side damage: measured on real firmware,
 * a run with 98 lines of kernel panic, swap_dup and OOM-kill in its console
 * produced a crashes.yaml identical in shape to a healthy run, because that
 * plugin hooks userspace fatal signals and a panic is not one. See
 * penguin/analysis/fastsnap/CRASHES.md.
 *
 * So "no crashes recorded" is not evidence the guest is healthy, and this is
 * the signal that does not depend on the guest being well enough to deliver a
 * signal. Two runs that executed the same thing from the same state must agree
 * here; if they do not, one of them is corrupt, whatever crashes.yaml says.
 *
 * last_digest() reports the device half and last_ram_digest() the RAM half,
 * kept separate so a divergence says WHICH half moved. last_us() times both.
 */
#define PENGUIN_FASTSNAP_STATE_DIGEST 5

/*
 * The fork oracle: a reference copy of the whole guest that the fast path
 * cannot have touched, and a page-level diff against it.
 *
 * WHY A FORK AND NOT JUST A DIGEST. A digest taken before and after answers
 * "did the state come back", which the ops above already do. It cannot answer
 * "come back WHERE", and a reset that leaves twelve pages wrong is exactly the
 * failure mode that matters -- it is what a dirty-tracking bug looks like, and
 * a hash tells you only that something moved.
 *
 * FORK_REF forks at the safe point where the vCPUs are already stopped. The
 * child blocks every signal and parks in pause(); it touches nothing, so its
 * copy of guest RAM stays byte-identical to the moment of the fork. It also
 * deliberately never runs the guest, which is what makes this cheap: there are
 * no vCPU threads to recreate in a child that fork() left single-threaded.
 *
 * FORK_DIFF then reads the child's memory back with process_vm_readv() -- fork
 * preserves the address-space layout, so a RAMBlock sits at the same host
 * address in both -- and compares page by page. What comes out is a count and
 * the first few differing guest addresses.
 *
 * THE CORRECTNESS CONDITION, checked rather than assumed: this works only
 * because guest RAM is MAP_PRIVATE, so the child gets copy-on-write. A RAM
 * block created shared (memory-backend-file with share=on, vhost-user) is the
 * SAME memory in both processes, and the "reference" would silently track the
 * parent's live state and report zero differences forever. FORK_REF refuses if
 * any block carries RAM_SHARED rather than producing that.
 *
 * Ordering: FORK_REF at the snapshot point, let the guest run, reset, then
 * FORK_DIFF. Zero differing pages means the reset put every byte back.
 * FORK_DROP reaps the child; a reference left parked holds a full CoW copy of
 * guest RAM, so drop it when done.
 */
#define PENGUIN_FASTSNAP_FORK_REF  6
#define PENGUIN_FASTSNAP_FORK_DIFF 7
#define PENGUIN_FASTSNAP_FORK_DROP 8

/*
 * How many guest pages one stretch of execution writes to.
 *
 * This is the input the reset cost model has been assuming rather than
 * measuring. RESET.md's host-side numbers put a 256 MB guest at 79 us to
 * restore a 1 MB dirty set and 2,136 us for a 16 MB one; the projected
 * ~1,000 exec/s and the claim that 8 instances fit in memory bandwidth both
 * rest on the middle column being right, and nothing had asked the guest.
 *
 * DIRTY_ARM arms QEMU's own migration dirty bitmap and zeroes the accumulated
 * set -- the clear IS the arm, because TCG re-marks a page's TLB entry
 * TLB_NOTDIRTY only once its dirty bits are gone. DIRTY_COUNT reads the set
 * accumulated since the last arm or count and clears it again, so a loop of
 * COUNTs yields one number per interval without re-arming. DIRTY_STOP
 * disarms.
 *
 * Read with penguin_fastsnap_dirty_pages(), which is a count of target pages;
 * penguin_fastsnap_dirty_page_size() gives the multiplier, and
 * penguin_fastsnap_dirty_pages_scanned() says how many pages were examined,
 * so "0 dirty" is distinguishable from "looked at nothing".
 * penguin_fastsnap_dirty_blocks() breaks the count down per RAMBlock, which
 * is what separates a guest working set from a flash write, and
 * penguin_fastsnap_dirty_report() names the first few dirty pages.
 *
 * Measure with no savevm/loadvm in the interval: the migration code walks and
 * clears the same bitmap, and a restore rewrites RAM wholesale.
 */
#define PENGUIN_FASTSNAP_DIRTY_ARM   9
#define PENGUIN_FASTSNAP_DIRTY_COUNT 10
#define PENGUIN_FASTSNAP_DIRTY_STOP  11

/*
 * The RAM half, and with it a complete reset.
 *
 * RAM_SNAPSHOT copies every RAM block and arms dirty tracking in one operation
 * -- separate ops would let the guest run in the gap between them and lose
 * those writes silently. RAM_RESTORE copies back the pages dirtied since,
 * invalidates translated code for exactly those ranges, and re-arms.
 *
 * A complete reset is RESTORE (devices) followed by RAM_RESTORE. Neither half
 * is sound alone: restoring devices without RAM rewinds the CPU's page-table
 * base into RAM that was never rewound, which destroys a real guest within a
 * handful of restores.
 */
#define PENGUIN_FASTSNAP_RAM_SNAPSHOT 12
#define PENGUIN_FASTSNAP_RAM_RESTORE  13
#define PENGUIN_FASTSNAP_RAM_RELEASE  14

/*
 * A complete reset, as two operations.
 *
 * LOOP_ARM takes the device block, snapshots RAM and arms dirty tracking, and
 * forks the reference -- all inside one bottom half. They cannot be separate
 * calls, and the reason is not tidiness. The guest executes between bottom
 * halves, so a fork reference taken even one op after the RAM snapshot holds a
 * DIFFERENT moment than the snapshot does. Every later comparison would then
 * find the pages the guest touched in that gap and report a failing reset
 * forever, for a reset that was correct. Taking them together is what makes
 * "zero differing pages" the right answer rather than an unreachable one.
 *
 * LOOP_RESET is the reset itself: device block, then the dirty RAM pages.
 *
 * The loop is then: LOOP_ARM, run the guest, LOOP_RESET, run the guest again.
 * FORK_DIFF at any point after a LOOP_RESET must report zero differing pages;
 * anything else is the reset failing to put the guest back.
 */
#define PENGUIN_FASTSNAP_LOOP_ARM   15
#define PENGUIN_FASTSNAP_LOOP_RESET 16

/*
 * Reset, then diff against the fork reference WITHOUT letting the guest run
 * in between.
 *
 * The same argument as RESTORE_VERIFY, and it is not a convenience. Each op
 * runs in its own bottom half and the guest executes between bottom halves, so
 * a FORK_DIFF scheduled after a LOOP_RESET reports the pages the guest dirtied
 * in the gap -- on a booted firmware image that is a few hundred pages of
 * ordinary kernel work. A correct reset would therefore be indistinguishable
 * from a broken one on a live target, and the oracle would be usable only on a
 * machine that is not running, which is not the machine whose reset we care
 * about.
 *
 * Doing both in one bottom half makes "zero differing pages" the right answer
 * on a live guest rather than an unreachable one.
 *
 * last_us() reports the RESET alone. The diff reads back the whole of guest
 * RAM through process_vm_readv() and costs tens of milliseconds, so folding it
 * into the timing would make the reset look two orders of magnitude more
 * expensive than it is -- and it is the reset, not the oracle, that a loop
 * pays for every iteration. Read the diff cost from diff_us().
 */
#define PENGUIN_FASTSNAP_LOOP_RESET_VERIFY 17

void penguin_fastsnap_set_denylist(const char *csv);
const char *penguin_fastsnap_section_names(void);
void penguin_fastsnap_schedule(int op);
uint64_t penguin_fastsnap_seq(void);
int penguin_fastsnap_last_rc(void);
int64_t penguin_fastsnap_last_us(void);
uint64_t penguin_fastsnap_last_digest(void);
uint64_t penguin_fastsnap_last_ram_digest(void);
uint64_t penguin_fastsnap_diff_pages(void);
int64_t penguin_fastsnap_diff_us(void);
uint64_t penguin_fastsnap_ram_restored_pages(void);
uint64_t penguin_fastsnap_ram_snapshot_bytes(void);
uint64_t penguin_fastsnap_diff_bytes_checked(void);
const char *penguin_fastsnap_diff_report(void);
uint64_t penguin_fastsnap_dirty_pages(void);
uint64_t penguin_fastsnap_dirty_pages_scanned(void);
uint64_t penguin_fastsnap_dirty_page_size(void);
const char *penguin_fastsnap_dirty_report(void);
const char *penguin_fastsnap_dirty_blocks(void);
uint64_t penguin_fastsnap_block_size(void);
int penguin_fastsnap_section_count(void);

#endif /* FASTSNAP_PENGUIN_FASTSNAP_H */
