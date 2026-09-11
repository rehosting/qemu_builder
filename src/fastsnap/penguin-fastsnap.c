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

#include "fastsnap/device-save.h"
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
static int64_t fastsnap_last_us;
static uint64_t fastsnap_seq;
static int fastsnap_last_rc = -1;

typedef enum {
    FASTSNAP_OP_TAKE = PENGUIN_FASTSNAP_TAKE,
    FASTSNAP_OP_RESTORE = PENGUIN_FASTSNAP_RESTORE,
    FASTSNAP_OP_RELEASE = PENGUIN_FASTSNAP_RELEASE,
    FASTSNAP_OP_PROBE = PENGUIN_FASTSNAP_PROBE,
} FastsnapOp;

static uint64_t fastsnap_last_digest;
static uint64_t fastsnap_probe_digest(void);

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
        if (fastsnap_denylist) {
            fastsnap_slot = device_save_kind(DEVICE_SNAPSHOT_DENYLIST,
                                             fastsnap_denylist);
        } else {
            fastsnap_slot = device_save_all();
        }
        fastsnap_last_us = g_get_monotonic_time() - t0;
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
    uint64_t h = 1469598103934665603ULL;      /* FNV-1a 64 */
    size_t i;

    tmp = fastsnap_denylist
        ? device_save_kind(DEVICE_SNAPSHOT_DENYLIST, fastsnap_denylist)
        : device_save_all();
    if (!tmp) {
        return 0;
    }
    for (i = 0; i < tmp->save_buffer_size; i++) {
        h ^= tmp->save_buffer[i];
        h *= 1099511628211ULL;
    }
    device_free_all(tmp);
    g_free(tmp);
    return h;
}

/*
 * Comma-separated section ids to exclude from the block, or NULL/"" to clear.
 * Takes effect on the next take. Call before scheduling one; it touches only
 * this module's own state, so it does not need the main loop.
 */
void __attribute__((visibility("default")))
penguin_fastsnap_set_denylist(const char *csv)
{
    g_strfreev(fastsnap_denylist);
    fastsnap_denylist = NULL;

    if (csv && *csv) {
        fastsnap_denylist = g_strsplit(csv, ",", -1);
        /* g_strsplit keeps surrounding whitespace; device-save.c compares with
         * strcmp, so trim here rather than silently never matching. */
        for (char **p = fastsnap_denylist; *p; p++) {
            g_strstrip(*p);
        }
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
