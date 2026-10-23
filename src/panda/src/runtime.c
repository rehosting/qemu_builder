/*
 * The PANDA runtime as a component: when libpanda-<target>.so is loaded into an engine (after the
 * engine, which it does not link: libqemu-system-<target>.so, or an executable that exports its
 * symbols), this constructor installs the runtime's hook table. Until then the engine's hook sites
 * do nothing.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "cpu.h"
#include "panda/callbacks/cb-support.h"
#include "panda/hooks.h"
#include "panda-abi.h"

/* The panda ABI hash this runtime compiled against, as a panda module (board, device) carries it. */
__attribute__((visibility("default")))
const char panda_module_abi[] = PANDA_ABI_HASH;

static const PandaHooks panda_runtime_hooks = {
    .version = PANDA_HOOKS_VERSION,
    .size = sizeof(PandaHooks),
    .before_block_exec = panda_callbacks_before_block_exec,
    .after_block_exec = panda_callbacks_after_block_exec,
    .before_block_translate = panda_callbacks_before_block_translate,
    .after_block_translate = panda_callbacks_after_block_translate,
    .before_handle_exception = panda_callbacks_before_handle_exception,
    .asid_changed = panda_callbacks_asid_changed,
    .guest_hypercall = panda_callbacks_guest_hypercall,
    .block_longjmp = panda_callbacks_block_longjmp,
    .before_find_fast = panda_callbacks_before_find_fast,
    .main_loop_wait = panda_callbacks_main_loop_wait,
    .after_loadvm = panda_callbacks_after_loadvm,
};

/* false: this runtime is not attached to the engine (panda_init refuses to start) */
bool panda_runtime_attached;

static void __attribute__((constructor)) panda_runtime_attach(void)
{
    Error *err = NULL;

    if (panda_hooks_install(&panda_runtime_hooks, PANDA_ABI_HASH, &err)) {
        panda_runtime_attached = true;
    } else {
        error_report_err(err);
    }
}
