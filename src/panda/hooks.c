/*
 * The engine's side of the PANDA component boundary: the hook table the PANDA runtime
 * (libpanda-<target>.so) installs when it is loaded. Nothing here runs unless a runtime is loaded.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "panda/hooks.h"
#include "panda-abi.h"

const PandaHooks *panda_hooks;
bool panda_hooks_nochain;

const char *panda_hooks_abi(void)
{
    return PANDA_ABI_HASH;
}

bool panda_hooks_install(const PandaHooks *hooks, const char *abi, Error **errp)
{
    if (qatomic_read(&panda_hooks)) {
        error_setg(errp, "a PANDA runtime is already loaded into this engine");
        return false;
    }
    if (hooks->version != PANDA_HOOKS_VERSION || hooks->size != sizeof(PandaHooks)) {
        error_setg(errp, "PANDA runtime hook table version %u (%u bytes), this engine's is "
                   "version %u (%zu bytes)", hooks->version, hooks->size,
                   PANDA_HOOKS_VERSION, sizeof(PandaHooks));
        return false;
    }
    if (!abi || strcmp(abi, PANDA_ABI_HASH)) {
        const char *ign = getenv("PANDA_MODULE_ABI");
        if (ign && !strcmp(ign, "ignore")) {
            warn_report("PANDA runtime built against panda ABI %.12s..., this engine is %.12s... "
                        "(PANDA_MODULE_ABI=ignore)", abi ? abi : "(none)", PANDA_ABI_HASH);
        } else {
            error_setg(errp, "PANDA runtime built against panda ABI %.12s..., this engine is "
                       "%.12s...", abi ? abi : "(none)", PANDA_ABI_HASH);
            error_append_hint(errp, "Use the libpanda built with this engine.\n");
            return false;
        }
    }
    qatomic_set(&panda_hooks, hooks);
    return true;
}
