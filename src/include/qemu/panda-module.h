/*
 * panda-module.h: included (-include) into every file of a panda module,
 * i.e. an external board or device shared object built with the SDK.
 * It stamps the module with the panda ABI hash of the headers it compiled
 * against; module_load_path() compares it with the engine's.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef QEMU_PANDA_MODULE_H
#define QEMU_PANDA_MODULE_H

#include "panda-abi.h"

#ifndef BUILD_DSO
#error "a panda module is built with -DBUILD_DSO (use the SDK's cflags)"
#endif

/* Weak: every translation unit of the module defines it; one copy stays. */
__attribute__((weak, visibility("default")))
const char panda_module_abi[] = PANDA_ABI_HASH;

#endif
