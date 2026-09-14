/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Penguin-facing entry points for the device block: a C ABI the CFFI layer can
 * drive, and the main-loop scheduling that makes it safe to ask for one from a
 * vCPU thread.
 *
 * WHY THIS IS NOT JUST device_save_all() EXPORTED. Two reasons, and the second
 * is the whole point of the feature.
 *
 * 1. Context. the save and restore calls walk live device state, so they
 *    need the BQL held and the vCPUs stopped. A Penguin pyplugin callback runs
 *    on a vCPU thread inside a hypercall, which is neither. So the work is
 *    scheduled onto the main loop, exactly as penguin_schedule_snapshot() does
 *    for savevm/loadvm.
 *
 * 2. NOT vm_stop(). penguin_load_snapshot() calls vm_stop(RUN_STATE_RESTORE_VM),
 *    and accel/tcg/tcg-all.c turns that specific runstate into an unconditional
 *    tb_flush__exclusive_or_serial(). Upstream's comment there says why:
 *
 *        "loadvm will update the content of RAM, bypassing the usual
 *         mechanisms that ensure we flush TBs for writes to memory we've
 *         translated code from, so we must flush all TBs."
 *
 *    A device-only restore updates no RAM. No guest instruction bytes change,
 *    so no translated block can go stale, so the flush is not merely expensive
 *    here -- it is unnecessary by construction. Measured on this lane's target,
 *    post-restore re-translation was the LARGER half of a full restore's cost,
 *    and a restore-latency benchmark cannot see it at all because it lands as
 *    throughput afterwards rather than as latency during.
 *
 *    So this pauses the vCPUs with pause_all_vcpus(), which stops them without
 *    a runstate transition, and never enters RUN_STATE_RESTORE_VM. If a future
 *    version of this restores RAM as well, that reasoning expires with it and
 *    the flush has to come back.
 *
 * Timing is taken here rather than in Python: the operations are tens of
 * microseconds, and a pyplugin round trip is hundreds.
 */
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/error-report.h"
#include "qemu/aio.h"
#include "system/cpus.h"
#include "system/runstate.h"
#include "system/ramblock.h"
#include "system/ramlist.h"
#include "qemu/rcu.h"

#include "fastsnap/device-save.h"
#include "fastsnap/dirty-track.h"
#include "fastsnap/fork-oracle.h"
#include "fastsnap/ram-snapshot.h"
#include "fastsnap/penguin-fastsnap.h"

/*
 * One slot. A fuzzing loop wants take-once, restore-many; multiple concurrent
 * blocks would need an id-keyed table, and nothing asks for that yet.
 */
static DeviceSaveState *fastsnap_slot;

/*
 * Sections to leave OUT of the block, NULL-terminated, owned here.
 *
 * This is not a tuning knob, it is a correctness requirement, and only a real
 * firmware target shows why. A virtio device's state is split in two: the
 * device model holds last_avail_idx/used_idx, and the vring itself lives in
 * GUEST RAM. A device-only restore puts back the first half and leaves the
 * second at whatever the guest has since made of it, and virtio_load() is
 * strict enough to notice:
 *
 *     VQ 1 size 0x100 < last_avail_idx 0x9 - used_idx 0x11
 *     error while loading state for instance 0x0 of device
 *     '0000:00:01.0/virtio-net': Failed to load element of type virtio
 *
 * Measured on a booted firmware image; a synthetic -M virt machine with no
 * virtio-net never reaches it. Any device whose state is co-located with guest
 * RAM has the same problem, so the fix is not to make virtio tolerant -- it is
 * to keep those devices out of a block that does not carry the RAM they refer
 * to. That is the announced trade: the fast path gives up the network backend.
 */
static char **fastsnap_denylist;

/*
 * Sections to keep IN the block, NULL-terminated, owned here. Mutually
 * exclusive with the denylist; setting one clears the other.
 *
 * This is the performance lever, and it is the larger of the two halves of a
 * reset: measured, a full seventeen-section block restores in 0.752 ms and a
 * two-section {cpu, timer} block in 0.043 ms, against a RAM half of tens of
 * microseconds. It is also the dangerous one. A denylist is conservative by
 * construction -- a device nobody named still gets restored -- whereas an
 * allowlist silently drops every section the caller did not think of, and a
 * dropped section does not fail, it drifts.
 *
 * So it is paired with the per-section device reference below, which names
 * what a scoped reset failed to put back rather than leaving it to be
 * discovered as a guest that misbehaves three thousand iterations later.
 */
static char **fastsnap_allowlist;

/*
 * Per-section digests of the FULL device set at the last LOOP_ARM, and the
 * result of comparing them after a reset. See device_section_digests().
 */
static DeviceSectionDigest *fastsnap_dev_ref;
static int fastsnap_dev_ref_n;
static int fastsnap_dev_diff_n = -1;
static int fastsnap_dev_unrestorable_n = -1;
static char *fastsnap_dev_diff_names;

static int64_t fastsnap_last_us;
static int64_t fastsnap_diff_us;
static uint64_t fastsnap_seq;
static int64_t fastsnap_bh_done_us;
static int fastsnap_last_rc = -1;

typedef enum {
    FASTSNAP_OP_TAKE = PENGUIN_FASTSNAP_TAKE,
    FASTSNAP_OP_RESTORE = PENGUIN_FASTSNAP_RESTORE,
    FASTSNAP_OP_RELEASE = PENGUIN_FASTSNAP_RELEASE,
    FASTSNAP_OP_PROBE = PENGUIN_FASTSNAP_PROBE,
    FASTSNAP_OP_RESTORE_VERIFY = PENGUIN_FASTSNAP_RESTORE_VERIFY,
    FASTSNAP_OP_STATE_DIGEST = PENGUIN_FASTSNAP_STATE_DIGEST,
    FASTSNAP_OP_FORK_REF = PENGUIN_FASTSNAP_FORK_REF,
    FASTSNAP_OP_FORK_DIFF = PENGUIN_FASTSNAP_FORK_DIFF,
    FASTSNAP_OP_FORK_DROP = PENGUIN_FASTSNAP_FORK_DROP,
    FASTSNAP_OP_DIRTY_ARM = PENGUIN_FASTSNAP_DIRTY_ARM,
    FASTSNAP_OP_DIRTY_COUNT = PENGUIN_FASTSNAP_DIRTY_COUNT,
    FASTSNAP_OP_DIRTY_STOP = PENGUIN_FASTSNAP_DIRTY_STOP,
    FASTSNAP_OP_RAM_SNAPSHOT = PENGUIN_FASTSNAP_RAM_SNAPSHOT,
    FASTSNAP_OP_RAM_RESTORE = PENGUIN_FASTSNAP_RAM_RESTORE,
    FASTSNAP_OP_RAM_RELEASE = PENGUIN_FASTSNAP_RAM_RELEASE,
    FASTSNAP_OP_LOOP_ARM = PENGUIN_FASTSNAP_LOOP_ARM,
    FASTSNAP_OP_LOOP_RESET = PENGUIN_FASTSNAP_LOOP_RESET,
    FASTSNAP_OP_LOOP_RESET_VERIFY = PENGUIN_FASTSNAP_LOOP_RESET_VERIFY,
} FastsnapOp;

static uint64_t fastsnap_last_digest;
static uint64_t fastsnap_last_ram_digest;
static uint64_t fastsnap_probe_digest(void);
static uint64_t fastsnap_ram_digest(void);

/*
 * FNV-1a over 64-bit words rather than bytes.
 *
 * The byte-wise fastsnap_block_hash() above is right for the device block, which is
 * tens of kilobytes. Guest RAM is six orders of magnitude bigger: byte-at-a-time
 * over 256 MB is ~100 ms, which would make the oracle's own measurement the
 * dominant cost of using it. Same construction, eight bytes at a time.
 *
 * This answers "are these the same bytes" between two points, nothing more. It
 * is not a cryptographic hash and must not be used as one.
 */
static uint64_t fastsnap_hash64(const void *p, size_t n, uint64_t h)
{
    const uint64_t *w = p;
    const uint8_t *tail;
    size_t words = n / sizeof(uint64_t);
    size_t i;

    for (i = 0; i < words; i++) {
        h ^= w[i];
        h *= 1099511628211ULL;
    }
    tail = (const uint8_t *)p + words * sizeof(uint64_t);
    for (i = 0; i < n % sizeof(uint64_t); i++) {
        h ^= tail[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/*
 * Every RAM block, in list order. The block's idstr and length are mixed in as
 * well as its contents, so that a block appearing, disappearing or being
 * resized changes the digest rather than silently shifting the comparison.
 *
 * Blocks with no host mapping are skipped by name, not ignored: their identity
 * still enters the hash, so one becoming unmapped is visible.
 *
 * BQL is held and the vCPUs are stopped by the caller, so the contents cannot
 * move under us. The RCU read lock is still taken because the block list itself
 * is RCU-protected.
 */
static uint64_t fastsnap_ram_digest(void)
{
    RAMBlock *block;
    uint64_t h = 1469598103934665603ULL;

    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH(block) {
        uint64_t len = block->used_length;

        h = fastsnap_hash64(block->idstr, strlen(block->idstr), h);
        h = fastsnap_hash64(&len, sizeof(len), h);
        if (block->host) {
            h = fastsnap_hash64(block->host, block->used_length, h);
        }
    }
    return h;
}

/*
 * How a block is scoped, in one place.
 *
 * TAKE, LOOP_ARM and the probe digest all have to agree: a probe that hashed a
 * full block while the slot held a scoped one would report a difference on
 * every comparison and read as a failing restore.
 *
 * BQL held, vCPUs stopped by the caller.
 */
static DeviceSaveState *fastsnap_take_block(void)
{
    if (fastsnap_allowlist) {
        return device_save_kind(DEVICE_SNAPSHOT_ALLOWLIST, fastsnap_allowlist);
    }
    if (fastsnap_denylist) {
        return device_save_kind(DEVICE_SNAPSHOT_DENYLIST, fastsnap_denylist);
    }
    return device_save_all();
}

/* The scope fastsnap_take_block() would apply, as a (kind, names) pair. */
static DeviceSnapshotKind fastsnap_scope(char ***names_out)
{
    if (fastsnap_allowlist) {
        *names_out = fastsnap_allowlist;
        return DEVICE_SNAPSHOT_ALLOWLIST;
    }
    if (fastsnap_denylist) {
        *names_out = fastsnap_denylist;
        return DEVICE_SNAPSHOT_DENYLIST;
    }
    *names_out = NULL;
    return DEVICE_SNAPSHOT_ALL;
}

/*
 * Capture the full per-section reference, discarding any previous one.
 * Returns 0, or -1 if the walk could not complete -- in which case the
 * reference is cleared, so a later comparison reports "no reference" rather
 * than comparing against a partial one.
 */
static int fastsnap_dev_ref_take(void)
{
    g_free(fastsnap_dev_ref);
    fastsnap_dev_ref = device_section_digests(&fastsnap_dev_ref_n);
    if (!fastsnap_dev_ref) {
        fastsnap_dev_ref_n = 0;
        return -1;
    }
    return 0;
}

/*
 * Compare the device state as it is now against the reference, section by
 * section, and record how many differ and which.
 *
 * MATCHED BY POSITION, NOT BY NAME, and that is not a detail. Section ids are
 * not unique: -M virt registers two sections both called "pflash_cfi01",
 * distinguished only by an instance id that the handler-list accessor does not
 * expose. A name-keyed join matches the second reference entry against the
 * first current one, and then reports a difference on every comparison for a
 * machine where nothing changed -- which is how this function first behaved,
 * and it took a real device (one whose subsection genuinely can appear between
 * an arm and a reset) to make the false positive look like a true one.
 *
 * Both walks come from qemu_savevm_foreach_handler() over the same list, so
 * index i is the same handler in both unless the list itself changed. A
 * changed list is reported rather than worked around: a differing count, or a
 * differing name at the same index, is a hot-plug or hot-unplug between the arm
 * and the reset, and "the device set moved underneath us" is never the same
 * answer as "nothing differs".
 *
 * Sets the count to -1 if there is no reference or the walk failed, which
 * callers must not read as zero.
 */
static void fastsnap_dev_diff_compute(void)
{
    DeviceSectionDigest *now;
    DeviceSnapshotKind kind;
    char **scope_names;
    GString *names;
    int n_now, i, ndiff = 0, nunres = 0;

    g_free(fastsnap_dev_diff_names);
    fastsnap_dev_diff_names = NULL;
    fastsnap_dev_diff_n = -1;
    fastsnap_dev_unrestorable_n = -1;

    if (!fastsnap_dev_ref) {
        return;
    }
    now = device_section_digests(&n_now);
    if (!now) {
        return;
    }

    names = g_string_new(NULL);
    if (n_now != fastsnap_dev_ref_n) {
        g_string_printf(names, "!section-count %d->%d",
                        fastsnap_dev_ref_n, n_now);
        g_free(now);
        fastsnap_dev_diff_n = -1;
        fastsnap_dev_unrestorable_n = -1;
        fastsnap_dev_diff_names = g_string_free(names, FALSE);
        return;
    }

    kind = fastsnap_scope(&scope_names);

    for (i = 0; i < n_now; i++) {
        const char *why = NULL;
        bool in_block;

        if (strcmp(fastsnap_dev_ref[i].idstr, now[i].idstr)) {
            why = "!";          /* the list reordered under us */
        } else if (fastsnap_dev_ref[i].digest != now[i].digest ||
                   fastsnap_dev_ref[i].len != now[i].len) {
            /*
             * TWO DIFFERENT FINDINGS, and conflating them sent a real
             * derivation down a false trail.
             *
             * A section the block did NOT carry differs because the scope is
             * too narrow. That is the allowlist question, and adding the
             * section fixes it.
             *
             * A section the block DID carry, restored from it, and that still
             * does not serialise to the same bytes, is telling you something
             * else entirely: its save is not a pure function of its restorable
             * state. mc146818rtc is the worked example -- rtc_pre_save() calls
             * rtc_update_time(), which reads the live clock, and
             * rtc_post_load() re-derives its timers from the current clock, so
             * it CANNOT come back byte-identical however correct the restore
             * is. Reported as "unrestorable" it is a fact about the device;
             * reported as a scope miss it is an instruction to add a section
             * that is already there, which is what happened: a run added it,
             * the report did not change, and throughput halved.
             *
             * So: '*' means it was in the block. Counted separately, never
             * folded into the scope answer, and never silently forgiven --
             * a genuine restore bug lands in the same bucket and has to be
             * visible.
             */
            why = "";
        }
        if (why) {
            in_block = device_section_in_scope(now[i].idstr, kind,
                                               scope_names);
            /*
             * The index is part of the name in the report because the name
             * alone does not identify the section -- two entries can share it.
             */
            g_string_append_printf(names, "%s%s%s%s#%d", ndiff + nunres ? "," : "",
                                   why, in_block ? "*" : "", now[i].idstr, i);
            if (in_block) {
                nunres++;
            } else {
                ndiff++;
            }
        }
    }

    g_free(now);
    fastsnap_dev_diff_n = ndiff;
    fastsnap_dev_unrestorable_n = nunres;
    fastsnap_dev_diff_names = g_string_free(names, FALSE);
}

/* BQL held, vCPUs stopped by the caller. */
static int fastsnap_do(FastsnapOp op)
{
    int64_t t0;

    switch (op) {
    case FASTSNAP_OP_TAKE:
        if (fastsnap_slot) {
            device_free_all(fastsnap_slot);
            g_free(fastsnap_slot);
            fastsnap_slot = NULL;
        }
        t0 = g_get_monotonic_time();
        fastsnap_slot = fastsnap_take_block();
        fastsnap_last_us = g_get_monotonic_time() - t0;
        /* After the clock, so the digest is not billed to the take. This is
         * the "A" a RESTORE_VERIFY digest is compared against. */
        fastsnap_last_digest = fastsnap_slot
            ? fastsnap_block_hash(fastsnap_slot->save_buffer,
                            fastsnap_slot->save_buffer_size)
            : 0;
        return 0;

    case FASTSNAP_OP_RESTORE:
        if (!fastsnap_slot) {
            error_report("fastsnap: restore with no block taken");
            return -1;
        }
        t0 = g_get_monotonic_time();
        device_restore_all(fastsnap_slot);
        fastsnap_last_us = g_get_monotonic_time() - t0;
        return 0;

    case FASTSNAP_OP_RESTORE_VERIFY:
        if (!fastsnap_slot) {
            error_report("fastsnap: restore with no block taken");
            return -1;
        }
        t0 = g_get_monotonic_time();
        device_restore_all(fastsnap_slot);
        fastsnap_last_us = g_get_monotonic_time() - t0;
        /* Still in the same bottom half, vCPUs still stopped: nothing has
         * executed since the restore, so re-serialising now must reproduce
         * the block byte for byte if the restore was faithful. */
        fastsnap_last_digest = fastsnap_probe_digest();
        return fastsnap_last_digest ? 0 : -1;

    case FASTSNAP_OP_LOOP_ARM:
        t0 = g_get_monotonic_time();
        if (fastsnap_slot) {
            device_free_all(fastsnap_slot);
            g_free(fastsnap_slot);
            fastsnap_slot = NULL;
        }
        fastsnap_slot = fastsnap_take_block();
        if (!fastsnap_slot) {
            error_report("fastsnap: loop arm could not take a device block");
            return -1;
        }
        if (fastsnap_ram_snapshot_take() != 0) {
            return -1;
        }
        /* Same bottom half as the snapshot, so the reference and the snapshot
         * are the same instant. See the header comment. */
        if (fastsnap_fork_ref_take() != 0) {
            return -1;
        }
        fastsnap_last_us = g_get_monotonic_time() - t0;
        fastsnap_last_digest = fastsnap_block_hash(fastsnap_slot->save_buffer,
                                             fastsnap_slot->save_buffer_size);
        /*
         * After the clock: the per-section reference is a diagnostic the loop
         * reads on verification laps only, and an arm is paid for once. Its
         * failure is reported and does not fail the arm -- the reset is still
         * sound without it -- but it leaves the reference empty so a later
         * comparison says "no reference" instead of "nothing differed".
         */
        if (fastsnap_dev_ref_take() != 0) {
            error_report("fastsnap: could not take a per-section device "
                         "reference; device verification is unavailable for "
                         "this arm");
        }
        return 0;

    case FASTSNAP_OP_LOOP_RESET: {
        int64_t n;
        if (!fastsnap_slot) {
            error_report("fastsnap: loop reset with no device block");
            return -1;
        }
        t0 = g_get_monotonic_time();
        device_restore_all(fastsnap_slot);
        n = fastsnap_ram_restore();
        fastsnap_last_us = g_get_monotonic_time() - t0;
        return n < 0 ? -1 : 0;
    }

    case FASTSNAP_OP_LOOP_RESET_VERIFY: {
        int64_t n, d, t1;
        if (!fastsnap_slot) {
            error_report("fastsnap: loop reset with no device block");
            return -1;
        }
        t0 = g_get_monotonic_time();
        device_restore_all(fastsnap_slot);
        n = fastsnap_ram_restore();
        t1 = g_get_monotonic_time();
        fastsnap_last_us = t1 - t0;
        if (n < 0) {
            return -1;
        }
        /* Same bottom half, so the guest has not run since the reset and any
         * difference found here is the reset's, not the guest's. Both oracles
         * run here for that reason: RAM against the fork reference, devices
         * against the per-section reference. The device oracle is what makes an
         * allowlist checkable -- the RAM oracle cannot see a device section
         * that was never restored, only the guest damage it eventually causes.
         */
        d = fastsnap_fork_ref_diff();
        fastsnap_dev_diff_compute();
        fastsnap_diff_us = g_get_monotonic_time() - t1;
        return d < 0 ? -1 : 0;
    }

    case FASTSNAP_OP_RAM_SNAPSHOT:
        t0 = g_get_monotonic_time();
        if (fastsnap_ram_snapshot_take() != 0) {
            return -1;
        }
        fastsnap_last_us = g_get_monotonic_time() - t0;
        return 0;

    case FASTSNAP_OP_RAM_RESTORE: {
        int64_t n;
        t0 = g_get_monotonic_time();
        n = fastsnap_ram_restore();
        fastsnap_last_us = g_get_monotonic_time() - t0;
        return n < 0 ? -1 : 0;
    }

    case FASTSNAP_OP_RAM_RELEASE:
        fastsnap_ram_snapshot_release();
        fastsnap_last_us = 0;
        return 0;

    case FASTSNAP_OP_FORK_REF:
        t0 = g_get_monotonic_time();
        if (fastsnap_fork_ref_take() != 0) {
            return -1;
        }
        fastsnap_last_us = g_get_monotonic_time() - t0;
        return 0;

    case FASTSNAP_OP_FORK_DIFF: {
        int64_t d;
        t0 = g_get_monotonic_time();
        d = fastsnap_fork_ref_diff();
        fastsnap_last_us = g_get_monotonic_time() - t0;
        return d < 0 ? -1 : 0;
    }

    case FASTSNAP_OP_FORK_DROP:
        fastsnap_fork_ref_drop();
        fastsnap_last_us = 0;
        return 0;

    case FASTSNAP_OP_DIRTY_ARM:
        t0 = g_get_monotonic_time();
        if (fastsnap_dirty_arm() != 0) {
            return -1;
        }
        fastsnap_last_us = g_get_monotonic_time() - t0;
        return 0;

    case FASTSNAP_OP_DIRTY_COUNT: {
        int64_t n;
        t0 = g_get_monotonic_time();
        n = fastsnap_dirty_count();
        fastsnap_last_us = g_get_monotonic_time() - t0;
        return n < 0 ? -1 : 0;
    }

    case FASTSNAP_OP_DIRTY_STOP:
        fastsnap_dirty_stop();
        fastsnap_last_us = 0;
        return 0;

    case FASTSNAP_OP_STATE_DIGEST:
        t0 = g_get_monotonic_time();
        fastsnap_last_ram_digest = fastsnap_ram_digest();
        fastsnap_last_digest = fastsnap_probe_digest();
        fastsnap_last_us = g_get_monotonic_time() - t0;
        return fastsnap_last_digest ? 0 : -1;

    case FASTSNAP_OP_PROBE:
        t0 = g_get_monotonic_time();
        fastsnap_last_digest = fastsnap_probe_digest();
        fastsnap_last_us = g_get_monotonic_time() - t0;
        return fastsnap_last_digest ? 0 : -1;

    case FASTSNAP_OP_RELEASE:
        if (fastsnap_slot) {
            device_free_all(fastsnap_slot);
            g_free(fastsnap_slot);
            fastsnap_slot = NULL;
        }
        fastsnap_last_us = 0;
        return 0;
    }
    return -1;
}

static void fastsnap_bh(void *opaque)
{
    FastsnapOp op = (FastsnapOp)(intptr_t)opaque;

    /*
     * Stop the vCPUs WITHOUT a runstate change -- see the file comment. This is
     * the difference between a device restore and penguin_load_snapshot().
     */
    bool paused = !runstate_is_running();
    if (!paused) {
        pause_all_vcpus();
    }

    fastsnap_last_rc = fastsnap_do(op);

    if (!paused) {
        resume_all_vcpus();
    }
    /*
     * Stamped before the sequence number is published, so a caller that sees
     * the bump can always read a timestamp that belongs to that operation.
     */
    fastsnap_bh_done_us = g_get_monotonic_time();
    fastsnap_seq++;
}

/*
 * Fire-and-forget from any thread, including a vCPU inside a hypercall.
 * Completion is observable through penguin_fastsnap_seq(); the result of the
 * operation through penguin_fastsnap_last_rc() and the accessors below.
 */
/*
 * Hash the device state AS IT IS RIGHT NOW, without touching the saved slot.
 *
 * THE POSITIVE CONTROL, and the reason it has to exist. On a synthetic machine
 * the selftest perturbs a known PL011 register and checks the block changed. On
 * real firmware there is no such register to reach for, so the only way to know
 * a restore did anything is to watch the device state itself move:
 *
 *     A = probe()          take a block
 *     ... guest runs ...
 *     B = probe()          assert B != A, or the probe is blind
 *     restore(block)
 *     C = probe()          assert C == A  AND  C != B
 *
 * Without this a device_restore_all() that silently restored nothing looks
 * exactly like a correct one: same duration, same absence of a re-translation
 * cliff, same verdict. It is the difference between measuring a mechanism and
 * measuring its absence.
 *
 * Returns a 64-bit hash of the block, or 0 if one cannot be taken. Honours the
 * denylist, so it compares like with like. BQL + stopped vCPUs, so it runs from
 * the same bottom half as the rest.
 */
static uint64_t fastsnap_probe_digest(void)
{
    DeviceSaveState *tmp;
    uint64_t h;

    tmp = fastsnap_take_block();
    if (!tmp) {
        return 0;
    }
    h = fastsnap_block_hash(tmp->save_buffer, tmp->save_buffer_size);
    device_free_all(tmp);
    g_free(tmp);
    return h;
}

/*
 * Comma-separated section ids to exclude from the block, or NULL/"" to clear.
 * Takes effect on the next take. Call before scheduling one; it touches only
 * this module's own state, so it does not need the main loop.
 */
static char **fastsnap_split(const char *csv)
{
    char **v;

    if (!csv || !*csv) {
        return NULL;
    }
    v = g_strsplit(csv, ",", -1);
    /* g_strsplit keeps surrounding whitespace; device-save.c compares with
     * strcmp, so trim here rather than silently never matching. */
    for (char **p = v; *p; p++) {
        g_strstrip(*p);
    }
    return v;
}

void __attribute__((visibility("default")))
penguin_fastsnap_set_denylist(const char *csv)
{
    g_strfreev(fastsnap_denylist);
    fastsnap_denylist = fastsnap_split(csv);
    if (fastsnap_denylist && fastsnap_allowlist) {
        /*
         * Both would mean two different answers to "is this section in the
         * block", and device-save.c takes exactly one kind. Clearing the other
         * is the only behaviour that cannot be misread: leaving both set and
         * picking a precedence would make the scoping depend on which setter
         * was called last in a way nothing reports.
         */
        g_strfreev(fastsnap_allowlist);
        fastsnap_allowlist = NULL;
    }
}

/*
 * Comma-separated section ids to keep IN the block, or NULL/"" to clear.
 * Clears any denylist. Takes effect on the next take.
 *
 * Read penguin_fastsnap_section_names() first: an id that matches nothing is
 * not an error here, and an allowlist of entirely mistyped names produces an
 * empty block that restores nothing, quickly.
 */
void __attribute__((visibility("default")))
penguin_fastsnap_set_allowlist(const char *csv)
{
    g_strfreev(fastsnap_allowlist);
    fastsnap_allowlist = fastsnap_split(csv);
    if (fastsnap_allowlist && fastsnap_denylist) {
        g_strfreev(fastsnap_denylist);
        fastsnap_denylist = NULL;
    }
}

/*
 * Newline-separated ids of every section a block would cover, so a caller can
 * see what is actually on this machine before choosing a denylist. Valid until
 * the next call.
 */
__attribute__((visibility("default")))
const char *penguin_fastsnap_section_names(void)
{
    static char *joined;
    char **list = device_list_all();

    g_free(joined);
    joined = g_strjoinv("\n", list);
    g_free(list);
    return joined;
}

void __attribute__((visibility("default")))
penguin_fastsnap_schedule(int op)
{
    aio_bh_schedule_oneshot(qemu_get_aio_context(), fastsnap_bh,
                            (void *)(intptr_t)op);
}

uint64_t __attribute__((visibility("default")))
penguin_fastsnap_seq(void)
{
    return fastsnap_seq;
}

int __attribute__((visibility("default")))
penguin_fastsnap_last_rc(void)
{
    return fastsnap_last_rc;
}

/* Duration of the last completed take or restore, in microseconds. */
int64_t __attribute__((visibility("default")))
penguin_fastsnap_last_us(void)
{
    return fastsnap_last_us;
}

/* Hash from the last PROBE. Compare across probes; the value itself is not
 * stable across builds or machines. */
uint64_t __attribute__((visibility("default")))
penguin_fastsnap_last_digest(void)
{
    return fastsnap_last_digest;
}

uint64_t __attribute__((visibility("default")))
penguin_fastsnap_last_ram_digest(void)
{
    return fastsnap_last_ram_digest;
}

uint64_t __attribute__((visibility("default")))
penguin_fastsnap_diff_pages(void)
{
    return fastsnap_fork_diff_pages();
}

/* Cost of the last fork-oracle comparison, kept out of last_us(): the diff
 * reads the whole of guest RAM back through process_vm_readv() and is tens of
 * milliseconds, while the reset it checks is hundreds of microseconds. A loop
 * pays for the reset every iteration and for the oracle only when it asks. */
int64_t __attribute__((visibility("default")))
penguin_fastsnap_diff_us(void)
{
    return fastsnap_diff_us;
}

uint64_t __attribute__((visibility("default")))
penguin_fastsnap_ram_restored_pages(void)
{
    return fastsnap_ram_restored_pages();
}

uint64_t __attribute__((visibility("default")))
penguin_fastsnap_ram_snapshot_bytes(void)
{
    return fastsnap_ram_snapshot_bytes();
}

/*
 * Reset cost, in work rather than in wall clock.
 *
 * Every attempt in this lane to explain where a lap goes has inferred
 * translation work from timings and been wrong three times in a row -- the
 * device half, the memcpy, and the invalidation were each convicted and then
 * acquitted. These are the counts themselves. Cumulative since process start;
 * the caller takes deltas around a reset and around a lap.
 */
uint64_t __attribute__((visibility("default")))
penguin_fastsnap_tb_flush_count(void)
{
    return fastsnap_tcg_stat(FASTSNAP_TCG_TB_FLUSH);
}

uint64_t __attribute__((visibility("default")))
penguin_fastsnap_tb_invalidate_count(void)
{
    return fastsnap_tcg_stat(FASTSNAP_TCG_TB_PHYS_INVALIDATE);
}

uint64_t __attribute__((visibility("default")))
penguin_fastsnap_tlb_full_flush_count(void)
{
    return fastsnap_tcg_stat(FASTSNAP_TCG_TLB_FULL_FLUSH);
}

uint64_t __attribute__((visibility("default")))
penguin_fastsnap_tlb_part_flush_count(void)
{
    return fastsnap_tcg_stat(FASTSNAP_TCG_TLB_PART_FLUSH);
}

uint64_t __attribute__((visibility("default")))
penguin_fastsnap_tlb_elide_flush_count(void)
{
    return fastsnap_tcg_stat(FASTSNAP_TCG_TLB_ELIDE_FLUSH);
}

uint64_t __attribute__((visibility("default")))
penguin_fastsnap_ram_pages_unchanged(void)
{
    return fastsnap_ram_pages_unchanged();
}

uint64_t __attribute__((visibility("default")))
penguin_fastsnap_ram_pages_invalidated(void)
{
    return fastsnap_ram_pages_invalidated();
}

uint64_t __attribute__((visibility("default")))
penguin_fastsnap_ram_pages_skipped_nocode(void)
{
    return fastsnap_ram_pages_skipped_nocode();
}

uint64_t __attribute__((visibility("default")))
penguin_fastsnap_diff_bytes_checked(void)
{
    return fastsnap_fork_bytes_checked();
}

const char * __attribute__((visibility("default")))
penguin_fastsnap_diff_report(void)
{
    return fastsnap_fork_report();
}

/* HOW the last comparison reached its answer, not just what it was.
 *
 * `proved` pages were shown equal by PFN identity -- parent and child still
 * share the frame, which the kernel guarantees means identical bytes -- and
 * were never read. `read` pages went through process_vm_readv and memcmp.
 * "281 MB byte-identical" means something different when 99.97% of it was
 * proven rather than compared, and a verdict that quotes the first number
 * without the second is hiding the mechanism that produced it.
 *
 * `pagemap_status` is 1 active, 0 off by env, -1 unavailable (PFNs read as
 * zero without CAP_SYS_ADMIN). -1 with the filter asked for is the case worth
 * surfacing: the oracle then costs slightly MORE and nothing else changes. */
uint64_t __attribute__((visibility("default")))
penguin_fastsnap_diff_pages_proved(void)
{
    return fastsnap_fork_pages_proved();
}

uint64_t __attribute__((visibility("default")))
penguin_fastsnap_diff_pages_read(void)
{
    return fastsnap_fork_pages_read();
}

int __attribute__((visibility("default")))
penguin_fastsnap_diff_pagemap_status(void)
{
    return fastsnap_fork_pagemap_status();
}

uint64_t __attribute__((visibility("default")))
penguin_fastsnap_dirty_pages(void)
{
    return fastsnap_dirty_pages();
}

uint64_t __attribute__((visibility("default")))
penguin_fastsnap_dirty_pages_scanned(void)
{
    return fastsnap_dirty_pages_scanned();
}

uint64_t __attribute__((visibility("default")))
penguin_fastsnap_dirty_page_size(void)
{
    return fastsnap_dirty_page_size();
}

const char * __attribute__((visibility("default")))
penguin_fastsnap_dirty_report(void)
{
    return fastsnap_dirty_report();
}

const char * __attribute__((visibility("default")))
penguin_fastsnap_dirty_blocks(void)
{
    return fastsnap_dirty_blocks();
}

uint64_t __attribute__((visibility("default")))
penguin_fastsnap_block_size(void)
{
    return fastsnap_slot ? fastsnap_slot->save_buffer_size : 0;
}

/* Number of sections a device block would cover on this machine. */
int __attribute__((visibility("default")))
penguin_fastsnap_section_count(void)
{
    char **list = device_list_all();
    int n = 0;

    while (list[n]) {
        n++;
    }
    g_free(list);
    return n;
}

/*
 * How many device sections differ from the reference taken at the last
 * LOOP_ARM, as of the last LOOP_RESET_VERIFY.
 *
 * Zero means the reset put every section back -- including the ones an
 * allowlist excluded from the block, which is the interesting case: a section
 * the guest never touches does not need restoring. Anything above zero names
 * what a scoped reset is giving up, and -1 means the comparison could not be
 * made (no reference, or the walk failed). -1 must not be read as zero.
 */
int __attribute__((visibility("default")))
penguin_fastsnap_dev_diff_sections(void)
{
    return fastsnap_dev_diff_n;
}

/*
 * Comma-separated ids of the differing sections. A leading '-' marks a section
 * that was in the reference and is gone now, '+' one that appeared. Empty when
 * nothing differs. Valid until the next LOOP_RESET_VERIFY.
 */
const char * __attribute__((visibility("default")))
penguin_fastsnap_dev_diff_report(void)
{
    return fastsnap_dev_diff_names ? fastsnap_dev_diff_names : "";
}

/*
 * When the last bottom half finished, on CLOCK_MONOTONIC in microseconds.
 *
 * This splits a loop iteration at the only place a caller cannot see. An
 * iteration measured from Python spans: schedule the reset, wait for the main
 * loop to pick the bottom half up, run it, resume the guest, and wait for the
 * guest to reach the next detector hit that polls for completion. last_us()
 * reports only the middle of that -- the operation itself -- so everything
 * else is one undifferentiated remainder, and on this lane's target that
 * remainder is two thirds of an ordinary iteration and 99% of one that ends in
 * a crash.
 *
 * With this, the remainder splits in two:
 *
 *     scheduled -> bh_done_us    main-loop latency plus the operation
 *     bh_done_us -> observed     guest execution plus the caller's own cost
 *
 * which is the difference between "the reset is slow" and "the round trip is
 * slow", and those have opposite fixes.
 *
 * g_get_monotonic_time() is clock_gettime(CLOCK_MONOTONIC) and so is Python's
 * time.clock_gettime(time.CLOCK_MONOTONIC), same epoch, so the two can be
 * subtracted. time.perf_counter() is the same clock on Linux but is not
 * documented to share an epoch with anything; callers should use the explicit
 * form.
 */
int64_t __attribute__((visibility("default")))
penguin_fastsnap_bh_done_us(void)
{
    return fastsnap_bh_done_us;
}

/*
 * Sections that WERE in the block, were restored from it, and still do not
 * serialise to the reference's bytes.
 *
 * Kept apart from dev_diff_sections() because the two license opposite
 * actions. A scope miss is fixed by widening the scope. This is not: either
 * the device's save is not a pure function of its restorable state -- see the
 * mc146818rtc case in fastsnap_dev_diff_compute() -- or the restore is
 * genuinely broken for that device. Neither is fixed by adding it to an
 * allowlist it is already in.
 *
 * -1 when no comparison could be made, which is not zero.
 */
int __attribute__((visibility("default")))
penguin_fastsnap_dev_unrestorable_sections(void)
{
    return fastsnap_dev_unrestorable_n;
}
