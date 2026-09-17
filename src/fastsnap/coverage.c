/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Edge coverage for the fastsnap loop. See include/fastsnap/coverage.h for
 * what this is for and for the three ways it can read as a healthy zero.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "hw/core/cpu.h"

/*
 * THE ABI IS UNCONDITIONAL, THE MECHANISM IS NOT.
 *
 * This logs edges by emitting ops into translated blocks, so it cannot exist
 * under KVM -- there are no translated blocks. But the penguin-facing symbols
 * still have to be in every library we ship, including libqemu-kvm-x86_64.so:
 * scripts/check-delta-present.sh derives the required ABI from
 * penguin-fastsnap.h and asserts every name is present in every system
 * library, and penguin's own preflight refuses a run over an absent symbol.
 * Dropping the file from the TCG-less build would turn "coverage does not
 * apply here" into "this library is missing part of the ABI", which is a
 * different and much more alarming report.
 *
 * So the file always builds and COV_ARM fails with a reason instead.
 */
#ifdef CONFIG_TCG
#include "exec/tb-flush.h"
#include "tcg/tcg-op-common.h"
#endif

#include "fastsnap/coverage.h"
#include "fastsnap/penguin-fastsnap.h"

/*
 * AFL's MAP_SIZE. Kept as the default because a corpus and a map are only
 * comparable at the same size, and every tool that might consume this one
 * (afl-showmap, a libafl observer over shared memory) assumes 1 << 16 unless
 * told otherwise.
 */
#define COV_MAP_SIZE_DEFAULT (1u << 16)
#define COV_MAP_SIZE_MIN     (1u << 8)
#define COV_MAP_SIZE_MAX     (1u << 24)

/*
 * THE MAP POINTER AND THE MASK ARE BAKED INTO GENERATED CODE. Neither may
 * change once a single block has been translated with them -- see the header.
 * cov_map is allocated at the first arm and never freed.
 */
static uint8_t *cov_map;
static uint8_t *cov_virgin;          /* OR of hit-count buckets, ever */
static uint32_t cov_map_size = COV_MAP_SIZE_DEFAULT;
static uint32_t cov_map_mask = COV_MAP_SIZE_DEFAULT - 1;

/* AFL's prev_loc. Host-global on purpose; see the header on multi-vCPU. */
static uint32_t cov_prev;

static bool cov_armed;
static bool cov_clear_on_reset = true;

/* Inclusive lo, exclusive hi. hi == 0 means "no filter, instrument the lot". */
static uint64_t cov_filter_lo;
static uint64_t cov_filter_hi;

/* Per-lap results, published by cov_scan(). */
static uint64_t cov_edges;
static uint64_t cov_hits;
static uint64_t cov_new_edges;
static uint64_t cov_new_buckets;
static uint64_t cov_total_edges;
static int64_t cov_scan_us;

/* Diagnostics. These are what distinguish "the guest found nothing" from
 * "the filter range is wrong and nothing was ever instrumented". */
static uint64_t cov_tbs_instrumented;
static uint64_t cov_tbs_filtered;

/*
 * PC -> block id. Any deterministic mix will do; what matters is that the
 * same PC yields the same id in every run, so a corpus collected against one
 * run means something against the next. splitmix64's finalizer, truncated.
 */
static inline uint32_t cov_block_id(uint64_t pc)
{
    uint64_t z = pc + 0x9e3779b97f4a7c15ULL;

    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    z = z ^ (z >> 31);
    return (uint32_t)z;
}

/*
 * AFL's hit-count buckets. Novelty is judged per bucket rather than per edge
 * because "this edge ran 3 times instead of 1" is the signal that finds loop
 * bounds, and an edge-presence bitmap cannot see it.
 */
static inline uint8_t cov_bucket(uint8_t n)
{
    if (n == 0) {
        return 0;
    } else if (n == 1) {
        return 1 << 0;
    } else if (n == 2) {
        return 1 << 1;
    } else if (n == 3) {
        return 1 << 2;
    } else if (n <= 7) {
        return 1 << 3;
    } else if (n <= 15) {
        return 1 << 4;
    } else if (n <= 31) {
        return 1 << 5;
    } else if (n <= 127) {
        return 1 << 6;
    }
    return 1 << 7;
}

#ifdef CONFIG_TCG

void fastsnap_cov_translate(uint64_t pc)
{
    TCGv_i32 prev, idx, cnt;
    TCGv_ptr slot;
    uint32_t cur;

    if (!cov_armed) {
        return;
    }
    if (cov_filter_hi && (pc < cov_filter_lo || pc >= cov_filter_hi)) {
        cov_tbs_filtered++;
        return;
    }
    cov_tbs_instrumented++;

    cur = cov_block_id(pc) & cov_map_mask;

    prev = tcg_temp_new_i32();
    idx = tcg_temp_new_i32();
    cnt = tcg_temp_new_i32();
    slot = tcg_temp_new_ptr();

    /*
     * idx = prev ^ cur. Both operands are already inside the map -- cur is
     * masked just above, and prev is only ever stored as a masked cur >> 1 --
     * so the xor is too, and no runtime mask is needed. That is one op saved
     * on every block execution, and it is only sound because of the store at
     * the bottom of this function; the two have to be read together.
     */
    tcg_gen_ld_i32(prev, tcg_constant_ptr(&cov_prev), 0);
    tcg_gen_xori_i32(idx, prev, cur);

    tcg_gen_ext_i32_ptr(slot, idx);
    tcg_gen_add_ptr(slot, tcg_constant_ptr(cov_map), slot);

    /* Byte counter, wrapping. AFL's buckets make 255->0 a rounding error in
     * the top bucket rather than a lost edge. */
    tcg_gen_ld8u_i32(cnt, slot, 0);
    tcg_gen_addi_i32(cnt, cnt, 1);
    tcg_gen_st8_i32(cnt, slot, 0);

    tcg_gen_st_i32(tcg_constant_i32(cur >> 1), tcg_constant_ptr(&cov_prev), 0);
}

/*
 * QUEUED, NOT CALLED, and the difference is an assertion failure.
 *
 * tb_flush__exclusive_or_serial() asserts
 *     !runstate_is_running() || (current_cpu && cpu_in_serial_context(...))
 * and neither holds at an arm. fastsnap_bh() deliberately stops the vCPUs with
 * pause_all_vcpus() rather than vm_stop(), precisely so the runstate never
 * enters RUN_STATE_RESTORE_VM and accel/tcg never forces a flush of its own --
 * so runstate_is_running() is still true -- and we are on the main loop, so
 * current_cpu is NULL. Calling it directly aborts QEMU.
 *
 * queue_tb_flush() defers it onto the vCPU's work queue via
 * async_safe_run_on_cpu(), which runs it in exclusive context. The vCPUs are
 * paused, so the flush happens when they resume and BEFORE they execute any
 * further guest code -- which is the ordering this needs: every block that
 * runs after an arm is translated after it, and so carries instrumentation.
 */
static void cov_flush_translations(void)
{
    if (first_cpu) {
        queue_tb_flush(first_cpu);
    }
}

#else  /* !CONFIG_TCG */

void fastsnap_cov_translate(uint64_t pc)
{
    /* No translation happens under KVM, so nothing calls this. */
}

static void cov_flush_translations(void)
{
}

#endif /* CONFIG_TCG */

/*
 * One pass over the map: count this lap's edges and hits, work out what is
 * new against the cumulative map, fold it in, and optionally zero the lap map.
 *
 * The zero-word skip is what keeps this off the lap budget. A lap of this
 * workload touches a few thousand edges out of 65,536, so all but a few
 * hundred of the 8,192 words are zero and are read once and dropped.
 */
static void cov_scan(bool clear)
{
    uint64_t *w = (uint64_t *)cov_map;
    uint64_t *v = (uint64_t *)cov_virgin;
    size_t nwords = cov_map_size / sizeof(uint64_t);
    uint64_t edges = 0, hits = 0, new_edges = 0, new_buckets = 0;
    int64_t t0;
    size_t i;

    if (!cov_map) {
        cov_edges = cov_hits = cov_new_edges = cov_new_buckets = 0;
        cov_scan_us = 0;
        return;
    }

    t0 = g_get_monotonic_time();
    for (i = 0; i < nwords; i++) {
        uint8_t *b, *vb;
        int k;

        if (!w[i]) {
            continue;
        }
        b = (uint8_t *)&w[i];
        vb = (uint8_t *)&v[i];
        for (k = 0; k < (int)sizeof(uint64_t); k++) {
            uint8_t bucket;

            if (!b[k]) {
                continue;
            }
            edges++;
            hits += b[k];
            bucket = cov_bucket(b[k]);
            if (!vb[k]) {
                new_edges++;
                cov_total_edges++;
            }
            if (bucket & ~vb[k]) {
                new_buckets++;
            }
            vb[k] |= bucket;
        }
        if (clear) {
            w[i] = 0;
        }
    }
    if (clear) {
        cov_prev = 0;
    }

    cov_edges = edges;
    cov_hits = hits;
    cov_new_edges = new_edges;
    cov_new_buckets = new_buckets;
    cov_scan_us = g_get_monotonic_time() - t0;
}

int fastsnap_cov_arm(void)
{
#ifndef CONFIG_TCG
    error_report("fastsnap: edge coverage needs TCG -- it is logged by ops "
                 "emitted into translated blocks, and KVM translates nothing. "
                 "Refusing rather than arming something that would report "
                 "zero edges forever.");
    return -1;
#else
    if (!cov_map) {
        cov_map = g_try_malloc0(cov_map_size);
        cov_virgin = g_try_malloc0(cov_map_size);
        if (!cov_map || !cov_virgin) {
            g_free(cov_map);
            g_free(cov_virgin);
            cov_map = cov_virgin = NULL;
            error_report("fastsnap: coverage map allocation failed (%u bytes)",
                         cov_map_size * 2);
            return -1;
        }
    } else {
        memset(cov_map, 0, cov_map_size);
    }
    cov_prev = 0;
    cov_tbs_instrumented = 0;
    cov_tbs_filtered = 0;
    cov_armed = true;
    cov_flush_translations();
    return 0;
#endif
}

void fastsnap_cov_disarm(void)
{
    cov_armed = false;
    /* Same reasoning as the arm: without a flush the blocks already
     * translated keep logging into the map after the caller believes it has
     * stopped, and a stale lap count is worse than no lap count. */
    cov_flush_translations();
}

void fastsnap_cov_clear(void)
{
    if (cov_map) {
        memset(cov_map, 0, cov_map_size);
    }
    cov_prev = 0;
}

void fastsnap_cov_read(void)
{
    cov_scan(false);
}

void fastsnap_cov_lap_end(void)
{
    cov_scan(true);
}

bool fastsnap_cov_clear_on_reset(void)
{
    return cov_clear_on_reset && cov_armed;
}

/* ---- the Penguin-facing ABI ---------------------------------------- */

/*
 * Restrict instrumentation to [lo, hi). hi == 0 instruments everything,
 * kernel included -- useful as a control, and roughly 5x the blocks.
 *
 * Set this BEFORE COV_ARM. It is read at translation time, so a change after
 * blocks are cached applies only to blocks translated afterwards; the arm's
 * flush is what makes a range change take effect everywhere.
 */
void __attribute__((visibility("default")))
penguin_fastsnap_cov_set_filter(uint64_t lo, uint64_t hi)
{
    cov_filter_lo = lo;
    cov_filter_hi = hi;
}

/*
 * Size the map. Power of two, 256 .. 16M, and only honoured before the FIRST
 * arm -- after that the pointer and mask are compiled into translated blocks
 * and cannot move. Returns false if the value was rejected or came too late,
 * rather than silently keeping the old size.
 */
bool __attribute__((visibility("default")))
penguin_fastsnap_cov_set_map_size(uint64_t size)
{
    if (cov_map) {
        error_report("fastsnap: coverage map already allocated at %u bytes; "
                     "the size is compiled into translated blocks and cannot "
                     "change in a live process", cov_map_size);
        return false;
    }
    if (size < COV_MAP_SIZE_MIN || size > COV_MAP_SIZE_MAX ||
        (size & (size - 1))) {
        error_report("fastsnap: coverage map size %" PRIu64 " must be a power "
                     "of two in [%u, %u]", size,
                     COV_MAP_SIZE_MIN, COV_MAP_SIZE_MAX);
        return false;
    }
    cov_map_size = (uint32_t)size;
    cov_map_mask = cov_map_size - 1;
    return true;
}

/*
 * Whether LOOP_RESET and LOOP_RESET_VERIFY summarise and clear the map as
 * part of the reset. On by default, because the alternative is one more
 * scheduled op per lap and an op costs more than the scan does.
 */
void __attribute__((visibility("default")))
penguin_fastsnap_cov_set_clear_on_reset(bool on)
{
    cov_clear_on_reset = on;
}

/*
 * The map's host address, as an integer. Returned as uint64_t rather than a
 * pointer because scripts/penguin-cffi-gen.py extracts prototypes by return
 * type and would silently drop a `uint8_t *` one -- an omission that reads
 * downstream as an accessor that always answers zero. ffi.cast on the Python
 * side; 0 means no map has been allocated yet.
 */
uint64_t __attribute__((visibility("default")))
penguin_fastsnap_cov_map_addr(void)
{
    return (uint64_t)(uintptr_t)cov_map;
}

uint64_t __attribute__((visibility("default")))
penguin_fastsnap_cov_map_size(void)
{
    return cov_map_size;
}

bool __attribute__((visibility("default")))
penguin_fastsnap_cov_armed(void)
{
    return cov_armed;
}

/* Distinct edges touched in the last summarised lap. */
uint64_t __attribute__((visibility("default")))
penguin_fastsnap_cov_edges(void)
{
    return cov_edges;
}

/* Total block executions logged in that lap (byte counters, so saturating at
 * 255 per edge). A rough proxy for how much code the lap ran. */
uint64_t __attribute__((visibility("default")))
penguin_fastsnap_cov_hits(void)
{
    return cov_hits;
}

/* Edges seen for the first time ever in that lap. */
uint64_t __attribute__((visibility("default")))
penguin_fastsnap_cov_new_edges(void)
{
    return cov_new_edges;
}

/*
 * Edges that hit a hit-count bucket not seen before -- AFL's "is this input
 * interesting" signal, and a strictly weaker condition than a new edge. This
 * is the one to drive corpus decisions from: an edge that ran three times
 * instead of once is how loop bounds get found, and new_edges cannot see it.
 */
uint64_t __attribute__((visibility("default")))
penguin_fastsnap_cov_new_buckets(void)
{
    return cov_new_buckets;
}

/* Distinct edges seen across every lap since the first arm. */
uint64_t __attribute__((visibility("default")))
penguin_fastsnap_cov_total_edges(void)
{
    return cov_total_edges;
}

/*
 * Blocks instrumented and blocks skipped by the filter, since the last arm.
 *
 * READ THESE BEFORE BELIEVING A ZERO. An empty map has two causes that look
 * identical from the map itself: the guest genuinely executed nothing new, or
 * the filter range names an address the target's code never occupies and
 * nothing was ever instrumented. instrumented == 0 with filtered > 0 is the
 * second, and it is the likelier one the first time a range is configured.
 */
uint64_t __attribute__((visibility("default")))
penguin_fastsnap_cov_tbs_instrumented(void)
{
    return cov_tbs_instrumented;
}

uint64_t __attribute__((visibility("default")))
penguin_fastsnap_cov_tbs_filtered(void)
{
    return cov_tbs_filtered;
}

/* How long the last summarising pass took. Part of the reset when
 * clear_on_reset is on, so it belongs in the lap budget. */
int64_t __attribute__((visibility("default")))
penguin_fastsnap_cov_scan_us(void)
{
    return cov_scan_us;
}
