/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The acceptance gate for the device-block port: does save/restore actually
 * move device state on this QEMU, on this machine?
 *
 * Runs at machine-init-done when FASTSNAP_SELFTEST is set in the environment,
 * then _exit()s with 0 (pass) or 1 (fail). Nothing here runs otherwise.
 *
 * THE POSITIVE CONTROL IS THE POINT. A device_restore_all() that silently
 * restored nothing would look exactly like a passing round-trip test, so the
 * sequence proves the instrument can see a difference before it reports one:
 *
 *   A = save()                       baseline
 *   perturb a device register        PL011 UARTIMSC -> s->int_enabled
 *   B = save()                       CONTROL: assert B != A
 *                                    (if this fails, save() is blind and every
 *                                     later comparison is meaningless)
 *   restore(A)
 *   C = save()                       assert C == A  AND  C != B
 *                                    (a no-op restore yields C == B, so the
 *                                     second assertion is what has teeth)
 *
 * The perturbation is board-specific -- PL011 at the arm `virt` UART0 base --
 * so the test reports SKIPPED rather than FAILED on a machine without one.
 * Treating "no pl011 here" as a failure would train people to ignore it.
 */
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/main-loop.h"
#include "system/runstate.h"
#include "system/system.h"
#include "system/address-spaces.h"
#include "exec/memattrs.h"
#include "hw/core/boards.h"

#include "fastsnap/device-save.h"
#include "fastsnap/penguin-fastsnap.h"

/* hw/arm/virt.c memmap: VIRT_UART0. PL011 UARTIMSC is at +0x38, and
 * int_enabled is a VMSTATE_UINT32 in vmstate_pl011, so a write here must show
 * up in the block. */
#define FASTSNAP_UART0_BASE  0x09000000ULL
#define FASTSNAP_PL011_IMSC  0x38
#define FASTSNAP_IMSC_VALUE  0x7ffU

static bool blocks_equal(DeviceSaveState *x, DeviceSaveState *y)
{
    return x->save_buffer_size == y->save_buffer_size &&
           memcmp(x->save_buffer, y->save_buffer, x->save_buffer_size) == 0;
}

/*
 * Phase 2: the scheduled path, and the property the whole design rests on.
 *
 * penguin_load_snapshot() goes through vm_stop(RUN_STATE_RESTORE_VM), and
 * accel/tcg/tcg-all.c turns that specific runstate into an unconditional
 * tb_flush -- which on this lane's target cost more than the restore itself,
 * and lands as post-restore throughput rather than as restore latency, so a
 * latency benchmark cannot see it.
 *
 * A device-only restore changes no RAM, so no translated block can go stale
 * and the flush is unnecessary rather than merely expensive. That is only true
 * as long as the restore never enters RUN_STATE_RESTORE_VM, so assert exactly
 * that, by watching the same transition tcg_vm_change_state() keys on. This
 * reads no TCG internals: tb_ctx lives in accel/tcg/tb-context.h, which is
 * accel-private, and reaching into it from here would be the layering
 * violation this port exists to avoid.
 */
static bool saw_restore_vm;
static bool phase2_done;
static int phase1_failures;
static VMChangeStateEntry *running_watch;
static void fastsnap_on_running(void *opaque, bool running, RunState state);

static void fastsnap_runstate_watch(void *opaque, bool running, RunState state)
{
    if (state == RUN_STATE_RESTORE_VM) {
        saw_restore_vm = true;
    }
}

static int fastsnap_await(uint64_t target_seq)
{
    /* Pump the main loop until the BH has run. Bounded so a wedge fails rather
     * than hangs a CI job. */
    for (int i = 0; i < 100000; i++) {
        if (penguin_fastsnap_seq() >= target_seq) {
            return 0;
        }
        main_loop_wait(true);
    }
    return -1;
}

static int fastsnap_selftest_scheduled(void)
{
    VMChangeStateEntry *watch;
    uint64_t seq;
    int failures = 0;

    /*
     * MUST run with the VM running. vm_stop() on an already-stopped VM returns
     * without notifying change-state handlers, so at machine-init-done the
     * watcher below can never fire and the assertion is inert -- it passes even
     * when a vm_stop(RUN_STATE_RESTORE_VM) is deliberately injected. That is
     * how this was caught: by its own negative control.
     */
    if (!runstate_is_running()) {
        printf("fastsnap: FAIL - scheduled phase ran with the VM stopped, "
               "where the no-tb_flush assertion cannot fire at all\n");
        return 1;
    }

    watch = qemu_add_vm_change_state_handler(fastsnap_runstate_watch, NULL);
    saw_restore_vm = false;

    seq = penguin_fastsnap_seq();
    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_TAKE);
    if (fastsnap_await(seq + 1) || penguin_fastsnap_last_rc() != 0) {
        printf("fastsnap: FAIL - scheduled take did not complete\n");
        failures++;
    } else {
        printf("fastsnap: scheduled take %" PRId64 " us, %" PRIu64 " bytes, "
               "%d sections\n",
               penguin_fastsnap_last_us(), penguin_fastsnap_block_size(),
               penguin_fastsnap_section_count());
    }

    seq = penguin_fastsnap_seq();
    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_RESTORE);
    if (fastsnap_await(seq + 1) || penguin_fastsnap_last_rc() != 0) {
        printf("fastsnap: FAIL - scheduled restore did not complete\n");
        failures++;
    } else {
        printf("fastsnap: scheduled restore %" PRId64 " us\n",
               penguin_fastsnap_last_us());
    }

    /* THE assertion. */
    if (saw_restore_vm) {
        printf("fastsnap: FAIL - the restore entered RUN_STATE_RESTORE_VM, "
               "which makes accel/tcg flush every translation block. The "
               "whole point of a device-only restore is that it does not.\n");
        failures++;
    } else {
        printf("fastsnap: no RUN_STATE_RESTORE_VM transition, so no tb_flush\n");
    }

    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_RELEASE);
    fastsnap_await(penguin_fastsnap_seq() + 1);
    qemu_del_vm_change_state_handler(watch);
    return failures;
}

static void fastsnap_selftest_run(Notifier *n, void *opaque)
{
    DeviceSaveState *a, *b, *c;
    uint32_t v = FASTSNAP_IMSC_VALUE;
    uint8_t *a_copy;
    size_t a_len;
    bool have_pl011 = false;
    int failures = 0;
    char **kept;
    int nkept = 0;

    if (!getenv("FASTSNAP_SELFTEST")) {
        return;
    }

    /* machine-init-done notifiers already run with the BQL held. */

    kept = device_list_all();
    for (nkept = 0; kept[nkept]; nkept++) {
        if (strstr(kept[nkept], "pl011")) {
            have_pl011 = true;
        }
    }
    /* If the iterative-handler predicate silently excluded nothing, the block
     * would contain RAM and be megabytes rather than kilobytes. */
    printf("fastsnap: %d device sections kept\n", nkept);
    g_free(kept);

    if (!have_pl011) {
        printf("fastsnap: SELFTEST SKIPPED - no pl011 section on machine '%s'; "
               "the perturbation this test uses does not exist here. Run it "
               "with -M virt on arm/aarch64.\n",
               current_machine ? MACHINE_GET_CLASS(current_machine)->name
                               : "(unknown)");
        fflush(stdout);
        _exit(0);
    }

    a = device_save_all();
    printf("fastsnap: A = %zu bytes\n", a->save_buffer_size);

    address_space_write(&address_space_memory,
                        FASTSNAP_UART0_BASE + FASTSNAP_PL011_IMSC,
                        MEMTXATTRS_UNSPECIFIED, &v, sizeof(v));

    b = device_save_all();
    printf("fastsnap: B = %zu bytes (after perturbation)\n",
           b->save_buffer_size);

    /* CONTROL. Everything after this is meaningless if it fails. */
    if (blocks_equal(a, b)) {
        printf("fastsnap: CONTROL FAILED - perturbation invisible in the "
               "device block. save() is not capturing device state, so no "
               "round-trip result below can be believed.\n");
        failures++;
    } else {
        printf("fastsnap: control OK - perturbation changed the block\n");
    }

    /* Compare against our own copy, so the round-trip verdict cannot be
     * produced by reading a buffer that restore freed. */
    a_len = a->save_buffer_size;
    a_copy = g_memdup2(a->save_buffer, a_len);

    device_restore_all(a);

    c = device_save_all();
    printf("fastsnap: C = %zu bytes (after restore of A)\n",
           c->save_buffer_size);

    if (!(c->save_buffer_size == a_len &&
          memcmp(c->save_buffer, a_copy, a_len) == 0)) {
        printf("fastsnap: FAIL - C != A, restore did not reproduce A\n");
        failures++;
    } else {
        printf("fastsnap: C == A\n");
    }
    if (blocks_equal(c, b)) {
        printf("fastsnap: FAIL - C == B, restore was a no-op\n");
        failures++;
    } else {
        printf("fastsnap: C != B, restore actually reverted the "
               "perturbation\n");
    }

    printf("fastsnap: phase 1 %s\n", failures ? "FAILED" : "passed");

    g_free(a_copy);
    device_free_all(a);
    device_free_all(b);
    device_free_all(c);
    g_free(a);
    g_free(b);
    g_free(c);

    fflush(stdout);

    /*
     * Hand off to phase 2, which needs the VM running. Recorded rather than
     * exited on, so a phase-1 failure still shows up in the final verdict.
     */
    phase1_failures = failures;
    running_watch = qemu_add_vm_change_state_handler(fastsnap_on_running, NULL);
}

static void fastsnap_on_running(void *opaque, bool running, RunState state)
{
    int failures;

    if (!running || phase2_done) {
        return;
    }
    phase2_done = true;

    failures = phase1_failures + fastsnap_selftest_scheduled();
    printf("fastsnap: SELFTEST %s\n", failures ? "FAILED" : "PASSED");
    fflush(stdout);
    /* _exit, not exit: returning through QEMU's atexit teardown from a
     * change-state handler segfaults, which would leave a PASS carrying a
     * core-dump exit status. */
    _exit(failures ? 1 : 0);
}

static Notifier fastsnap_selftest_notifier = {
    .notify = fastsnap_selftest_run,
};

static void __attribute__((constructor))
fastsnap_selftest_register(void)
{
    qemu_add_machine_init_done_notifier(&fastsnap_selftest_notifier);
}
