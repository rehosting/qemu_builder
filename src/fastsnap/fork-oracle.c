/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A reference copy of the guest that the fast path cannot have touched.
 *
 * Every instrument this lane built by instrumenting the fast path turned out
 * to be unable to fail -- a restore verified by re-serialising through the same
 * code that did the restore, a tb_flush assertion that ran from a state where
 * the transition could not occur, an A/B/C comparison whose C was unreachable
 * by construction. The fix is not a better assertion inside the mechanism. It
 * is a reference produced by something the mechanism does not participate in.
 *
 * fork() is that something. The kernel copies the address space; no code in
 * this file, in device-save.c, or in any future dirty-tracking scheme has a say
 * in whether the copy is faithful. The child then blocks every signal and parks
 * in pause() forever, so its copy stays exactly as the fork left it.
 *
 * The child never runs the guest, which is the design decision that makes this
 * affordable. fork() leaves the child single-threaded, so running a guest there
 * would mean recreating vCPU threads inside a process that may hold locks owned
 * by threads which no longer exist. Instead the parent reads the child's memory
 * back with process_vm_readv(). fork preserves the address-space layout, so a
 * RAMBlock is at the same host address in both, and the comparison is a plain
 * memcmp of two copies of the same guest page.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "system/ramblock.h"
#include "system/ramlist.h"
#include "qemu/rcu.h"

#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include "fastsnap/fork-oracle.h"

#define FASTSNAP_DIFF_CHUNK   (1024 * 1024)
#define FASTSNAP_PAGE         4096
#define FASTSNAP_REPORT_MAX   6

static pid_t fastsnap_ref_pid = -1;
static uint64_t fastsnap_ref_diff_pages;
static uint64_t fastsnap_ref_bytes_checked;
static char fastsnap_ref_report[512];

uint64_t fastsnap_fork_diff_pages(void) { return fastsnap_ref_diff_pages; }
uint64_t fastsnap_fork_bytes_checked(void) { return fastsnap_ref_bytes_checked; }
const char *fastsnap_fork_report(void) { return fastsnap_ref_report; }

/*
 * The condition the whole design rests on, checked rather than trusted.
 *
 * A copy-on-write reference requires the mapping to be private. A RAM block
 * created shared -- memory-backend-file with share=on, vhost-user, anything
 * behind RAM_SHARED -- is literally the same physical memory in both
 * processes. The "reference" would then follow the parent's live state, every
 * comparison would find zero differing pages, and the oracle would certify
 * whatever it was pointed at. That is the exact failure shape this file exists
 * to rule out, so it is a refusal, not a warning.
 */
static bool fastsnap_fork_ram_is_private(char **why)
{
    RAMBlock *block;

    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH(block) {
        if (!block->host) {
            continue;
        }
        if (block->flags & RAM_SHARED) {
            *why = g_strdup_printf("RAM block '%s' is shared (RAM_SHARED), so "
                                   "a forked child would see the parent's live "
                                   "memory rather than a copy", block->idstr);
            return false;
        }
    }
    return true;
}

/*
 * Guest RAM is MADV_DONTFORK, so by default a forked child does NOT inherit it.
 *
 * system/physmem.c marks every RAM block DONTFORK at creation over its
 * max_length, for KVM's benefit, and skips it only under qtest -- where the
 * comment reads "it may be forked (eg for fuzzing purposes)". Without undoing
 * that, the child's copy of guest RAM is simply absent and process_vm_readv()
 * returns EFAULT for every block.
 *
 * That is worth stating plainly because of how it would fail in a less careful
 * implementation: a diff that treated a failed read as "nothing differs" would
 * report a perfect reset, every time, having compared nothing at all. This one
 * returns -1 and says so, which is why the problem surfaced on the first run
 * rather than as a pile of false confidence.
 *
 * The window is narrow by construction: inheritance is turned on immediately
 * before fork() and off immediately after, so QEMU's own invariant is restored
 * for any later fork. The child keeps the mapping it was given.
 */
static int fastsnap_fork_set_inherit(bool on)
{
#ifdef MADV_DOFORK
    RAMBlock *block;
    int rc = 0;

    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH(block) {
        if (!block->host) {
            continue;
        }
        if (madvise(block->host, block->max_length,
                    on ? MADV_DOFORK : MADV_DONTFORK) != 0) {
            error_report("fastsnap: madvise(%s, %s) failed: %s", block->idstr,
                         on ? "DOFORK" : "DONTFORK", strerror(errno));
            rc = -1;
        }
    }
    return rc;
#else
    error_report("fastsnap: MADV_DOFORK unavailable; guest RAM cannot be "
                 "inherited by a forked reference on this platform");
    return on ? -1 : 0;
#endif
}

int fastsnap_fork_ref_take(void)
{
    char *why = NULL;
    pid_t pid;

    if (fastsnap_ref_pid > 0) {
        fastsnap_fork_ref_drop();
    }

    if (!fastsnap_fork_ram_is_private(&why)) {
        error_report("fastsnap: refusing to take a fork reference: %s", why);
        g_free(why);
        return -1;
    }

    fflush(NULL);   /* or the child inherits and later re-emits buffered output */

    if (fastsnap_fork_set_inherit(true) != 0) {
        fastsnap_fork_set_inherit(false);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        error_report("fastsnap: fork for reference failed: %s", strerror(errno));
        fastsnap_fork_set_inherit(false);
        return -1;
    }

    if (pid == 0) {
        /*
         * CHILD. Touch nothing.
         *
         * fork() from a multi-threaded QEMU leaves this process holding
         * whatever locks the other threads happened to own -- the malloc arena
         * lock above all. Anything that allocates, or that runs a QEMU signal
         * handler which allocates, can deadlock here and the parent would block
         * forever waiting on a reference that never parks.
         *
         * So: block every signal (sigfillset/sigprocmask are async-signal-safe
         * and allocate nothing), then pause() forever. With all signals masked
         * pause() never returns, and SIGKILL from fastsnap_fork_ref_drop() is
         * what ends this process.
         */
        sigset_t all;
        sigfillset(&all);
        sigprocmask(SIG_SETMASK, &all, NULL);
        for (;;) {
            pause();
        }
        _exit(0);
    }

    /* Parent: put QEMU's invariant back. The child keeps what it was given. */
    fastsnap_fork_set_inherit(false);

    fastsnap_ref_pid = pid;
    fastsnap_ref_diff_pages = 0;
    fastsnap_ref_bytes_checked = 0;
    fastsnap_ref_report[0] = '\0';
    return 0;
}

void fastsnap_fork_ref_drop(void)
{
    int status;

    if (fastsnap_ref_pid <= 0) {
        return;
    }
    kill(fastsnap_ref_pid, SIGKILL);
    waitpid(fastsnap_ref_pid, &status, 0);
    fastsnap_ref_pid = -1;
}

/*
 * Compare live guest RAM against the reference, page by page.
 *
 * Caller holds the BQL with the vCPUs stopped, so the live side cannot move
 * underneath the walk. process_vm_readv() is permitted here under the default
 * yama ptrace_scope=1 because the target is this process's own child; it needs
 * no elevated privilege and no relaxed sysctl.
 *
 * Returns the number of differing pages, or -1 if the reference could not be
 * read at all -- which is reported as an error rather than as "no differences",
 * because a read failure and a clean comparison must never look alike.
 */
int64_t fastsnap_fork_ref_diff(void)
{
    RAMBlock *block;
    uint8_t *buf;
    uint64_t diffs = 0, checked = 0;
    int nreported = 0;
    size_t report_used = 0;
    bool read_failed = false;

    if (fastsnap_ref_pid <= 0) {
        error_report("fastsnap: diff with no fork reference taken");
        return -1;
    }

    buf = g_malloc(FASTSNAP_DIFF_CHUNK);
    fastsnap_ref_report[0] = '\0';

    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH(block) {
        uint64_t off;

        if (!block->host) {
            continue;
        }

        for (off = 0; off < block->used_length; off += FASTSNAP_DIFF_CHUNK) {
            size_t len = MIN(FASTSNAP_DIFF_CHUNK, block->used_length - off);
            struct iovec local = { .iov_base = buf, .iov_len = len };
            struct iovec remote = { .iov_base = block->host + off,
                                    .iov_len = len };
            ssize_t got = process_vm_readv(fastsnap_ref_pid, &local, 1,
                                           &remote, 1, 0);
            size_t p;

            if (got != (ssize_t)len) {
                if (!read_failed) {
                    error_report("fastsnap: could not read reference memory at "
                                 "%s+0x%" PRIx64 ": %s", block->idstr, off,
                                 got < 0 ? strerror(errno) : "short read");
                    read_failed = true;
                }
                continue;
            }

            for (p = 0; p < len; p += FASTSNAP_PAGE) {
                size_t plen = MIN((size_t)FASTSNAP_PAGE, len - p);

                checked += plen;
                if (memcmp(buf + p, block->host + off + p, plen) == 0) {
                    continue;
                }
                diffs++;
                if (nreported < FASTSNAP_REPORT_MAX) {
                    int n = snprintf(fastsnap_ref_report + report_used,
                                     sizeof(fastsnap_ref_report) - report_used,
                                     "%s%s+0x%" PRIx64,
                                     report_used ? ", " : "",
                                     block->idstr, off + p);
                    if (n > 0 &&
                        report_used + n < sizeof(fastsnap_ref_report)) {
                        report_used += n;
                        nreported++;
                    }
                }
            }
        }
    }

    g_free(buf);

    if (read_failed) {
        return -1;
    }

    fastsnap_ref_diff_pages = diffs;
    fastsnap_ref_bytes_checked = checked;
    return (int64_t)diffs;
}
