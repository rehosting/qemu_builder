/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The RAM half of the reset: put back the pages the guest dirtied.
 *
 * Measured on a real firmware target, one fuzzing iteration dirties ~128 pages
 * (~520 KB) of a 256 MB guest, so copying back the dirty set costs ~54 us --
 * about 9% of a reset whose device half is ~0.4 ms. This exists for
 * correctness, not for speed: a device-only restore rewinds the CPU's
 * page-table base into RAM that was never rewound, and measured on real
 * firmware that destroys the guest within a handful of restores (kernel panic
 * in rcu_process_callbacks, swap_dup errors, OOM kills). See REALFW.md.
 *
 * The reference is an in-process copy, deliberately NOT the fork oracle's
 * child. The oracle has to stay independent of the mechanism it checks: if the
 * restore read its bytes from the fork child and then the verification compared
 * against that same child, a completely broken restore would verify perfectly.
 * That is the shape of failure this lane has produced five times, and it is
 * cheap to design out rather than detect.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/bitmap.h"
#include "system/physmem.h"
#include "system/ram_addr.h"
#include "system/ramblock.h"
#include "system/ramlist.h"
#include "exec/translation-block.h"
#include "system/tcg.h"
#include "exec/target_page.h"
#include "qemu/rcu.h"
#include "qemu/cutils.h"
#include "qemu/host-utils.h"
#include "system/memory.h"
#include "system/physmem.h"

#include "fastsnap/ram-snapshot.h"
#include "fastsnap/dirty-track.h"

typedef struct {
    char idstr[256];
    uint64_t len;
    uint8_t *data;
} FastsnapRamCopy;

static FastsnapRamCopy *ram_copies;
static int ram_ncopies;
static uint64_t ram_snapshot_bytes;
static uint64_t ram_restored_pages;
static uint64_t ram_restored_code_pages;
static int tb_guard = -1;   /* -1 = not yet read from the environment */

uint64_t fastsnap_ram_snapshot_bytes(void) { return ram_snapshot_bytes; }
uint64_t fastsnap_ram_restored_pages(void) { return ram_restored_pages; }

/*
 * Of the pages the last restore put back, how many actually held translated
 * code.
 *
 * Measured, a reset's cost does not end when the reset does: on a bugbench
 * lap the guest takes ~24 us per restored page to get back to the next
 * detector hit, which is two orders of magnitude more than the 4 KB memcpy
 * that is on the reset's own clock. The obvious suspect is the one piece of
 * per-page work whose CONSEQUENCE lands after the vCPUs resume -- killing a
 * page's translated blocks so the guest has to build them again.
 *
 * This counter is what makes that checkable rather than plausible. If a lap
 * restores 25 pages and two of them held code, re-translation cannot be
 * costing 24 us across all 25, and the suspect is wrong.
 */
uint64_t fastsnap_ram_restored_code_pages(void) { return ram_restored_code_pages; }

/*
 * Whether to skip invalidation for pages QEMU already knows hold no code.
 *
 * DIRTY_MEMORY_CODE is set for a page with no translated blocks and cleared by
 * tlb_protect_code() when one is created, so "flag set" means there is nothing
 * to invalidate and the call is a no-op. accel/tcg/cputlb.c makes exactly this
 * test before its own tb_invalidate_phys_range_fast() in the notdirty write
 * path; this restore loop was calling unconditionally, which is a divergence
 * from the upstream idiom at the analogous site rather than a deliberate
 * choice.
 *
 * On by default because it is semantically equivalent. FASTSNAP_TB_GUARD=0
 * restores the unconditional call, which exists so the cost can be attributed
 * by A/B rather than argued: if turning the guard off does not move the
 * post-resume time, the invalidation was never what that time was.
 */
static bool fastsnap_tb_guard(void)
{
    if (tb_guard < 0) {
        const char *e = getenv("FASTSNAP_TB_GUARD");
        tb_guard = (e && *e == '0') ? 0 : 1;
        if (!tb_guard) {
            error_report("fastsnap: FASTSNAP_TB_GUARD=0 -- invalidating every "
                         "restored page whether or not it holds translated "
                         "code. This is a measurement setting.");
        }
    }
    return tb_guard != 0;
}
bool fastsnap_ram_snapshot_present(void) { return ram_ncopies > 0; }

void fastsnap_ram_snapshot_release(void)
{
    int i;

    for (i = 0; i < ram_ncopies; i++) {
        g_free(ram_copies[i].data);
    }
    g_free(ram_copies);
    ram_copies = NULL;
    ram_ncopies = 0;
    ram_snapshot_bytes = 0;
}

/*
 * Copy every RAM block, then arm dirty tracking -- both inside one call.
 *
 * These cannot be two ops. Each op runs in its own bottom half, and the guest
 * executes between bottom halves, so a snapshot taken in one and armed in the
 * next would miss every write in the gap. Those pages would then never be
 * restored, and nothing downstream would report it: the dirty set would be
 * self-consistent, the restore would succeed, and the guest would be silently
 * wrong. Taking and arming together is what makes "dirty since the snapshot"
 * mean what it says.
 */
int fastsnap_ram_snapshot_take(void)
{
    RAMBlock *block;
    int n = 0, i = 0;

    fastsnap_ram_snapshot_release();

    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH(block) {
        if (block->host && block->used_length) {
            n++;
        }
    }
    if (!n) {
        error_report("fastsnap: no RAM blocks to snapshot");
        return -1;
    }

    ram_copies = g_new0(FastsnapRamCopy, n);
    RAMBLOCK_FOREACH(block) {
        if (!block->host || !block->used_length) {
            continue;
        }
        pstrcpy(ram_copies[i].idstr, sizeof(ram_copies[i].idstr),
                block->idstr);
        ram_copies[i].len = block->used_length;
        ram_copies[i].data = g_malloc(block->used_length);
        memcpy(ram_copies[i].data, block->host, block->used_length);
        ram_snapshot_bytes += block->used_length;
        i++;
    }
    ram_ncopies = i;

    /* Arm here, in the same bottom half, for the reason above. Also clears any
     * dirty set left over from before the snapshot, which would otherwise make
     * the first restore copy back pages that were never in scope. */
    if (fastsnap_dirty_arm() != 0) {
        fastsnap_ram_snapshot_release();
        return -1;
    }
    return 0;
}

static const FastsnapRamCopy *ram_copy_for(const char *idstr)
{
    int i;

    for (i = 0; i < ram_ncopies; i++) {
        if (strcmp(ram_copies[i].idstr, idstr) == 0) {
            return &ram_copies[i];
        }
    }
    return NULL;
}


/*
 * Take and clear the dirty bitmap for one block, a word at a time.
 *
 * physical_memory_test_and_clear_dirty() walks the range page by page and does
 * one atomic test-and-clear per page, whether or not anything is set. Measured
 * on a 256 MB guest that is 65,634 atomic read-modify-writes and ~770 us with
 * ZERO pages dirty -- about 45% of a complete reset, and O(total RAM) in a
 * mechanism whose entire premise is being O(dirty).
 *
 * A fuzzing iteration dirties ~128 pages of 65,634, so ~99.8% of the bitmap is
 * zero. Reading a whole word first and touching the atomic only when it is
 * non-zero turns 65,634 atomics into ~1,025 plain loads plus an atomic per
 * non-empty word.
 *
 * This is what migration/ram.c already does in its own sync path; that function
 * is static and works on the migration bitmap, so the loop is reproduced here
 * rather than called. The alignment precondition and the fallback are kept
 * exactly as upstream has them: if the block is not word-aligned the general
 * function is correct and this shortcut is not, so it defers rather than
 * guessing.
 *
 * ON THE TARGET_PAGE_BITS HERE vs qemu_target_page_size() IN THE RESTORE LOOP.
 * They look like two different answers to the same question and they are not,
 * so this note exists to stop the "inconsistency" being fixed into a real one.
 * qemu_target_page_size() is a static inline over TARGET_PAGE_SIZE, which is
 * defined in terms of TARGET_PAGE_BITS in the same header; and these files
 * build in system_ss, which is not COMPILING_PER_TARGET, so both resolve to the
 * runtime target_page.bits rather than to a per-target constant. The bitmap
 * walk and the page loop therefore cannot disagree, on any target, including
 * the ones where the page size varies at runtime.
 */
static uint64_t fastsnap_dirty_take(RAMBlock *block, unsigned long *bmap)
{
    ram_addr_t start = block->offset;
    uint64_t length = block->used_length;
    unsigned long word = BIT_WORD(start >> TARGET_PAGE_BITS);
    unsigned long * const *src;
    unsigned long idx, offset;
    uint64_t num_dirty = 0;
    int k, nr;

    if (((word * BITS_PER_LONG) << TARGET_PAGE_BITS) != start ||
        (length & ((BITS_PER_LONG << TARGET_PAGE_BITS) - 1))) {
        return physical_memory_test_and_clear_dirty(start, length,
                                                    DIRTY_MEMORY_MIGRATION,
                                                    bmap);
    }

    nr = BITS_TO_LONGS(length >> TARGET_PAGE_BITS);
    idx = (word * BITS_PER_LONG) / DIRTY_MEMORY_BLOCK_SIZE;
    offset = BIT_WORD((word * BITS_PER_LONG) % DIRTY_MEMORY_BLOCK_SIZE);

    src = qatomic_rcu_read(&ram_list.dirty_memory[DIRTY_MEMORY_MIGRATION])
              ->blocks;

    for (k = 0; k < nr; k++) {
        if (src[idx][offset]) {
            unsigned long bits = qatomic_xchg(&src[idx][offset], 0);

            bmap[k] = bits;
            num_dirty += ctpopl(bits);
        }
        if (++offset >= BITS_TO_LONGS(DIRTY_MEMORY_BLOCK_SIZE)) {
            offset = 0;
            idx++;
        }
    }

    /* Only when something was actually cleared, matching upstream: this walks
     * every vCPU's TLB to put TLB_NOTDIRTY back, and is what re-arms tracking
     * for the next interval. */
    if (num_dirty) {
        physical_memory_dirty_bits_cleared(start, length);
    }
    memory_region_clear_dirty_bitmap(block->mr, 0, length);
    return num_dirty;
}

/*
 * Restore every page dirtied since the snapshot, and re-arm.
 *
 * WHY THE TB INVALIDATION IS NOT OPTIONAL, and why nothing else in this tree
 * would catch its absence.
 *
 * penguin-fastsnap.c avoids vm_stop(RUN_STATE_RESTORE_VM) precisely so that
 * accel/tcg does not flush every translation block, and its reasoning is that a
 * device-only restore changes no RAM so no translated block can go stale. That
 * reasoning expires exactly here. Consider: the guest writes a code page after
 * the snapshot -- which kills any TB covering it -- then executes it, building
 * fresh TBs from the new bytes. Putting the old bytes back leaves those TBs
 * live and describing content that is no longer there.
 *
 * The fork oracle cannot see this. It compares memory, and memory would be
 * byte-perfect; the damage shows up as the guest executing code that is not in
 * its RAM. So this is reasoned rather than measured, and it is invalidation of
 * the restored range only -- not tb_flush(), whose cost is the 2.3x cliff this
 * whole design exists to avoid.
 */
int64_t fastsnap_ram_restore(void)
{
    RAMBlock *block;
    size_t psize = qemu_target_page_size();
    uint64_t restored = 0, code_pages = 0;

    if (!ram_ncopies) {
        error_report("fastsnap: RAM restore with no snapshot taken");
        return -1;
    }

    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH(block) {
        const FastsnapRamCopy *copy;
        unsigned long *bmap;
        uint64_t npages, n;
        unsigned long bit = 0;

        if (!block->host || !block->used_length) {
            continue;
        }
        copy = ram_copy_for(block->idstr);
        if (!copy) {
            error_report("fastsnap: RAM block '%s' appeared after the "
                         "snapshot; refusing a partial restore", block->idstr);
            return -1;
        }
        if (copy->len != block->used_length) {
            error_report("fastsnap: RAM block '%s' was resized (%" PRIu64
                         " -> %" PRIu64 "); refusing a partial restore",
                         block->idstr, copy->len,
                         (uint64_t)block->used_length);
            return -1;
        }

        npages = DIV_ROUND_UP(block->used_length, psize);
        bmap = bitmap_new(npages);

        /* test_and_clear, so this both reads the set and re-arms: the clear
         * path reaches tlb_reset_dirty_range_all(), which puts TLB_NOTDIRTY
         * back so the next interval traps the first store to each page again.
         * Without that the second interval silently under-counts. */
        n = fastsnap_dirty_take(block, bmap);
        if (n) {
            for (;;) {
                uint64_t off, len;

                bit = find_next_bit(bmap, npages, bit);
                if (bit >= npages) {
                    break;
                }
                off = (uint64_t)bit * psize;
                len = MIN(psize, block->used_length - off);

                memcpy(block->host + off, copy->data + off, len);
                /*
                 * tcg_enabled() folds to a compile-time false in a build
                 * without CONFIG_TCG, which removes the call entirely -- the
                 * same idiom system/physmem.c uses, and it is load-bearing
                 * rather than stylistic: tb_invalidate_phys_range() is a TCG
                 * symbol with no stub, so an unguarded call links fine against
                 * a softmmu target and fails to link libqemu-kvm-*.so. The
                 * fastsnap selftest builds only aarch64-softmmu, so it cannot
                 * catch that; the penguin image build did.
                 */
                if (tcg_enabled()) {
                    /* Cleared by tlb_protect_code() when a TB is built on this
                     * page, so a SET flag means there is no code here and
                     * nothing to invalidate. Counted either way. */
                    bool has_code = !physical_memory_get_dirty_flag(
                        block->offset + off, DIRTY_MEMORY_CODE);

                    if (has_code) {
                        code_pages++;
                    }
                    if (has_code || !fastsnap_tb_guard()) {
                        tb_invalidate_phys_range(NULL, block->offset + off,
                                                 block->offset + off + len - 1);
                    }
                }
                restored++;
                bit++;
            }
        }
        g_free(bmap);
    }

    ram_restored_pages = restored;
    ram_restored_code_pages = code_pages;
    return (int64_t)restored;
}
