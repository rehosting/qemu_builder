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

/*
 * Per-section digests of the device state as it is RIGHT NOW.
 *
 * This exists to answer a question a whole-block digest cannot: a scoped
 * restore (allowlist or denylist) puts back only some sections, so "the block
 * came back" is true by construction and says nothing about the sections that
 * were left out. Digesting each section separately and comparing two such
 * arrays names the sections a stretch of guest execution actually moved, which
 * is the measurement a candidate allowlist has to be judged against.
 *
 * Always covers the FULL section set, whatever scoping the caller has
 * configured for its block -- a reference that shrank with the allowlist would
 * make every allowlist look sufficient.
 *
 * BQL held, vCPUs stopped. g_free() the returned array; the strings are
 * embedded in it.
 */
typedef struct DeviceSectionDigest {
    char idstr[256];
    uint64_t digest;
    uint64_t len;
} DeviceSectionDigest;

DeviceSectionDigest *device_section_digests(int *n_out);

/*
 * FNV-1a 64 over a byte range. Not a cryptographic hash -- it answers "are
 * these the same bytes", between two points in one process. Shared so that the
 * whole-block digest and the per-section digests cannot drift apart.
 */
uint64_t fastsnap_block_hash(const uint8_t *p, size_t n);

bool fastsnap_devices_is_restoring(void);

#endif /* FASTSNAP_DEVICE_SAVE_H */
