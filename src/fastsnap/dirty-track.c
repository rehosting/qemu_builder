/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The size of the guest's write working set, per interval.
 *
 * WHY THIS EXISTS. A dirty-page reset costs O(dirty pages), and the host-side
 * numbers in penguin/analysis/fastsnap/RESET.md put the same 256 MB guest at
 * 79 us for a 1 MB dirty set and 2,136 us for a 16 MB one -- a 27x spread, and
 * the difference between a design that parallelises 24 ways and one that
 * parallelises twice. Nothing in the projection measured which applies.
 *
 * THE MECHANISM IS QEMU'S OWN, deliberately. Live migration already has to
 * answer "which pages changed since I last looked", and it answers it with the
 * DIRTY_MEMORY_MIGRATION bitmap that TCG maintains from the softmmu store
 * path. Building a second tracker would mean a second thing to be wrong, with
 * no independent check on it; this one is the same machinery that moves guests
 * between hosts.
 *
 * HOW IT ARMS, which is the part that is not obvious. TCG marks a page's TLB
 * entry TLB_NOTDIRTY only while cpu_physical_memory_is_clean() holds, and a
 * page goes clean by having its dirty bits CLEARED. So clearing the bitmap is
 * not bookkeeping after the fact -- it is the arming step, and it only works
 * because physical_memory_test_and_clear_dirty() ends in
 * physical_memory_dirty_bits_cleared() -> tlb_reset_dirty_range_all(), which
 * walks every vCPU's TLB and puts TLB_NOTDIRTY back on entries that had
 * already been resolved as dirty. Without that, pages already live in a TLB
 * would be written without ever setting a bit, and the count would come back
 * plausibly small and wrong.
 *
 * WHY GLOBAL_DIRTY_DIRTY_RATE AND NOT GLOBAL_DIRTY_MIGRATION. Both raise the
 * same bitmap -- global_dirty_tracking is tested as a whole in
 * memory_region_get_dirty_log_mask(). But savevm/loadvm go through the
 * migration code, and that takes and releases GLOBAL_DIRTY_MIGRATION itself.
 * Sharing the flag with it means a savevm's log_stop clears the flag this
 * module believes it holds, and tracking silently stops while every accessor
 * keeps returning numbers. GLOBAL_DIRTY_DIRTY_RATE is the flag for exactly
 * this use -- measuring, not moving -- and does not collide.
 *
 * The global flag is what makes DMA count. TCG's store path sets the migration
 * bit unconditionally, but the address_space_write()/DMA path goes through
 * invalidate_and_set_dirty(), which consults
 * memory_region_get_dirty_log_mask() and includes DIRTY_MEMORY_MIGRATION only
 * while global_dirty_tracking is non-zero. So without the arm, a block written
 * by a device -- or by the selftest's own control poke -- would not be
 * counted at all.
 *
 * WHAT IT COUNTS, stated so the number is not read as more than it is:
 *
 *  - Pages whose CONTENTS were written. Not pages read, not pages executed.
 *    That is the right set for a reset that copies back what changed.
 *  - Per target page (qemu_target_page_size()), across every RAMBlock with a
 *    host mapping -- main RAM, but also pflash and any device RAM. The
 *    per-block breakdown is reported separately precisely so a caller can see
 *    whether the number is the guest's working set or a flash write.
 *  - A page written ten times in one interval counts once. A page written and
 *    then written back to its original value still counts: this measures what
 *    a reset would have to copy, not what actually differs. That is the
 *    correct quantity for a cost model and the wrong one for a correctness
 *    oracle -- the fork oracle in fork-oracle.c is the latter.
 *
 * WHAT IT DOES NOT COUNT. A RAMBlock with RAM_MIGRATABLE clear gets no
 * migration bit from the DMA path (memory_region_get_dirty_log_mask() gates on
 * qemu_ram_is_migratable()), so device writes into such a block are invisible
 * here even though vCPU writes to it are not. Those blocks are named in the
 * block report with a '!' so an under-count has somewhere to show up rather
 * than being folded silently into a total.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/bitmap.h"
#include "qemu/rcu.h"
#include "qapi/error.h"
#include "system/memory.h"
#include "system/physmem.h"
#include "system/ram_addr.h"
#include "system/ramblock.h"
#include "system/ramlist.h"
#include "exec/target_page.h"

#include "fastsnap/dirty-track.h"

#define FASTSNAP_DIRTY_REPORT_MAX 12

static bool dirty_armed;
static uint64_t dirty_pages;
static uint64_t dirty_scanned;
static char dirty_report_buf[1024];
static char dirty_blocks_buf[1024];

uint64_t fastsnap_dirty_pages(void) { return dirty_pages; }
uint64_t fastsnap_dirty_pages_scanned(void) { return dirty_scanned; }
uint64_t fastsnap_dirty_page_size(void) { return qemu_target_page_size(); }
const char *fastsnap_dirty_report(void) { return dirty_report_buf; }
const char *fastsnap_dirty_blocks(void) { return dirty_blocks_buf; }

static void dirty_append(char *buf, size_t cap, size_t *used, const char *fmt,
                         ...) G_GNUC_PRINTF(4, 5);

static void dirty_append(char *buf, size_t cap, size_t *used, const char *fmt,
                         ...)
{
    va_list ap;
    int n;

    if (*used + 1 >= cap) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(buf + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (n > 0 && *used + (size_t)n < cap) {
        *used += (size_t)n;
    }
}

/*
 * One pass over every RAMBlock: read the migration dirty bits, clear them,
 * and let the clear re-arm TCG's TLB.
 *
 * @record false is the arming pass -- at arm time every bit is still set from
 * RAM creation, so recording would produce a report of the entire guest. The
 * counters are still updated, so a caller can see the arm cleared a whole
 * memory's worth of bits rather than nothing.
 *
 * Caller holds the BQL with the vCPUs stopped, so no write can land between
 * the read of a block's bits and the clear of them. Off that safe point the
 * bits are still atomic but the interval boundary is not, and the count would
 * belong to no particular stretch of execution.
 */
static int64_t dirty_walk(bool record)
{
    RAMBlock *block;
    size_t psize = qemu_target_page_size();
    uint64_t total = 0, scanned = 0;
    size_t report_used = 0, blocks_used = 0;
    int nreported = 0;

    dirty_report_buf[0] = '\0';
    dirty_blocks_buf[0] = '\0';

    /*
     * A no-op under TCG, where the vCPU sets the bit as it stores. Called
     * anyway because it is what makes this correct under an accelerator whose
     * bitmap lives in the kernel, and leaving it out would make the code
     * quietly TCG-only for a reason no one would find later.
     */
    memory_global_dirty_log_sync(false);

    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH(block) {
        unsigned long *bmap;
        uint64_t npages, n;

        if (!block->host || block->used_length == 0) {
            continue;
        }

        npages = DIV_ROUND_UP(block->used_length, psize);
        bmap = bitmap_new(npages);

        n = physical_memory_test_and_clear_dirty(block->offset,
                                                 block->used_length,
                                                 DIRTY_MEMORY_MIGRATION, bmap);
        total += n;
        scanned += npages;

        if (record && n) {
            unsigned long bit = 0;

            dirty_append(dirty_blocks_buf, sizeof(dirty_blocks_buf),
                         &blocks_used, "%s%s%s=%" PRIu64,
                         blocks_used ? ", " : "",
                         qemu_ram_is_migratable(block) ? "" : "!",
                         block->idstr, n);

            while (nreported < FASTSNAP_DIRTY_REPORT_MAX) {
                bit = find_next_bit(bmap, npages, bit);
                if (bit >= npages) {
                    break;
                }
                dirty_append(dirty_report_buf, sizeof(dirty_report_buf),
                             &report_used, "%s%s+0x%" PRIx64,
                             report_used ? ", " : "", block->idstr,
                             (uint64_t)bit * psize);
                nreported++;
                bit++;
            }
        }

        g_free(bmap);
    }

    dirty_pages = total;
    dirty_scanned = scanned;
    return (int64_t)total;
}

int fastsnap_dirty_arm(void)
{
    Error *err = NULL;

    if (!dirty_armed) {
        if (!memory_global_dirty_log_start(GLOBAL_DIRTY_DIRTY_RATE, &err)) {
            error_report("fastsnap: could not start dirty logging: %s",
                         error_get_pretty(err));
            error_free(err);
            return -1;
        }
        dirty_armed = true;
    }

    /*
     * Clear whatever is standing. At the first arm that is every page in the
     * machine -- RAM blocks are created with all dirty bits set -- so without
     * this the first count would report the whole guest and look like a
     * catastrophic working set rather than an un-zeroed counter.
     */
    dirty_walk(false);
    return 0;
}

int64_t fastsnap_dirty_count(void)
{
    if (!dirty_armed) {
        error_report("fastsnap: dirty count with tracking not armed; the "
                     "bitmap is not being maintained for the DMA path, so a "
                     "low number here would mean nothing");
        return -1;
    }
    return dirty_walk(true);
}

void fastsnap_dirty_stop(void)
{
    if (!dirty_armed) {
        return;
    }
    memory_global_dirty_log_stop(GLOBAL_DIRTY_DIRTY_RATE);
    dirty_armed = false;
}
