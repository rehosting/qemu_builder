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
#include "system/runstate.h"
#include "system/system.h"
#include "system/address-spaces.h"
#include "exec/memattrs.h"
#include "hw/core/boards.h"

#include "fastsnap/device-save.h"

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

    printf("fastsnap: SELFTEST %s\n", failures ? "FAILED" : "PASSED");

    g_free(a_copy);
    device_free_all(a);
    device_free_all(b);
    device_free_all(c);
    g_free(a);
    g_free(b);
    g_free(c);

    fflush(stdout);
    /* _exit, not exit: returning through QEMU's atexit teardown from a
     * machine-init-done notifier segfaults, which would leave a PASS carrying
     * a core-dump exit status. */
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
