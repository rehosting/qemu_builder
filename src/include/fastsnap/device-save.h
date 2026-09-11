/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Device state in a block: save every non-iterative savevm section into one
 * heap buffer, and restore from it, without touching disk or the RAM stream.
 *
 * Adopted from qemu-libafl-bridge (AFLplusplus), commit 4df4d2dcfa, whose base
 * was QEMU 9.1.1. Upstream carried no per-file licence header; the repo is
 * GPL-2.0. See PROVENANCE.md in this directory for the full declaration and
 * the list of changes.
 */
#ifndef FASTSNAP_DEVICE_SAVE_H
#define FASTSNAP_DEVICE_SAVE_H

#include "qemu/osdep.h"

#define DEVICE_SAVE_KIND_FULL 0

typedef struct DeviceSaveState {
    uint8_t kind;
    uint8_t *save_buffer;
    size_t save_buffer_size;
} DeviceSaveState;

typedef enum DeviceSnapshotKind {
    DEVICE_SNAPSHOT_ALL,
    DEVICE_SNAPSHOT_ALLOWLIST,
    DEVICE_SNAPSHOT_DENYLIST
} DeviceSnapshotKind;

/*
 * All four calls require the BQL. The save and restore calls additionally
 * require the vCPUs to be stopped -- they walk live device state.
 */
DeviceSaveState *device_save_all(void);
DeviceSaveState *device_save_kind(DeviceSnapshotKind kind, char **names);

void device_restore_all(DeviceSaveState *dss);
void device_free_all(DeviceSaveState *dss);

/*
 * NULL-terminated list of the section ids a device snapshot would cover.
 * The strings point into QEMU's own handler list and must not be freed or
 * outlive a device hot-unplug; g_free() the array itself.
 */
char **device_list_all(void);

bool fastsnap_devices_is_restoring(void);

#endif /* FASTSNAP_DEVICE_SAVE_H */
