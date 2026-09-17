/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Edge coverage for the fastsnap loop: an AFL-shaped bitmap filled by code
 * emitted into every translated block, with no guest-side instrumentation.
 *
 * WHY THIS EXISTS. The loop could already reset a guest 248 times a second on
 * real armel firmware and feed each lap a mutated request. What it could not
 * do was notice that an input had reached somewhere new, and without that the
 * number is a loop rate rather than a fuzzing rate -- the published peers it
 * gets compared against (Nyx ~17,000/s, FIRM-AFL) are all coverage-GUIDED, and
 * a rate measured without the feedback they include is not the same quantity.
 * This is the missing half of that comparison.
 *
 * WHY NOT A TCG PLUGIN. QEMU has a designed interface for exactly this
 * (qemu_plugin_register_vcpu_tb_trans_cb) and it IS built here -- the shipped
 * libqemu-system-armel.so exports 89 qemu_plugin_* symbols, because QEMU's
 * configure turns plugins on by default whenever a C++ compiler is present,
 * so configs/default.json not naming --enable-plugins does not mean they are
 * off. It is still the wrong tool, for two reasons that survive checking:
 *
 *   1. A plugin cannot see the reset. The map has to be summarised and
 *      cleared at the same instant guest RAM is rewound, inside the same
 *      bottom half -- see fastsnap_cov_lap_end(). Penguin embeds QEMU as a
 *      library and drives it over the CFFI ABI; a plugin is loaded by QEMU
 *      and has no channel back to the embedding process, so its clear would
 *      land microseconds either side of the reset and every lap's map would
 *      carry a tail of the previous lap.
 *
 *   2. The plugin API cannot express an edge. Its inline ops are exactly
 *      QEMU_PLUGIN_INLINE_ADD_U64 and QEMU_PLUGIN_INLINE_STORE_U64 against a
 *      fixed scoreboard entry (include/plugins/qemu-plugin.h) -- there is no
 *      indexed addressing, so map[prev ^ cur]++ is not expressible inline and
 *      has to become a per-block CALLBACK through the plugin dispatch. That is
 *      an indirect call per translated block against the eight inline TCG ops
 *      emitted below (before optimisation -- the pointer arithmetic may fold),
 *      on a lane whose whole subject is the cost of a lap.
 *
 * WHAT IS EMITTED, per instrumented block, is AFL's classic edge hash:
 *
 *      idx        = prev ^ cur           cur is a translate-time CONSTANT
 *      map[idx]  += 1                    one byte, wrapping
 *      prev       = cur >> 1
 *
 * `cur` is fixed when the block is translated and blocks are cached, so the
 * per-execution cost is a load, an xor, a byte load, an add, a byte store and
 * a store -- no call, no branch, no lookup.
 *
 * THREE SHARP EDGES, each of which reads as a healthy zero if ignored:
 *
 *   * INSTRUMENTATION IS BAKED IN AT TRANSLATION TIME. A block translated
 *     before arming carries no instrumentation and will never carry any,
 *     because it is cached. fastsnap_cov_arm() therefore queues a tb_flush;
 *     see the comment there for why it queues rather than calls.
 *
 *   * THE MAP POINTER IS BAKED IN TOO. It is materialised as a TCG constant
 *     inside every instrumented block, so freeing or reallocating the map
 *     while any block references it is a use-after-free in generated code.
 *     The map is therefore allocated once, at the first arm, and NEVER freed
 *     or resized. Changing the size needs a restart, which is a constraint
 *     worth having over a lifetime rule nobody can check.
 *
 *   * THE FILTER IS A VIRTUAL ADDRESS RANGE, and this is a full-system
 *     emulator, so it cannot distinguish two processes that share a range.
 *     Under the fastsnap loop the guest is one pinned workload and that is
 *     tolerable -- but "coverage looks empty" and "the range is wrong" are
 *     indistinguishable from the map alone, which is why
 *     fastsnap_cov_tbs_instrumented() and _filtered() exist. Check them
 *     before believing a zero.
 *
 * MULTI-vCPU. `prev` is one host-global word, as it is in AFL's QEMU mode, so
 * two vCPUs translating and executing concurrently interleave their edge
 * history and produce noise. It cannot produce an out-of-bounds access: `cur`
 * is masked into the map at translation time and `prev` is only ever written
 * as `cur >> 1`, so `prev ^ cur` is always inside a power-of-two map whatever
 * order the writes land in. Noise, not corruption.
 */
#ifndef FASTSNAP_COVERAGE_H
#define FASTSNAP_COVERAGE_H

#include "qemu/osdep.h"

/*
 * Emit the edge-logging ops for a block starting at @pc. Called from
 * translator_loop() with the TCG context live; a no-op unless armed and
 * unless @pc passes the filter. Emits nothing at all when filtered out, so a
 * filtered block costs nothing to execute -- the filter is a translate-time
 * decision, not a runtime branch.
 */
void fastsnap_cov_translate(uint64_t pc);

/*
 * Allocate the map if this is the first arm, zero it, and start instrumenting
 * newly translated blocks. Returns 0, or -1 if the map could not be allocated.
 * BQL held, vCPUs paused.
 */
int fastsnap_cov_arm(void);

/* Stop instrumenting newly translated blocks. The map is kept and stays
 * readable; already-translated blocks keep writing to it until they are
 * flushed. BQL held, vCPUs paused. */
void fastsnap_cov_disarm(void);

/*
 * Summarise the map into the per-lap counters, fold it into the cumulative
 * "seen ever" map, then zero it. One pass over the map does all three.
 *
 * This is the operation a fuzzing loop actually wants, and folding it into
 * the reset rather than exposing it as its own op is deliberate: a hooked
 * syscall on this lane costs 95.880 us of portal round trip against 1.161 us
 * unhooked, so an extra call per lap would cost more than the scan it asks
 * for. See fastsnap_cov_set_clear_on_reset().
 */
void fastsnap_cov_lap_end(void);

/* Summarise without clearing -- the COV_READ op. */
void fastsnap_cov_read(void);

/* Zero the per-lap map and the edge history, leaving the cumulative map. */
void fastsnap_cov_clear(void);

/* Whether LOOP_RESET should call fastsnap_cov_lap_end(). */
bool fastsnap_cov_clear_on_reset(void);

/*
 * The map index a block at @pc would log to -- the hash, masked.
 *
 * Exposed for the selftest, and it is worth saying why rather than leaving it
 * as a stray export. The guest-driven controls can only assert that coverage
 * responds to whatever the machine happens to execute, and on -M virt with no
 * kernel that is two blocks inside a nix sandbox. They cannot say anything
 * about how the hash behaves across a real text segment's worth of addresses,
 * which is the property a corpus actually depends on: a "hash" that collapsed
 * to (pc & mask) would pass every one of those controls and then alias every
 * page-aligned block onto sixteen slots.
 *
 * So the selftest asks the function directly over a synthetic address range.
 * That check needs no guest, no machine and no luck, and it is the only part
 * of this file whose strength does not vary with the host.
 */
uint32_t fastsnap_cov_block_index(uint64_t pc);

#endif /* FASTSNAP_COVERAGE_H */
