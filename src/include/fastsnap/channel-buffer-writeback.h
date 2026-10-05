/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A QIOChannel over a plain memory buffer, with an optional write-back target.
 *
 * Adopted from qemu-libafl-bridge (AFLplusplus), commit 4df4d2dcfa, whose base
 * was QEMU 9.1.1. Upstream carried no per-file licence header; the repo is
 * GPL-2.0. See PROVENANCE.md in this directory.
 */
#ifndef FASTSNAP_CHANNEL_BUFFER_WRITEBACK_H
#define FASTSNAP_CHANNEL_BUFFER_WRITEBACK_H

#include "qemu/osdep.h"

#include "migration/qemu-file.h"
#include "io/channel.h"
#include "qom/object.h"

/*
 * Upper bound on one device block. Allocated per save; the pages are never
 * touched beyond the ~64 KB a real block occupies, so the cost is address
 * space rather than memory. Sized to survive a machine with an unusually
 * large device set rather than to be tight.
 */
#define FASTSNAP_DEVICE_BLOCK_LIMIT (32 * 1024 * 1024)

#define TYPE_QIO_CHANNEL_BUFFER_WRITEBACK "qio-channel-buffer-writeback"
OBJECT_DECLARE_SIMPLE_TYPE(QIOChannelBufferWriteback,
                           QIO_CHANNEL_BUFFER_WRITEBACK)

struct QIOChannelBufferWriteback {
    QIOChannel parent;

    size_t capacity;
    size_t usage;
    size_t offset;
    uint8_t *data;

    /* Copied out of @data on close/finalize. NULL disables write-back. */
    uint8_t *writeback_buf;
    size_t writeback_buf_capacity;
    size_t *writeback_buf_usage;

    bool internal_allocation;
};

/**
 * qio_channel_buffer_writeback_new:
 * @capacity: size of the channel's own scratch buffer, which it allocates
 * @writeback_buf: caller-owned buffer that the scratch buffer is copied into
 *                 when the channel is closed
 * @writeback_buf_capacity: size of @writeback_buf
 * @writeback_buf_usage: out-param receiving the number of bytes written back
 *
 * The write side. @writeback_buf must not be NULL.
 */
QIOChannelBufferWriteback *
qio_channel_buffer_writeback_new(size_t capacity, uint8_t *writeback_buf,
                                 size_t writeback_buf_capacity,
                                 size_t *writeback_buf_usage);

/**
 * qio_channel_buffer_writeback_new_external:
 * @buf: caller-owned buffer to read and write through; not freed by the channel
 * @capacity: total size of @buf
 * @usage: how much of @buf currently holds data
 *
 * As above, but wrapping a buffer the caller already owns.
 */
QIOChannelBufferWriteback *qio_channel_buffer_writeback_new_external(
    uint8_t *buf, size_t capacity, size_t usage, uint8_t *writeback_buf,
    size_t writeback_buf_capacity, size_t *writeback_buf_usage);

/**
 * qio_channel_buffer_writeback_new_reader:
 * @buf: caller-owned buffer to read from; not freed by the channel
 * @usage: how many bytes of @buf are readable
 *
 * fastsnap addition. A read-only view over a caller-owned buffer, with
 * write-back disabled. This is what lets the restore path work without any
 * patch to io/channel-buffer.c: qemu-libafl-bridge reached for its own
 * qio_channel_buffer_new_external() there, which is both an extra upstream
 * edit and a double free -- upstream's qio_channel_buffer_finalize() calls
 * g_free(ioc->data) unconditionally, so unref'ing a channel that borrowed the
 * caller's buffer frees a buffer the caller still owns. Keeping the borrowed
 * case inside this file removes the patch and the bug together.
 */
QIOChannelBufferWriteback *
qio_channel_buffer_writeback_new_reader(uint8_t *buf, size_t usage);

#endif /* FASTSNAP_CHANNEL_BUFFER_WRITEBACK_H */
