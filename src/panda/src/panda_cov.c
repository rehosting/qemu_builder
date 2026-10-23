/*
 * Edge coverage for fuzzing: an AFL-shaped hit-count map filled from translated code.
 *
 * Each block translated while coverage is on starts with the classic AFL edge update
 *     map[prev ^ cur]++;  prev = cur >> 1;
 * cur being the block's id, a translation-time constant, and prev a per-vCPU field of CPUState
 * (patch 0070). That is eight TCG ops per block execution and no helper call. The map is a byte
 * array of a power-of-two size (default 64 KiB, AFL's MAP_SIZE), so afl-fuzz, afl-showmap or a
 * pandare2 loop can read it as is; a caller may hand in its own map (AFL's shared memory).
 *
 * Block ids hash the block's ram_addr (tb_page_addr0: RAM block offset + offset of the pc), not
 * its virtual address. A CF_PCREL block is shared by every virtual address that maps its
 * physical page, and runs whatever address it was found at; the ram_addr is the one thing all of
 * those have in common, so the constant baked into the block is right for each of them. A
 * library's code then has the same ids in every process, and after a fast restore (same RAM
 * layout) the same ids as before. A block executed from outside RAM (no ram_addr) hashes its pc.
 *
 * Optional filter on the block's virtual start [lo, hi) (e.g. user space only, or one binary's
 * text): a block outside it gets no code and leaves prev alone. With CF_PCREL a block passes or
 * fails the filter on the address it was translated at, and is shared with other mappings of the
 * same page.
 *
 * Cost when off (the default): translator_loop tests panda_cov_enabled once per block it
 * translates; nothing is emitted, so generated code is identical to a build without this.
 * Turning coverage on or off flushes the TBs (through panda_do_flush_tb, before the next lookup)
 * so every block runs with or without the update.
 *
 * The map pointer and its size are baked into translated code, so they are fixed by the first
 * panda_cov_enable() for the rest of the process, and the map must stay valid that long.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "hw/core/cpu.h"
#include "exec/translation-block.h"
#include "tcg/tcg.h"
#include "tcg/tcg-op-common.h"
#include "panda/plugin.h"
#include "panda/panda_api.h"
#include "panda/hooks.h"

#define COV_DEFAULT_SIZE (1u << 16)
#define COV_MIN_SIZE     (1u << 8)
#define COV_MAX_SIZE     (1u << 24)

bool panda_cov_enabled;

static uint8_t *cov_map;
static uint32_t cov_size;
static uint32_t cov_mask;
static bool cov_own_map;
static uint64_t cov_lo, cov_hi;     /* hi == 0: no filter */
static uint64_t cov_tbs, cov_tbs_filtered, cov_enables;

/* splitmix64's finalizer: any fixed mix will do, as long as the same address gives the same id */
static uint32_t cov_hash(uint64_t a)
{
    a += 0x9e3779b97f4a7c15ULL;
    a = (a ^ (a >> 30)) * 0xbf58476d1ce4e5b9ULL;
    a = (a ^ (a >> 27)) * 0x94d049bb133111ebULL;
    return (uint32_t)(a ^ (a >> 31));
}

void panda_cov_gen(TranslationBlock *tb, uint64_t pc)
{
    tb_page_addr_t ra = tb_page_addr0(tb);
    uint32_t cur;
    intptr_t prev_off = offsetof(CPUState, panda_cov_prev) - sizeof(CPUState);
    TCGv_i32 prev, cnt;
    TCGv_ptr slot;

    if (cov_hi && (pc < cov_lo || pc >= cov_hi)) {
        qatomic_inc(&cov_tbs_filtered);
        return;
    }
    qatomic_inc(&cov_tbs);
    cur = cov_hash(ra != (tb_page_addr_t)-1 ? (uint64_t)ra : pc ^ (1ULL << 63)) & cov_mask;

    /* prev is always a masked id >> 1, so prev ^ cur is in the map: no runtime mask */
    prev = tcg_temp_new_i32();
    cnt = tcg_temp_new_i32();
    slot = tcg_temp_new_ptr();
    tcg_gen_ld_i32(prev, tcg_env, prev_off);
    tcg_gen_xori_i32(prev, prev, cur);
    tcg_gen_ext_i32_ptr(slot, prev);
    tcg_gen_add_ptr(slot, slot, tcg_constant_ptr(cov_map));
    tcg_gen_ld8u_i32(cnt, slot, 0);
    tcg_gen_addi_i32(cnt, cnt, 1);
    tcg_gen_st8_i32(cnt, slot, 0);
    tcg_gen_st_i32(tcg_constant_i32(cur >> 1), tcg_env, prev_off);
}

void panda_cov_reset_prev(void)
{
    CPUState *cs;

    CPU_FOREACH(cs) {
        qatomic_set(&cs->panda_cov_prev, 0);
    }
}

int panda_cov_enable(uint8_t *map, uint32_t size)
{
    if (size && (size < COV_MIN_SIZE || size > COV_MAX_SIZE || (size & (size - 1)))) {
        error_report("panda_cov_enable: size %u is not a power of two in [%u, %u]",
                     size, COV_MIN_SIZE, COV_MAX_SIZE);
        return -1;
    }
    if (cov_map) {
        if ((map && map != cov_map) || (size && size != cov_size)) {
            error_report("panda_cov_enable: the map is fixed by the first enable (%p, %u bytes)",
                         cov_map, cov_size);
            return -1;
        }
    } else {
        cov_size = size ? size : COV_DEFAULT_SIZE;
        cov_mask = cov_size - 1;
        cov_own_map = !map;
        cov_map = map ? map : g_malloc0(cov_size);
    }
    panda_cov_reset_prev();
    if (!panda_cov_enabled) {
        qatomic_set(&panda_cov_enabled, true);
        cov_enables++;
        panda_do_flush_tb();
    }
    return 0;
}

void panda_cov_disable(void)
{
    if (panda_cov_enabled) {
        qatomic_set(&panda_cov_enabled, false);
        panda_do_flush_tb();
    }
}

bool panda_cov_is_enabled(void)
{
    return panda_cov_enabled;
}

void panda_cov_set_range(uint64_t lo, uint64_t hi)
{
    if (lo == cov_lo && hi == cov_hi) {
        return;
    }
    cov_lo = lo;
    cov_hi = hi;
    if (panda_cov_enabled) {
        panda_do_flush_tb();
    }
}

uint8_t *panda_cov_map(void)
{
    return cov_map;
}

uint32_t panda_cov_map_size(void)
{
    return cov_size;
}

void panda_cov_reset(void)
{
    if (cov_map) {
        memset(cov_map, 0, cov_size);
    }
    panda_cov_reset_prev();
}

const char *panda_cov_info(void)
{
    static char buf[256];

    snprintf(buf, sizeof(buf),
             "enabled=%d size=%u own_map=%d lo=0x%" PRIx64 " hi=0x%" PRIx64
             " tbs=%" PRIu64 " tbs_filtered=%" PRIu64 " enables=%" PRIu64,
             panda_cov_enabled, cov_size, cov_own_map, cov_lo, cov_hi, cov_tbs,
             cov_tbs_filtered, cov_enables);
    return buf;
}
