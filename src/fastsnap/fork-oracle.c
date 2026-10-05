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

/* /proc/<pid>/pagemap entry bits, from Documentation/admin-guide/mm/pagemap. */
#define PM_PRESENT      (1ULL << 63)
#define PM_SWAPPED      (1ULL << 62)
#define PM_PFN_MASK     ((1ULL << 55) - 1)

static pid_t fastsnap_ref_pid = -1;
static uint64_t fastsnap_ref_diff_pages;
static uint64_t fastsnap_ref_bytes_checked;
static uint64_t fastsnap_ref_pages_proved;
static uint64_t fastsnap_ref_pages_read;
static int fastsnap_ref_pagemap_status = FASTSNAP_PAGEMAP_OFF;
static char fastsnap_ref_report[512];

uint64_t fastsnap_fork_diff_pages(void) { return fastsnap_ref_diff_pages; }
uint64_t fastsnap_fork_bytes_checked(void) { return fastsnap_ref_bytes_checked; }
uint64_t fastsnap_fork_pages_proved(void) { return fastsnap_ref_pages_proved; }
uint64_t fastsnap_fork_pages_read(void) { return fastsnap_ref_pages_read; }
int fastsnap_fork_pagemap_status(void) { return fastsnap_ref_pagemap_status; }
const char *fastsnap_fork_report(void) { return fastsnap_ref_report; }

/*
 * THE ONLY SOUND WAY TO READ LESS.
 *
 * The diff below is memory-bandwidth bound: 281 MB through process_vm_readv()
 * plus a memcmp, 61 ms, 4.6 GB/s, and 34% of a measured loop's wall clock from
 * 0.5% of its laps. There is no constant factor left in it. The only lever is
 * to compare fewer pages, and the only admissible bound on what CAN differ is
 * one that does not come from the restore's own bookkeeping -- because that
 * bookkeeping is precisely what this file exists to not trust. QEMU's dirty
 * bitmap is therefore disqualified: a bug in it would hide itself.
 *
 * The kernel supplies one. Parent and child share physical frames until
 * copy-on-write breaks, and two mappings on the same PFN are byte-identical by
 * guarantee. So: equal PFN -> proven equal, skip. Different PFN -> might still
 * be equal, so compare it.
 *
 * THE DIRECTION OF THE ERROR IS THE WHOLE SAFETY ARGUMENT. Every uncertainty
 * -- a page not present, swapped, a PFN that reads zero, a short read of
 * pagemap -- resolves to "compare it". The filter can only ever do extra work.
 * It cannot cause a differing page to be reported as equal, because it never
 * skips on anything but proof.
 *
 * Measured on the shipped image against a 281 MB mapping with a forked child:
 * 18.1x at 24 divergent pages, 17.8x at 500, 6.0x at 5,000, 2.0x at 20,000,
 * reporting the same diff count as the full comparison every time.
 *
 * PFNs read as zero without CAP_SYS_ADMIN, which is the reason for the status
 * accessor rather than a quiet fallback: the failure mode of an instrument
 * that reads as working while doing nothing is the one this lane keeps
 * building checks against. The status is reported, and the caller is expected
 * to put it where a result is read.
 */
static bool fastsnap_pagemap_enabled(void)
{
    const char *e = getenv("FASTSNAP_FORK_PAGEMAP");
    /* Default ON. It cannot change an answer -- only how many pages have to
     * be read to reach it -- so the conservative default is the fast one,
     * and the env var exists to take it out of the picture when bisecting. */
    return !(e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N'));
}

static int fastsnap_pagemap_open(pid_t pid)
{
    char path[64];

    if (pid == 0) {
        snprintf(path, sizeof(path), "/proc/self/pagemap");
    } else {
        snprintf(path, sizeof(path), "/proc/%d/pagemap", (int)pid);
    }
    return open(path, O_RDONLY | O_CLOEXEC);
}

/* Fill `out` with npages entries for the mapping starting at `addr`.
 * False on any short or failed read: the caller then compares everything. */
static bool fastsnap_pagemap_read(int fd, const void *addr, uint64_t *out,
                                  size_t npages)
{
    off_t off = (off_t)(((uintptr_t)addr / FASTSNAP_PAGE) * sizeof(uint64_t));
    size_t want = npages * sizeof(uint64_t), done = 0;

    while (done < want) {
        ssize_t r = pread(fd, (char *)out + done, want - done,
                          off + (off_t)done);
        if (r <= 0) {
            return false;
        }
        done += (size_t)r;
    }
    return true;
}

/* Proof of equality, or nothing. Anything short of two present, unswapped,
 * non-zero, equal PFNs means "compare it". */
static inline bool fastsnap_pages_proven_equal(uint64_t a, uint64_t b)
{
    if (!(a & PM_PRESENT) || !(b & PM_PRESENT)) {
        return false;
    }
    if ((a & PM_SWAPPED) || (b & PM_SWAPPED)) {
        return false;
    }
    if ((a & PM_PFN_MASK) == 0 || (b & PM_PFN_MASK) == 0) {
        return false;   /* no CAP_SYS_ADMIN, or genuinely no frame */
    }
    return (a & PM_PFN_MASK) == (b & PM_PFN_MASK);
}

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
    uint64_t diffs = 0, checked = 0, proved = 0, pages_read = 0;
    int nreported = 0;
    size_t report_used = 0;
    bool read_failed = false;
    int pm_self = -1, pm_ref = -1;
    uint64_t *pm_a = NULL, *pm_b = NULL;
    size_t pm_cap = 0;

    if (fastsnap_ref_pid <= 0) {
        error_report("fastsnap: diff with no fork reference taken");
        return -1;
    }

    buf = g_malloc(FASTSNAP_DIFF_CHUNK);
    fastsnap_ref_report[0] = '\0';
    fastsnap_ref_pages_proved = 0;
    fastsnap_ref_pages_read = 0;

    if (fastsnap_pagemap_enabled()) {
        pm_self = fastsnap_pagemap_open(0);
        pm_ref = fastsnap_pagemap_open(fastsnap_ref_pid);
        if (pm_self < 0 || pm_ref < 0) {
            fastsnap_ref_pagemap_status = FASTSNAP_PAGEMAP_UNAVAILABLE;
        } else {
            pm_cap = FASTSNAP_DIFF_CHUNK / FASTSNAP_PAGE;
            pm_a = g_new(uint64_t, pm_cap);
            pm_b = g_new(uint64_t, pm_cap);
            fastsnap_ref_pagemap_status = FASTSNAP_PAGEMAP_ACTIVE;
        }
    } else {
        fastsnap_ref_pagemap_status = FASTSNAP_PAGEMAP_OFF;
    }

    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH(block) {
        uint64_t off;

        if (!block->host) {
            continue;
        }

        for (off = 0; off < block->used_length; off += FASTSNAP_DIFF_CHUNK) {
            size_t len = MIN(FASTSNAP_DIFF_CHUNK, block->used_length - off);
            size_t npages = (len + FASTSNAP_PAGE - 1) / FASTSNAP_PAGE;
            bool have_pm = false;
            bool any_candidate = true;
            size_t p;

            /*
             * The prefilter, per chunk. Two pagemap reads of at most 2 KB
             * each stand in for a 1 MB process_vm_readv, and a chunk with no
             * candidate page is skipped without any read at all -- which on a
             * loop that dirties 24 pages is nearly every chunk.
             */
            if (pm_a && npages <= pm_cap) {
                have_pm = fastsnap_pagemap_read(pm_self, block->host + off,
                                                pm_a, npages) &&
                          fastsnap_pagemap_read(pm_ref, block->host + off,
                                                pm_b, npages);
                if (have_pm) {
                    size_t i, prove = 0;
                    for (i = 0; i < npages; i++) {
                        if (fastsnap_pages_proven_equal(pm_a[i], pm_b[i])) {
                            prove++;
                        }
                    }
                    any_candidate = (prove < npages);
                    if (!any_candidate) {
                        /* Every page in this chunk is proven equal. It is
                         * CHECKED -- by a kernel guarantee rather than by a
                         * memcmp -- so it counts toward bytes_checked, and
                         * proved records by which means. */
                        checked += len;
                        proved += npages;
                        continue;
                    }
                }
            }

            struct iovec local = { .iov_base = buf, .iov_len = len };
            struct iovec remote = { .iov_base = block->host + off,
                                    .iov_len = len };
            ssize_t got = process_vm_readv(fastsnap_ref_pid, &local, 1,
                                           &remote, 1, 0);

            if (got != (ssize_t)len) {
                if (!read_failed) {
                    error_report("fastsnap: could not read reference memory at "
                                 "%s+0x%" PRIx64 ": %s", block->idstr, off,
                                 got < 0 ? strerror(errno) : "short read");
                    read_failed = true;
                }
                continue;
            }
            pages_read += npages;

            for (p = 0; p < len; p += FASTSNAP_PAGE) {
                size_t plen = MIN((size_t)FASTSNAP_PAGE, len - p);

                checked += plen;
                if (have_pm &&
                    fastsnap_pages_proven_equal(pm_a[p / FASTSNAP_PAGE],
                                                pm_b[p / FASTSNAP_PAGE])) {
                    proved++;
                    continue;
                }
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
    g_free(pm_a);
    g_free(pm_b);
    if (pm_self >= 0) {
        close(pm_self);
    }
    if (pm_ref >= 0) {
        close(pm_ref);
    }

    if (read_failed) {
        return -1;
    }

    /*
     * ACTIVE BUT PROVING NOTHING IS NOT ACTIVE. Without CAP_SYS_ADMIN every
     * PFN reads zero, every page falls through to the memcmp, and the only
     * visible symptom is that the oracle costs slightly MORE than before.
     * Say so, once, rather than leaving the cost to be noticed.
     */
    if (fastsnap_ref_pagemap_status == FASTSNAP_PAGEMAP_ACTIVE && proved == 0 &&
        checked > 0) {
        static bool said;
        fastsnap_ref_pagemap_status = FASTSNAP_PAGEMAP_UNAVAILABLE;
        if (!said) {
            said = true;
            warn_report("fastsnap: the pagemap prefilter proved nothing on a "
                        "full comparison, so every PFN read as zero. That is "
                        "CAP_SYS_ADMIN missing, not an absence of sharing; the "
                        "oracle is doing its full %" PRIu64 " MB read and "
                        "paying for the pagemap on top. Run with "
                        "--extra_docker_args \"--cap-add=SYS_ADMIN\" or set "
                        "FASTSNAP_FORK_PAGEMAP=0.", checked >> 20);
        }
    }

    fastsnap_ref_diff_pages = diffs;
    fastsnap_ref_bytes_checked = checked;
    fastsnap_ref_pages_proved = proved;
    fastsnap_ref_pages_read = pages_read;
    return (int64_t)diffs;
}
