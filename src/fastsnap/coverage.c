/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Edge coverage for the fastsnap loop. See include/fastsnap/coverage.h for
 * what this is for and for the three ways it can read as a healthy zero.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/host-utils.h"
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

/*
 * OUTSIDE the CONFIG_TCG guard on purpose. The hash is arithmetic, not
 * codegen, and the selftest checks it in every build -- including the KVM one,
 * where nothing else in this file can run.
 */
uint32_t fastsnap_cov_block_index(uint64_t pc)
{
    return cov_block_id(pc) & cov_map_mask;
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
 * ---- summarising a lap ----------------------------------------------
 *
 * One pass over the map: count this lap's edges and hits, work out what is
 * new against the cumulative map, fold it in, and optionally zero the lap map.
 *
 * WHAT IT COSTS, measured by fastsnap_cov_scan_selfcheck() below on synthetic
 * maps and cross-checked against real firmware runs:
 *
 *     cost ~= 0.56 ns x (words in the map)
 *           + 29 ns   x (words that are not zero)
 *           + 4.1 ns  x (bytes that are set)
 *
 * Fitted at 1 MiB on two shapes with the same number of set bytes in eight
 * times fewer words, then used to PREDICT a 64 KiB map: 125.7 us against
 * 123.4 measured, 2%. On this workload's laps the middle term is the whole
 * story -- a few thousand edges land in very nearly as many distinct words,
 * so it is about 92 of the 165 us, against 73 for the skim and 11 for the
 * bytes themselves.
 *
 * THAT MIDDLE TERM REPLACED A WRONG ONE, and the error is worth keeping.
 * The model this lane used until now read
 *
 *     cost ~= 0.5 ns x (words skimmed) + 5.1 ns x (bytes in non-zero words)
 *
 * which fitted the real runs just as well -- because in every one of them a
 * non-zero word held about one set byte, making "bytes in non-zero words"
 * numerically identical to "8 x non-zero words". The fit could not tell the
 * two apart, and they recommend opposite things: the first says stop touching
 * seven dead bytes per word, the second says stop paying whatever is paid on
 * arrival at a word. Only a map with the bytes-per-word ratio deliberately
 * changed can separate them. That is what COV_FILL_PACKED is for.
 *
 * WHAT THE 29 ns IS remains open, and four attempts to remove it are recorded
 * on cov_fold() below. It is not the skim, not the seven unset bytes, not
 * branch misprediction on the novelty tests, and not hideable by prefetching
 * the cumulative map. It is ~116 cycles per word arrived at, with everything
 * cache-resident, and it is where any future win here has to come from.
 */
typedef struct CovFold {
    uint64_t edges;
    uint64_t hits;
    uint64_t new_edges;
    uint64_t new_buckets;
} CovFold;

/*
 * THE ONE THAT RUNS IN THE LOOP -- and it is the reference loop, because
 * nothing else tried here was faster. What follows is why, since "we left it
 * alone" is otherwise indistinguishable from "nobody looked".
 *
 * The lane's open-questions list named two levers and sized the pair at well
 * over 150 us a lap. Both were measured, paired against the shipped scan
 * control on the same host in the same process; see
 * fastsnap_cov_scan_selfcheck() for the rig and the shapes.
 *
 * LEVER 1, "walk the set bytes with ctz64 instead of all eight". At real
 * occupancy a non-zero word holds about one set byte, so seven of every eight
 * byte iterations look wasted. Measured: slightly SLOWER, and on a map packed
 * to eight set bytes per word -- where it should have won biggest -- it lost
 * by more. Eight independent byte tests issue in parallel; a ctz walk is a
 * serial dependency chain through the word being cleared. The seven "wasted"
 * iterations never cost seven iterations of time.
 *
 * LEVER 2, "the skim is O(map size), and the map is 99.5% empty". True, and
 * the skim really is about 45% of the pass: on an EMPTY 1 MiB map, which
 * isolates the term because nothing is folded, the reference takes ~72 us and
 * an eight-words-at-a-time skim takes ~50 us. But the saving does not survive
 * a map with anything in it. Tested three ways -- a separate chunk-folding
 * function, the same fused into the skim loop keeping the loaded words, and
 * either of those with the cumulative map's line prefetched and the fold
 * deferred eight chunks behind -- every one landed between 195 and 202 us
 * against the reference's 158-172 us at the production shape. 1.4 ns saved
 * per empty chunk against ~12 ns lost per non-empty one puts break-even near
 * 10% of chunks occupied; this workload runs at 15%. cov_fold_chunked() is
 * kept and benched so the next person can re-measure rather than re-derive.
 *
 * WHY BOTH LEVERS MISSED is the same reason, and it was hidden in the cost
 * model this lane had been using:
 *
 *     cost ~= 0.5 ns x (words skimmed) + 5.1 ns x (bytes in non-zero words)
 *
 * In every run that was fitted on, each non-zero word held about one set
 * byte, so the second term was numerically identical to "41 ns per non-zero
 * word". The fit cannot tell those apart, and they say opposite things about
 * what to fix. Handing this function the SAME number of set bytes packed into
 * an eighth as many words settles it: the fold cost falls about fivefold. The
 * term is per non-zero word, ~34 ns, about 135 cycles. That is a stall, and
 * no arrangement of the byte loop or the skim touches it.
 *
 * WHAT THAT PER-WORD COST IS, the rig also answers, and the answer is why
 * there is nothing left to do here. Running the SAME code over a cumulative
 * map pre-filled with 0xff makes both novelty tests resolve the same way
 * every time -- identical memory traffic, perfectly predicted branches. See
 * the saturated shapes in fastsnap_cov_scan_selfcheck().
 */
static CovFold cov_fold(uint64_t *w, uint64_t *v, size_t nwords, bool clear)
{
    CovFold f = { 0, 0, 0, 0 };
    size_t i;

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
            f.edges++;
            f.hits += b[k];
            bucket = cov_bucket(b[k]);
            if (!vb[k]) {
                f.new_edges++;
            }
            if (bucket & ~vb[k]) {
                f.new_buckets++;
            }
            vb[k] |= bucket;
        }
        if (clear) {
            w[i] = 0;
        }
    }
    return f;
}
/*
 * Bench probe: eight words skimmed at a time, one branch per cache line
 * instead of eight. Not used by cov_scan(); see above for the numbers that
 * kept it out, and fastsnap_cov_scan_selfcheck() checks it still agrees with
 * the reference exactly, so re-measuring it is a one-line change rather than
 * a rewrite.
 */
#define COV_SKIM_WORDS 8        /* 64 bytes: one cache line */

static CovFold cov_fold_chunked(uint64_t *w, uint64_t *v, size_t nwords,
                                bool clear)
{
    CovFold f = { 0, 0, 0, 0 };
    size_t i;

    /*
     * The map size is a power of two of at least COV_MAP_SIZE_MIN bytes, so
     * nwords is a power of two of at least 32 and is always a whole number of
     * skim chunks. Asserted rather than assumed: a remainder would leave the
     * tail of the map unscanned and report it as no coverage.
     */
    assert(nwords >= COV_SKIM_WORDS && nwords % COV_SKIM_WORDS == 0);

    for (i = 0; i < nwords; i += COV_SKIM_WORDS) {
        uint64_t any = 0;
        size_t j;

        for (j = 0; j < COV_SKIM_WORDS; j++) {
            any |= w[i + j];
        }
        if (!any) {
            continue;
        }
        for (j = 0; j < COV_SKIM_WORDS; j++) {
            uint8_t *b, *vb;
            int k;

            if (!w[i + j]) {
                continue;
            }
            b = (uint8_t *)&w[i + j];
            vb = (uint8_t *)&v[i + j];
            for (k = 0; k < (int)sizeof(uint64_t); k++) {
                uint8_t bucket;

                if (!b[k]) {
                    continue;
                }
                f.edges++;
                f.hits += b[k];
                bucket = cov_bucket(b[k]);
                if (!vb[k]) {
                    f.new_edges++;
                }
                if (bucket & ~vb[k]) {
                    f.new_buckets++;
                }
                vb[k] |= bucket;
            }
            if (clear) {
                w[i + j] = 0;
            }
        }
    }
    return f;
}

/*
 * CANDIDATE: the byte fold with every data-dependent branch removed.
 *
 * The attribution probe above says most of the pass is the per-non-zero-word
 * term, and the 64 KiB shape says that term is NOT a memory stall -- a 64 KiB
 * cumulative map is cache-resident, and removing it still took 130 us down to
 * 38 us. What is left in those bytes is three unpredictable branches: is this
 * byte set, is this edge new, is this bucket new. A set byte's position within
 * its word is random, and on a fuzzing campaign past its first seconds "is
 * this new" is a coin-flip that resolves late.
 *
 * So do all eight bytes unconditionally and turn each question into
 * arithmetic. cov_bucket(0) is 0, which is what makes this safe: a zero byte
 * contributes nothing to any counter and ORs nothing into the cumulative map,
 * so the loop needs no guard to skip it. The cost is eight fixed iterations
 * per non-zero word instead of one-plus-seven-cheap-ones; the saving is every
 * mispredict in them.
 */
static CovFold cov_fold_branchless(uint64_t *w, uint64_t *v, size_t nwords,
                                   bool clear)
{
    CovFold f = { 0, 0, 0, 0 };
    size_t i;

    for (i = 0; i < nwords; i++) {
        uint8_t *b, *vb;
        int k;

        if (!w[i]) {
            continue;
        }
        b = (uint8_t *)&w[i];
        vb = (uint8_t *)&v[i];
        for (k = 0; k < (int)sizeof(uint64_t); k++) {
            uint8_t c = b[k];
            uint8_t seen = vb[k];
            uint8_t bucket = cov_bucket(c);

            f.edges += (c != 0);
            f.hits += c;
            f.new_edges += (c != 0) & (seen == 0);
            f.new_buckets += ((bucket & ~seen) != 0);
            vb[k] = seen | bucket;
        }
        if (clear) {
            w[i] = 0;
        }
    }
    return f;
}

static void cov_scan(bool clear)
{
    CovFold f;
    int64_t t0;

    if (!cov_map) {
        cov_edges = cov_hits = cov_new_edges = cov_new_buckets = 0;
        cov_scan_us = 0;
        return;
    }

    t0 = g_get_monotonic_time();
    f = cov_fold((uint64_t *)cov_map, (uint64_t *)cov_virgin,
                 cov_map_size / sizeof(uint64_t), clear);
    cov_scan_us = g_get_monotonic_time() - t0;

    if (clear) {
        cov_prev = 0;
    }
    cov_edges = f.edges;
    cov_hits = f.hits;
    cov_new_edges = f.new_edges;
    cov_new_buckets = f.new_buckets;
    cov_total_edges += f.new_edges;
}

/*
 * ---- the differential check, and the bench -----------------------------
 *
 * TWO JOBS, AND THE FIRST IS WHY THE SECOND IS SAFE TO ACT ON.
 *
 * Every other failure in this file is arranged to read as a zero: an empty
 * map, no instrumented blocks, no edges after a disarm. A zero is something a
 * test can insist on. The summarising pass is not like that. Miscount a byte,
 * fold into the wrong half of a word, skip the tail of the map, and the
 * result is not a zero -- it is a NUMBER of the right order of magnitude,
 * with a cumulative map that keeps filling. Nothing downstream would reject
 * it; the campaign would simply explore less than it should, forever, and the
 * scan would be the last place anyone looked.
 *
 * So every candidate implementation is run beside the shipped one over the
 * same synthetic map and required to agree EXACTLY -- not only on the four
 * counters, but on the resulting cumulative map and the cleared lap map, byte
 * for byte. That last pair is the half that matters: counts can agree while
 * the bytes folded into the cumulative map differ, which is precisely what a
 * host-byte-order mistake produces, and this file is compiled for big-endian
 * hosts too.
 *
 * THE BENCH exists because this scan has now attracted four optimisations
 * that looked obvious and were not, and re-deriving that is expensive. It
 * times each candidate against the shipped scan, paired and alternating in
 * one process, and it times the shipped scan TWICE -- first and last. If
 * those two disagree, the numbers in between are a story about slot order and
 * cache rather than about the implementations, and the caller is told so
 * instead of being left to read four plausible figures.
 *
 * Two things flatter every number here equally, so neither changes a ranking:
 * the map is memcpy'd in immediately before each timed pass, so it is
 * cache-hot in a way a real lap's is not, and the synthetic edges are uniform
 * where a real lap's are clustered by the hash.
 */
static uint64_t cov_check_rng(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);

    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/*
 * Put @edges nonzero bytes into @map, values 1..255, and count how many
 * distinct bytes ended up set (fewer than @edges: the draws collide, as they
 * do in the real map).
 *
 * TWO LAYOUTS, AND THE DIFFERENCE BETWEEN THEM IS A MEASUREMENT, NOT A STYLE.
 * SPREAD scatters bytes uniformly, which is what a real lap looks like: a
 * 1 MiB map at this occupancy puts almost every edge in a word of its own.
 * PACKED fills whole words, so the same number of edges occupies an eighth as
 * many words.
 *
 * The published cost model for this scan reads
 *
 *     cost ~= 0.5 ns x (words skimmed) + 5.1 ns x (bytes in non-zero words)
 *
 * and it was fitted on real runs where every non-zero word held about one set
 * byte -- so "bytes in non-zero words" was 8 x "non-zero words" in every
 * sample, and the fit cannot tell the two apart. The coefficient could as
 * easily be 41 ns per non-zero word, which is a completely different claim
 * about where the time goes and about what is worth optimising. PACKED breaks
 * the tie by holding set bytes fixed while dividing non-zero words by eight.
 *
 * PACKED also closes a correctness gap that SPREAD leaves open. The fast fold
 * walks the set bytes of a word with ctz64 in a do/while; at real occupancy
 * that loop almost never runs more than once, so the multi-byte path -- where
 * a mistake in clearing the consumed byte would hang or double-count -- is
 * barely exercised by a realistic map.
 */
typedef enum { COV_FILL_SPREAD, COV_FILL_PACKED } CovFillMode;

static uint64_t cov_check_fill(uint8_t *map, uint32_t map_size, uint32_t edges,
                               CovFillMode mode, uint64_t *seed)
{
    uint64_t set = 0;
    uint32_t i;

    memset(map, 0, map_size);
    if (mode == COV_FILL_PACKED) {
        uint32_t nwords = map_size / 8;
        uint32_t words = edges / 8;

        for (i = 0; i < words; i++) {
            uint64_t r = cov_check_rng(seed);
            uint8_t *w = map + (size_t)(r % nwords) * 8;
            int k;

            for (k = 0; k < 8; k++) {
                w[k] = (uint8_t)((cov_check_rng(seed) >> 32) % 255) + 1;
            }
        }
    } else {
        for (i = 0; i < edges; i++) {
            uint64_t r = cov_check_rng(seed);

            map[r % map_size] = (uint8_t)((r >> 32) % 255) + 1;
        }
    }
    for (i = 0; i < map_size; i++) {
        if (map[i]) {
            set++;
        }
    }
    return set;
}

bool fastsnap_cov_scan_selfcheck(uint32_t map_size, uint32_t edges,
                                 bool packed, bool saturate,
                                 unsigned reps,
                                 FastsnapCovScanCheck *out)
{
    /*
     * FIVE SLOTS, AND THE FIFTH IS A CONTROL. Each implementation gets its own
     * lap map and its own cumulative map, so none of them inherits another's
     * cache state, and they run in a fixed order inside one rep. That order
     * could itself be the result -- a 1 MiB buffer memcpy'd and then walked
     * four times over is not the same for the first walk as for the fourth --
     * so the shipped scan is timed TWICE, first and last. If those two agree,
     * the middle three mean what they say. If they do not, nothing here does,
     * and the caller is told the skew rather than left to read four numbers
     * that are really a story about cache.
     */
    enum { SLOT_SHIPPED, SLOT_CHUNKED, SLOT_BRANCHLESS, SLOT_CONTROL,
           N_SLOTS };
    static const char *const slot_name[N_SLOTS] = {
        "shipped", "cache-line skim", "branchless fold",
        "shipped (order control)",
    };
    size_t nwords = map_size / sizeof(uint64_t);
    uint8_t *pristine = NULL;
    uint8_t *map[N_SLOTS] = { NULL };
    uint8_t *virgin[N_SLOTS] = { NULL };
    int64_t slot_us[N_SLOTS] = { 0 };
    uint64_t seed = 0x5eed1234ULL;
    CovFillMode mode;
    bool ok = true;
    unsigned r;
    int slot, round;

    memset(out, 0, sizeof(*out));
    out->mismatch = "the differential check did not run";

    if (map_size < COV_MAP_SIZE_MIN || map_size > COV_MAP_SIZE_MAX ||
        (map_size & (map_size - 1))) {
        out->mismatch = "map size is not a power of two in range";
        return false;
    }
    mode = packed ? COV_FILL_PACKED : COV_FILL_SPREAD;

    pristine = g_try_malloc0(map_size);
    for (slot = 0; slot < N_SLOTS; slot++) {
        map[slot] = g_try_malloc0(map_size);
        virgin[slot] = g_try_malloc0(map_size);
        if (!map[slot] || !virgin[slot]) {
            ok = false;
        }
    }
    if (!pristine || !ok) {
        goto out_free;
    }

    /*
     * Seed the cumulative maps with a random history before the first round.
     * A virgin map that starts empty makes every edge new and never takes the
     * "already seen, different bucket" path -- the one branch here whose
     * answer a fuzzer's corpus decisions actually rest on, and the one the
     * branchless candidate has to get right without a branch.
     */
    if (saturate) {
        /*
         * ATTRIBUTION, USING THE SHIPPED CODE UNCHANGED. Every cumulative
         * byte already holds every bucket, so "is this edge new" and "is this
         * bucket new" are both always no -- the same loads, the same stores,
         * the same cache lines, but two branches that never mispredict.
         * Whatever this shape is faster by is what those branches cost. It is
         * not a configuration anything runs in: a saturated map finds nothing
         * by definition.
         */
        memset(virgin[0], 0xff, map_size);
    } else {
        cov_check_fill(virgin[0], map_size, edges ? edges : 1, mode, &seed);
        for (r = 0; r < map_size; r++) {
            virgin[0][r] = cov_bucket(virgin[0][r]);
        }
    }
    for (slot = 1; slot < N_SLOTS; slot++) {
        memcpy(virgin[slot], virgin[0], map_size);
    }

    /*
     * Two rounds with different maps and a SHARED, accumulating history. The
     * second round is the one where most edges are already known, so it is
     * the round that exercises new_buckets against a nonzero cumulative byte;
     * running only the first would leave that arithmetic unchecked.
     */
    for (round = 0; ok && round < 2; round++) {
        CovFold got[N_SLOTS];

        out->set_bytes = cov_check_fill(pristine, map_size, edges, mode,
                                        &seed);
        for (slot = 0; slot < N_SLOTS; slot++) {
            memcpy(map[slot], pristine, map_size);
        }

        got[SLOT_SHIPPED] = cov_fold((uint64_t *)map[SLOT_SHIPPED],
                                     (uint64_t *)virgin[SLOT_SHIPPED],
                                     nwords, true);
        got[SLOT_CHUNKED] = cov_fold_chunked((uint64_t *)map[SLOT_CHUNKED],
                                             (uint64_t *)virgin[SLOT_CHUNKED],
                                             nwords, true);
        got[SLOT_BRANCHLESS] =
            cov_fold_branchless((uint64_t *)map[SLOT_BRANCHLESS],
                                (uint64_t *)virgin[SLOT_BRANCHLESS],
                                nwords, true);
        got[SLOT_CONTROL] = cov_fold((uint64_t *)map[SLOT_CONTROL],
                                     (uint64_t *)virgin[SLOT_CONTROL],
                                     nwords, true);
        for (slot = SLOT_CHUNKED; slot <= SLOT_CONTROL; slot++) {
            if (memcmp(&got[SLOT_SHIPPED], &got[slot], sizeof(CovFold))) {
                out->mismatch = slot_name[slot];
                ok = false;
            } else if (memcmp(virgin[SLOT_SHIPPED], virgin[slot], map_size)) {
                out->mismatch = "the cumulative map (the counts agreed; the "
                                "bytes folded into it did not)";
                ok = false;
            } else if (memcmp(map[SLOT_SHIPPED], map[slot], map_size)) {
                out->mismatch = "the cleared lap map";
                ok = false;
            }
        }
        if (ok) {
            out->mismatch = NULL;
            out->edges = got[SLOT_SHIPPED].edges;
            out->new_edges = got[SLOT_SHIPPED].new_edges;
            out->new_buckets = got[SLOT_SHIPPED].new_buckets;
        }
    }

    for (r = 0; ok && r < reps; r++) {
        for (slot = 0; slot < N_SLOTS; slot++) {
            int64_t t0;

            memcpy(map[slot], pristine, map_size);
            t0 = g_get_monotonic_time();
            switch (slot) {
            case SLOT_CHUNKED:
                cov_fold_chunked((uint64_t *)map[slot],
                                 (uint64_t *)virgin[slot], nwords, true);
                break;
            case SLOT_BRANCHLESS:
                cov_fold_branchless((uint64_t *)map[slot],
                                    (uint64_t *)virgin[slot], nwords, true);
                break;
            default:
                cov_fold((uint64_t *)map[slot], (uint64_t *)virgin[slot],
                         nwords, true);
                break;
            }
            slot_us[slot] += g_get_monotonic_time() - t0;
        }
    }

    {
        size_t k;
        uint64_t *pw = (uint64_t *)pristine;

        out->nonzero_words = 0;
        for (k = 0; k < nwords; k++) {
            if (pw[k]) {
                out->nonzero_words++;
            }
        }
    }
    out->nwords = nwords;
    out->reps = reps;
    if (reps) {
        out->shipped_ns = (uint64_t)slot_us[SLOT_SHIPPED] * 1000 / reps;
        out->chunked_ns = (uint64_t)slot_us[SLOT_CHUNKED] * 1000 / reps;
        out->branchless_ns = (uint64_t)slot_us[SLOT_BRANCHLESS] * 1000 / reps;
        out->control_ns = (uint64_t)slot_us[SLOT_CONTROL] * 1000 / reps;
    }

out_free:
    if (!pristine) {
        out->mismatch = "allocation failed";
        ok = false;
    }
    g_free(pristine);
    for (slot = 0; slot < N_SLOTS; slot++) {
        if (!map[slot] || !virgin[slot]) {
            out->mismatch = "allocation failed";
            ok = false;
        }
        g_free(map[slot]);
        g_free(virgin[slot]);
    }
    return ok;
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
