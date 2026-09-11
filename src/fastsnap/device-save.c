/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Device state in a block.
 *
 * Adopted from qemu-libafl-bridge (AFLplusplus), commit 4df4d2dcfa, whose base
 * was QEMU 9.1.1. Upstream carried no per-file licence header; the repo is
 * GPL-2.0. See PROVENANCE.md in this directory for the change list.
 *
 * The one structural change worth reading here: upstream walks
 * savevm_state.handlers directly, which means it needs SaveStateEntry's and
 * SaveState's layout, which are private to migration/savevm.c. It gets them by
 * hoisting both structs into migration/savevm.h and deleting them from the .c.
 * That is a duplicated definition that must track upstream exactly or corrupt
 * memory silently, and it is the single most version-fragile thing a curated
 * patch series could carry. Here the handler list is reached through the
 * accessors added by the "migration: expose the savevm handler list" patch
 * instead: SaveStateEntry stays incomplete outside savevm.c, so nothing in
 * this file can go stale against a layout change.
 */
#include "qemu/osdep.h"

#include "migration/qemu-file.h"
#include "migration/savevm.h"
#include "migration/vmstate.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "qemu/error-report.h"

#include "fastsnap/channel-buffer-writeback.h"
#include "fastsnap/device-save.h"

/*
 * The entry points carry default visibility: the libqemu-system-*.so
 * libraries are built with hidden visibility for the CFFI boundary, and
 * these are meant to be driven from Penguin's Python side, the same way
 * system/penguin.c's ABI is.
 */
static bool fastsnap_restoring_devices;

bool __attribute__((visibility("default")))
fastsnap_devices_is_restoring(void)
{
    return fastsnap_restoring_devices;
}

static bool is_in_list(const char *str, char **list)
{
    while (*list) {
        if (!strcmp(str, *list)) {
            return true;
        }
        list++;
    }
    return false;
}

/*
 * Which sections belong in a device block.
 *
 * @iterative is the accessor's report of "this handler has a save_setup op",
 * which is the predicate 9.1.1's se->is_ram actually encoded -- the field name
 * was misleading even then. It is true for every live/iterative handler: ram,
 * dirty-bitmap, slirp, spapr/htab, vfio. Matching on idstr == "ram" instead
 * would be wrong, because it would pull the other live handlers into a block
 * that has no stream to iterate them over.
 *
 * "globalstate" is excluded because it carries the runstate string, and
 * restoring it mid-run would reinstate whatever runstate was current at save.
 */
static bool section_wanted(const char *idstr, bool iterative,
                           DeviceSnapshotKind kind, char **names)
{
    if (iterative || !strcmp(idstr, "globalstate")) {
        return false;
    }

    switch (kind) {
    case DEVICE_SNAPSHOT_ALLOWLIST:
        return is_in_list(idstr, names);
    case DEVICE_SNAPSHOT_DENYLIST:
        return !is_in_list(idstr, names);
    default:
        return true;
    }
}

typedef struct DeviceSaveCtx {
    QEMUFile *f;
    DeviceSnapshotKind kind;
    char **names;
} DeviceSaveCtx;

static int device_save_section(SaveStateEntry *se, const char *idstr,
                               bool iterative, void *opaque)
{
    DeviceSaveCtx *ctx = opaque;
    Error *err = NULL;
    int ret;

    if (!section_wanted(idstr, iterative, ctx->kind, ctx->names)) {
        return 0;
    }

    ret = qemu_savevm_save_one(ctx->f, se, &err);
    if (ret < 0) {
        error_reportf_err(err, "fastsnap: saving section '%s': ", idstr);
        return ret;
    }
    error_free(err);
    return 0;
}

/* BQL held, vCPUs stopped. */
DeviceSaveState * __attribute__((visibility("default")))
device_save_kind(DeviceSnapshotKind kind, char **names)
{
    DeviceSaveState *dss = g_new0(DeviceSaveState, 1);
    QIOChannelBufferWriteback *wbioc;
    DeviceSaveCtx ctx;
    QEMUFile *f;

    dss->kind = DEVICE_SAVE_KIND_FULL;
    dss->save_buffer = g_new(uint8_t, FASTSNAP_DEVICE_BLOCK_LIMIT);

    wbioc = qio_channel_buffer_writeback_new(FASTSNAP_DEVICE_BLOCK_LIMIT,
                                             dss->save_buffer,
                                             FASTSNAP_DEVICE_BLOCK_LIMIT,
                                             &dss->save_buffer_size);
    f = qemu_file_new_output(QIO_CHANNEL(wbioc));

    ctx = (DeviceSaveCtx){ .f = f, .kind = kind, .names = names };
    if (qemu_savevm_foreach_handler(device_save_section, &ctx) < 0) {
        /*
         * A partial block would restore a machine into a state no execution
         * ever produced. There is no useful recovery from here.
         */
        abort();
    }

    qemu_put_byte(f, QEMU_VM_EOF);
    qemu_fclose(f);

    return dss;
}

DeviceSaveState * __attribute__((visibility("default")))
device_save_all(void)
{
    return device_save_kind(DEVICE_SNAPSHOT_ALL, NULL);
}

/* BQL held, vCPUs stopped. */
void __attribute__((visibility("default")))
device_restore_all(DeviceSaveState *dss)
{
    bool saved_restoring = fastsnap_restoring_devices;
    QIOChannelBufferWriteback *bioc;
    Error *err = NULL;
    QEMUFile *f;

    assert(dss->save_buffer != NULL);

    /*
     * A borrowed, read-only view. Upstream reached for
     * qio_channel_buffer_new_external() in io/channel-buffer.c, whose
     * finalizer g_free()s the borrowed pointer -- freeing dss->save_buffer
     * out from under the caller and making the later device_free_all() a
     * double free. Reproduced as a SIGSEGV against qemu-libafl-bridge
     * @4df4d2dcfa; see PROVENANCE.md.
     */
    bioc = qio_channel_buffer_writeback_new_reader(dss->save_buffer,
                                                   dss->save_buffer_size);
    f = qemu_file_new_input(QIO_CHANNEL(bioc));

    fastsnap_restoring_devices = true;
    if (qemu_load_device_state(f, &err) < 0) {
        error_reportf_err(err, "fastsnap: restoring the device block: ");
        abort();
    }
    error_free(err);
    fastsnap_restoring_devices = saved_restoring;

    object_unref(OBJECT(bioc));
    qemu_fclose(f);
}

void __attribute__((visibility("default")))
device_free_all(DeviceSaveState *dss)
{
    g_free(dss->save_buffer);
    dss->save_buffer = NULL;
    dss->save_buffer_size = 0;
}

typedef struct DeviceListCtx {
    char **list;
    size_t n;
    size_t cap;
} DeviceListCtx;

static int device_list_section(SaveStateEntry *se, const char *idstr,
                               bool iterative, void *opaque)
{
    DeviceListCtx *ctx = opaque;

    if (!section_wanted(idstr, iterative, DEVICE_SNAPSHOT_ALL, NULL)) {
        return 0;
    }

    if (ctx->n + 1 >= ctx->cap) {
        ctx->cap = ctx->cap ? ctx->cap * 2 : 32;
        ctx->list = g_renew(char *, ctx->list, ctx->cap);
    }
    ctx->list[ctx->n++] = (char *)idstr;
    return 0;
}

char ** __attribute__((visibility("default")))
device_list_all(void)
{
    DeviceListCtx ctx = { 0 };

    qemu_savevm_foreach_handler(device_list_section, &ctx);

    if (!ctx.list) {
        ctx.list = g_new(char *, 1);
    }
    ctx.list[ctx.n] = NULL;
    return ctx.list;
}
