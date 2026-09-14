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

#include "system/memory.h"
#include "fastsnap/channel-buffer-writeback.h"
#include "fastsnap/device-save.h"
#include "fastsnap/penguin-fastsnap.h"
#include "fastsnap/fork-oracle.h"
#include "fastsnap/ram-snapshot.h"

/* hw/arm/virt.c memmap: VIRT_UART0. PL011 UARTIMSC is at +0x38, and
 * int_enabled is a VMSTATE_UINT32 in vmstate_pl011, so a write here must show
 * up in the block. */
#define FASTSNAP_UART0_BASE  0x09000000ULL
#define FASTSNAP_PL011_IMSC  0x38
#define FASTSNAP_PAGEMAP_ACTIVE_ST 1
#define FASTSNAP_IMSC_VALUE  0x7ffU

/* hw/arm/virt.c memmap: VIRT_MEM. 1 MB in, clear of the board's own setup. */
#define FASTSNAP_RAM_BASE    0x40000000ULL
#define FASTSNAP_RAM_PROBE   (FASTSNAP_RAM_BASE + 0x100000ULL)

/*
 * Eight pages for the dirty-tracking control, 64 KB apart so each lands in a
 * distinct target page whatever the page size turns out to be, and 2 MB in so
 * they are clear of the single word phases 3 and 4 poke.
 */
#define FASTSNAP_DIRTY_POKES   8
#define FASTSNAP_DIRTY_BASE    (FASTSNAP_RAM_BASE + 0x200000ULL)
#define FASTSNAP_DIRTY_STRIDE  0x10000ULL

static bool blocks_equal(DeviceSaveState *x, DeviceSaveState *y)
{
    return x->save_buffer_size == y->save_buffer_size &&
           memcmp(x->save_buffer, y->save_buffer, x->save_buffer_size) == 0;
}

/*
 * Phase 2: the scheduled path, and the property the whole design rests on.
 *
 * penguin_load_snapshot() goes through vm_stop(RUN_STATE_RESTORE_VM), and
 * accel/tcg/tcg-all.c turns that specific runstate into an unconditional
 * tb_flush -- which on this lane's target cost more than the restore itself,
 * and lands as post-restore throughput rather than as restore latency, so a
 * latency benchmark cannot see it.
 *
 * A device-only restore changes no RAM, so no translated block can go stale
 * and the flush is unnecessary rather than merely expensive. That is only true
 * as long as the restore never enters RUN_STATE_RESTORE_VM, so assert exactly
 * that, by watching the same transition tcg_vm_change_state() keys on. This
 * reads no TCG internals: tb_ctx lives in accel/tcg/tb-context.h, which is
 * accel-private, and reaching into it from here would be the layering
 * violation this port exists to avoid.
 */
static bool saw_restore_vm;
static bool phase2_done;
static int phase1_failures;
static VMChangeStateEntry *running_watch;
static void fastsnap_on_running(void *opaque, bool running, RunState state);

static void fastsnap_runstate_watch(void *opaque, bool running, RunState state)
{
    if (state == RUN_STATE_RESTORE_VM) {
        saw_restore_vm = true;
    }
}

static int fastsnap_await(uint64_t target_seq)
{
    /* Pump the main loop until the BH has run. Bounded so a wedge fails rather
     * than hangs a CI job. */
    for (int i = 0; i < 100000; i++) {
        if (penguin_fastsnap_seq() >= target_seq) {
            return 0;
        }
        main_loop_wait(true);
    }
    return -1;
}

static int fastsnap_selftest_scheduled(void)
{
    VMChangeStateEntry *watch;
    uint64_t seq, take_digest;
    int failures = 0;

    /*
     * MUST run with the VM running. vm_stop() on an already-stopped VM returns
     * without notifying change-state handlers, so at machine-init-done the
     * watcher below can never fire and the assertion is inert -- it passes even
     * when a vm_stop(RUN_STATE_RESTORE_VM) is deliberately injected. That is
     * how this was caught: by its own negative control.
     */
    if (!runstate_is_running()) {
        printf("fastsnap: FAIL - scheduled phase ran with the VM stopped, "
               "where the no-tb_flush assertion cannot fire at all\n");
        return 1;
    }

    watch = qemu_add_vm_change_state_handler(fastsnap_runstate_watch, NULL);
    saw_restore_vm = false;

    seq = penguin_fastsnap_seq();
    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_TAKE);
    if (fastsnap_await(seq + 1) || penguin_fastsnap_last_rc() != 0) {
        printf("fastsnap: FAIL - scheduled take did not complete\n");
        failures++;
    } else {
        printf("fastsnap: scheduled take %" PRId64 " us, %" PRIu64 " bytes, "
               "%d sections\n",
               penguin_fastsnap_last_us(), penguin_fastsnap_block_size(),
               penguin_fastsnap_section_count());
    }
    take_digest = penguin_fastsnap_last_digest();

    /*
     * RESTORE_VERIFY rather than RESTORE, so this phase also gates the op the
     * real-firmware harness depends on. Phase 1 can compare device_save_all()
     * buffers directly because it runs with nothing executing; a caller on a
     * running VM cannot, because the earliest it can schedule a probe is from
     * a later guest event, by which time cpu and timer state have moved and no
     * digest can ever match. RESTORE_VERIFY re-serialises inside the same
     * bottom half, vCPUs still stopped, which is the only place the comparison
     * means anything.
     */
    seq = penguin_fastsnap_seq();
    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_RESTORE_VERIFY);
    if (fastsnap_await(seq + 1) || penguin_fastsnap_last_rc() != 0) {
        printf("fastsnap: FAIL - scheduled restore did not complete\n");
        failures++;
    } else {
        printf("fastsnap: scheduled restore %" PRId64 " us\n",
               penguin_fastsnap_last_us());
        if (!take_digest) {
            printf("fastsnap: FAIL - take reported no digest\n");
            failures++;
        } else if (penguin_fastsnap_last_digest() != take_digest) {
            printf("fastsnap: FAIL - re-serialising immediately after the "
                   "restore does not reproduce the block, so the restore is "
                   "not faithful (take %" PRIu64 ", after restore %" PRIu64
                   ")\n", take_digest, penguin_fastsnap_last_digest());
            failures++;
        } else {
            printf("fastsnap: restore reproduces the block byte for byte "
                   "(digest %" PRIu64 ")\n", take_digest);
        }
    }

    /* THE assertion. */
    if (saw_restore_vm) {
        printf("fastsnap: FAIL - the restore entered RUN_STATE_RESTORE_VM, "
               "which makes accel/tcg flush every translation block. The "
               "whole point of a device-only restore is that it does not.\n");
        failures++;
    } else {
        printf("fastsnap: no RUN_STATE_RESTORE_VM transition, so no tb_flush\n");
    }

    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_RELEASE);
    fastsnap_await(penguin_fastsnap_seq() + 1);
    qemu_del_vm_change_state_handler(watch);
    return failures;
}


/*
 * Phase 3: the whole-guest state digest, and its positive control.
 *
 * The digest exists to answer "did these two runs end in the same state", which
 * is the only question that can distinguish a real fuzzing finding from a guest
 * the reset quietly corrupted. crashes.yaml cannot: measured on real firmware,
 * a run with 98 lines of kernel panic, swap_dup and OOM-kill produced a
 * crashes.yaml identical in shape to a healthy run.
 *
 * So the digest has to be able to FAIL, and the failure has to be demonstrated
 * rather than assumed. A fastsnap_ram_digest() that hashed the block list but
 * never dereferenced host would return a stable value forever and look exactly
 * like a correct implementation. The perturbation below is what separates them:
 *
 *   D1 = digest
 *   write one word of guest RAM
 *   D2 = digest      CONTROL: assert D2 != D1, or the digest is blind to RAM
 *   put the word back
 *   D3 = digest      assert D3 == D1
 *
 * The device half must NOT move across any of this, which is a second control:
 * it shows the two halves are separately sourced rather than one value copied
 * into both accessors.
 */
static int fastsnap_selftest_state_digest(void)
{
    uint64_t ram_a, ram_b, ram_c, dev_a, dev_b;
    uint32_t saved = 0, poke = 0xdeadbeefU;
    uint64_t seq;
    int failures = 0;

    seq = penguin_fastsnap_seq();
    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_STATE_DIGEST);
    if (fastsnap_await(seq + 1) || penguin_fastsnap_last_rc() != 0) {
        printf("fastsnap: FAIL - state digest did not complete\n");
        return 1;
    }
    ram_a = penguin_fastsnap_last_ram_digest();
    dev_a = penguin_fastsnap_last_digest();
    printf("fastsnap: state digest %" PRId64 " us, ram %" PRIu64 "\n",
           penguin_fastsnap_last_us(), ram_a);

    address_space_read(&address_space_memory, FASTSNAP_RAM_PROBE,
                       MEMTXATTRS_UNSPECIFIED, &saved, sizeof(saved));
    address_space_write(&address_space_memory, FASTSNAP_RAM_PROBE,
                        MEMTXATTRS_UNSPECIFIED, &poke, sizeof(poke));

    seq = penguin_fastsnap_seq();
    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_STATE_DIGEST);
    fastsnap_await(seq + 1);
    ram_b = penguin_fastsnap_last_ram_digest();
    dev_b = penguin_fastsnap_last_digest();

    /* THE CONTROL. Everything the oracle would ever conclude rests on this. */
    if (ram_b == ram_a) {
        printf("fastsnap: CONTROL FAILED - one word of guest RAM changed and "
               "the state digest did not move, so it is not reading RAM. No "
               "comparison built on it can be believed.\n");
        failures++;
    } else {
        printf("fastsnap: control OK - state digest sees a RAM change\n");
    }
    if (dev_b != dev_a) {
        printf("fastsnap: FAIL - device digest moved when only RAM was "
               "touched, so the two halves are not separately sourced\n");
        failures++;
    }

    address_space_write(&address_space_memory, FASTSNAP_RAM_PROBE,
                        MEMTXATTRS_UNSPECIFIED, &saved, sizeof(saved));

    seq = penguin_fastsnap_seq();
    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_STATE_DIGEST);
    fastsnap_await(seq + 1);
    ram_c = penguin_fastsnap_last_ram_digest();

    if (ram_c != ram_a) {
        printf("fastsnap: FAIL - restoring the word did not restore the "
               "digest (%" PRIu64 " vs %" PRIu64 "), so it is not a pure "
               "function of guest state\n", ram_c, ram_a);
        failures++;
    } else {
        printf("fastsnap: state digest is reproducible\n");
    }

    return failures;
}


/*
 * Phase 4: the fork oracle, and the two ways it could lie.
 *
 * It could report differences that are not there, or -- far more dangerous --
 * report none because it is not really comparing anything. A diff that
 * process_vm_readv()'d nothing and returned 0, or a reference sharing memory
 * with the parent so the two sides are the same bytes by construction, would
 * both certify a completely broken reset. So:
 *
 *   ref = fork()
 *   diff -> d0     must be 0: nothing has changed yet
 *   poke one word of guest RAM
 *   diff -> d1     CONTROL: must EXCEED d0, or the oracle cannot see anything
 *   put the word back
 *   diff -> d2     must be 0 again: it does not report phantom differences
 *
 * d0 and d2 are what make d1 mean something. A diff hardwired to return 1
 * passes the control alone and fails these.
 */
static int fastsnap_selftest_fork_oracle(void)
{
    uint32_t saved = 0, poke = 0xfeedfaceU;
    uint64_t seq;
    int64_t d0, d1, d2;
    int failures = 0;

    seq = penguin_fastsnap_seq();
    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_FORK_REF);
    if (fastsnap_await(seq + 1) || penguin_fastsnap_last_rc() != 0) {
        printf("fastsnap: FAIL - could not take a fork reference\n");
        return 1;
    }
    printf("fastsnap: fork reference taken in %" PRId64 " us\n",
           penguin_fastsnap_last_us());

    seq = penguin_fastsnap_seq();
    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_FORK_DIFF);
    if (fastsnap_await(seq + 1) || penguin_fastsnap_last_rc() != 0) {
        printf("fastsnap: FAIL - fork diff could not read the reference\n");
        penguin_fastsnap_schedule(PENGUIN_FASTSNAP_FORK_DROP);
        fastsnap_await(penguin_fastsnap_seq() + 1);
        return 1;
    }
    d0 = (int64_t)penguin_fastsnap_diff_pages();
    printf("fastsnap: fork diff baseline %" PRId64 " pages differ, %"
           PRIu64 " bytes compared, %" PRId64 " us\n",
           d0, penguin_fastsnap_diff_bytes_checked(),
           penguin_fastsnap_last_us());

    if (penguin_fastsnap_diff_bytes_checked() == 0) {
        printf("fastsnap: FAIL - the diff compared zero bytes, so a verdict of "
               "'no differences' means nothing\n");
        failures++;
    }

    address_space_read(&address_space_memory, FASTSNAP_RAM_PROBE,
                       MEMTXATTRS_UNSPECIFIED, &saved, sizeof(saved));
    address_space_write(&address_space_memory, FASTSNAP_RAM_PROBE,
                        MEMTXATTRS_UNSPECIFIED, &poke, sizeof(poke));

    seq = penguin_fastsnap_seq();
    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_FORK_DIFF);
    fastsnap_await(seq + 1);
    d1 = (int64_t)penguin_fastsnap_diff_pages();

    /* THE CONTROL. */
    if (d1 <= d0) {
        printf("fastsnap: CONTROL FAILED - one word of guest RAM changed and "
               "the fork diff did not see more differing pages (%" PRId64
               " then %" PRId64 "). The reference is not an independent copy, "
               "so a clean diff proves nothing.\n", d0, d1);
        failures++;
    } else {
        printf("fastsnap: control OK - fork diff sees a RAM change (%" PRId64
               " -> %" PRId64 " pages, at %s)\n", d0, d1,
               penguin_fastsnap_diff_report());
    }

    address_space_write(&address_space_memory, FASTSNAP_RAM_PROBE,
                        MEMTXATTRS_UNSPECIFIED, &saved, sizeof(saved));

    seq = penguin_fastsnap_seq();
    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_FORK_DIFF);
    fastsnap_await(seq + 1);
    d2 = (int64_t)penguin_fastsnap_diff_pages();

    if (d2 != d0) {
        printf("fastsnap: FAIL - putting the word back left %" PRId64
               " differing pages, expected %" PRId64 ". The diff reports "
               "changes that are not there.\n", d2, d0);
        failures++;
    } else {
        printf("fastsnap: fork diff returns to baseline\n");
    }

    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_FORK_DROP);
    fastsnap_await(penguin_fastsnap_seq() + 1);
    return failures;
}


/* ------------------------------------------------------------------------
 * TEMPORARY INSTRUMENTATION -- profiling harness for device_save/restore.
 * Runs only when FASTSNAP_PROFILE is set in the environment. Delete this
 * block (and its one call site at the end of phase 1) when done.
 * ------------------------------------------------------------------------ */

static int64_t fs_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int fs_cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Per-sample = ns for ONE operation, obtained by timing @batch of them.
 * Returns the median; *p10/*p90 get the 10th/90th percentile. */
static int64_t fs_bench(void (*op)(void *), void *arg, int reps, int batch,
                        int64_t *p10, int64_t *p90)
{
    int64_t *v = g_new(int64_t, reps);
    int i, k;
    int64_t med;

    for (k = 0; k < batch * 3; k++) {      /* warm up */
        op(arg);
    }
    for (i = 0; i < reps; i++) {
        int64_t t0 = fs_now_ns();
        for (k = 0; k < batch; k++) {
            op(arg);
        }
        v[i] = (fs_now_ns() - t0) / batch;
    }
    qsort(v, reps, sizeof(int64_t), fs_cmp_i64);
    med = v[reps / 2];
    if (p10) *p10 = v[reps / 10];
    if (p90) *p90 = v[(reps * 9) / 10];
    g_free(v);
    return med;
}

static void fs_op_restore(void *arg)
{
    device_restore_all((DeviceSaveState *)arg);
}

typedef struct { DeviceSnapshotKind kind; char **names; } FsSaveArg;

static void fs_op_save(void *arg)
{
    FsSaveArg *sa = arg;
    DeviceSaveState *d = device_save_kind(sa->kind, sa->names);
    device_free_all(d);
    g_free(d);
}

/* The allocation half of device_save_kind(), on its own: the two 32 MB
 * buffers and the 32 KB QEMUFile, with no section walk at all. */
static void fs_op_alloc_only(void *arg)
{
    uint8_t *save_buffer = g_new(uint8_t, FASTSNAP_DEVICE_BLOCK_LIMIT);
    uint8_t *chan = g_new0(uint8_t, FASTSNAP_DEVICE_BLOCK_LIMIT);
    size_t usage = 63029;
    memcpy(save_buffer, chan, usage);   /* the finalizer's write-back */
    g_free(chan);
    g_free(save_buffer);
    (void)arg;
}


/* Micro-benchmarks that bound how much of a restore is the QEMUFile byte
 * reader rather than the vmstate walk. All three move the same block. */
typedef struct { uint8_t *buf; size_t len; uint8_t *dst; } FsRawArg;

static void fs_op_memcpy(void *arg)
{
    FsRawArg *r = arg;
    memcpy(r->dst, r->buf, r->len);
}

static void fs_op_getbyte(void *arg)
{
    FsRawArg *r = arg;
    QIOChannelBufferWriteback *bioc =
        qio_channel_buffer_writeback_new_reader(r->buf, r->len);
    QEMUFile *f = qemu_file_new_input(QIO_CHANNEL(bioc));
    size_t i;
    for (i = 0; i < r->len; i++) {
        r->dst[i] = qemu_get_byte(f);
    }
    object_unref(OBJECT(bioc));
    qemu_fclose(f);
}

static void fs_op_getbuffer(void *arg)
{
    FsRawArg *r = arg;
    QIOChannelBufferWriteback *bioc =
        qio_channel_buffer_writeback_new_reader(r->buf, r->len);
    QEMUFile *f = qemu_file_new_input(QIO_CHANNEL(bioc));
    qemu_get_buffer(f, r->dst, r->len);
    object_unref(OBJECT(bioc));
    qemu_fclose(f);
}

static char *fs_nomatch[] = { (char *)"__fastsnap_no_such_section__", NULL };

static void fs_op_restore_mrt(void *arg)
{
    memory_region_transaction_begin();
    device_restore_all((DeviceSaveState *)arg);
    memory_region_transaction_commit();
}

static void fs_op_mrt_only(void *arg)
{
    memory_region_transaction_begin();
    memory_region_transaction_commit();
    (void)arg;
}

static long fs_rss_kb(void)
{
    long v = 0;
    FILE *fp = fopen("/proc/self/statm", "r");
    long total = 0, res = 0;
    if (fp) {
        if (fscanf(fp, "%ld %ld", &total, &res) == 2) {
            v = res * (sysconf(_SC_PAGESIZE) / 1024);
        }
        fclose(fp);
    }
    return v;
}

/*
 * Modes, selected by FASTSNAP_PROFILE:
 *   list        -- print the section names, one per line, and exit
 *   all         -- restore_all / restore_empty / save_all / save_empty
 *   one:<i>     -- restore_empty then section <i> alone
 *   loo:<i>     -- restore_all then all-except-<i>
 *   save:<i>    -- save_empty then save of section <i> alone
 *   mrt         -- restore_all with and without a memory-region transaction
 *                  bracket, measured in both orders
 *   drift       -- restore_all median over successive rounds, with RSS
 * Each mode is meant to be run in a FRESH process: restore cost drifts
 * upward with the number of restores already done, so two measurements in
 * one process are not comparable.
 */
static void fastsnap_profile_run(void)
{
    const char *mode = getenv("FASTSNAP_PROFILE");
    int reps  = getenv("FASTSNAP_PROFILE_REPS")
                ? atoi(getenv("FASTSNAP_PROFILE_REPS")) : 41;
    int batch = getenv("FASTSNAP_PROFILE_BATCH")
                ? atoi(getenv("FASTSNAP_PROFILE_BATCH")) : 20;
    char **kept;
    int n, i, idx = -1;
    int64_t p10, p90, t;
    DeviceSaveState *all, *none;

    if (!mode) {
        return;
    }
    if (reps < 5) reps = 5;
    if (batch < 1) batch = 1;
    if (strchr(mode, ':')) {
        idx = atoi(strchr(mode, ':') + 1);
    }

    kept = device_list_all();
    for (n = 0; kept[n]; n++) {
        ;
    }

    if (!strcmp(mode, "list")) {
        for (i = 0; i < n; i++) {
            printf("fastsnap: PROFILE-LIST %d %s\n", i, kept[i]);
        }
        fflush(stdout);
        _exit(0);
    }

    all  = device_save_all();
    none = device_save_kind(DEVICE_SNAPSHOT_ALLOWLIST, fs_nomatch);

    printf("fastsnap: PROFILE mode=%s reps=%d batch=%d sections=%d "
           "all=%zu bytes empty=%zu bytes\n",
           mode, reps, batch, n, all->save_buffer_size,
           none->save_buffer_size);

    if (!strcmp(mode, "raw")) {
        FsRawArg r = { all->save_buffer, all->save_buffer_size,
                       g_new(uint8_t, all->save_buffer_size) };
        t = fs_bench(fs_op_memcpy, &r, reps, batch, &p10, &p90);
        printf("fastsnap: PROFILE-RAW memcpy      %8" PRId64 " ns (p10 %"
               PRId64 " p90 %" PRId64 ")\n", t, p10, p90);
        t = fs_bench(fs_op_getbuffer, &r, reps, batch, &p10, &p90);
        printf("fastsnap: PROFILE-RAW get_buffer  %8" PRId64 " ns (p10 %"
               PRId64 " p90 %" PRId64 ")\n", t, p10, p90);
        t = fs_bench(fs_op_getbyte, &r, reps, batch, &p10, &p90);
        printf("fastsnap: PROFILE-RAW get_byte x%zu %8" PRId64 " ns (p10 %"
               PRId64 " p90 %" PRId64 ")\n", r.len, t, p10, p90);
        t = fs_bench(fs_op_restore, all, reps, batch, &p10, &p90);
        printf("fastsnap: PROFILE-RAW restore_all %8" PRId64 " ns (p10 %"
               PRId64 " p90 %" PRId64 ")\n", t, p10, p90);
        fflush(stdout);
        _exit(0);
    }

    if (!strncmp(mode, "drift", 5)) {
        int rounds = getenv("FASTSNAP_PROFILE_ROUNDS")
                     ? atoi(getenv("FASTSNAP_PROFILE_ROUNDS")) : 12;
        int r;
        DeviceSaveState *sel = all;
        if (idx >= 0 && idx < n) {
            char *pair[2] = { kept[idx], NULL };
            sel = device_save_kind(DEVICE_SNAPSHOT_ALLOWLIST, pair);
            printf("fastsnap: PROFILE-DRIFT section %d %s\n", idx, kept[idx]);
        }
        for (r = 0; r < rounds; r++) {
            t = fs_bench(fs_op_restore, sel, reps, batch, &p10, &p90);
            printf("fastsnap: PROFILE-DRIFT round %2d after %8d restores "
                   "med %8" PRId64 " ns  rss %ld kB\n",
                   r, r * (reps * batch + batch * 3), t, fs_rss_kb());
            fflush(stdout);
        }
        _exit(0);
    }

    if (!strcmp(mode, "mrt")) {
        int64_t a1, b1, a2, b2, c;
        c  = fs_bench(fs_op_mrt_only, NULL, reps, batch, NULL, NULL);
        a1 = fs_bench(fs_op_restore_mrt, all, reps, batch, &p10, &p90);
        printf("fastsnap: PROFILE-MRT bracketed(first)   med %8" PRId64
               " ns (p10 %" PRId64 " p90 %" PRId64 ")\n", a1, p10, p90);
        b1 = fs_bench(fs_op_restore, all, reps, batch, &p10, &p90);
        printf("fastsnap: PROFILE-MRT plain(second)      med %8" PRId64
               " ns (p10 %" PRId64 " p90 %" PRId64 ")\n", b1, p10, p90);
        b2 = fs_bench(fs_op_restore, all, reps, batch, NULL, NULL);
        a2 = fs_bench(fs_op_restore_mrt, all, reps, batch, NULL, NULL);
        printf("fastsnap: PROFILE-MRT plain(third)       med %8" PRId64
               " ns\n", b2);
        printf("fastsnap: PROFILE-MRT bracketed(fourth)  med %8" PRId64
               " ns\n", a2);
        printf("fastsnap: PROFILE-MRT empty transaction  med %8" PRId64
               " ns\n", c);
        printf("fastsnap: PROFILE-MRT ORDER CONTROL: bracketed is %.2fx and "
               "%.2fx of the plain run measured next to it\n",
               (double)a1 / (double)b1, (double)a2 / (double)b2);
        fflush(stdout);
        _exit(0);
    }

    if (!strncmp(mode, "one", 3) || !strncmp(mode, "loo", 3)) {
        DeviceSaveState *sel;
        char *pair[2];
        bool solo = !strncmp(mode, "one", 3);

        if (idx < 0 || idx >= n) {
            printf("fastsnap: PROFILE bad index\n");
            _exit(2);
        }
        pair[0] = kept[idx];
        pair[1] = NULL;
        sel = device_save_kind(solo ? DEVICE_SNAPSHOT_ALLOWLIST
                                    : DEVICE_SNAPSHOT_DENYLIST, pair);

        /* baseline measured FIRST, in the same fresh process */
        t = fs_bench(fs_op_restore, solo ? none : all, reps, batch,
                     &p10, &p90);
        printf("fastsnap: PROFILE-%s %d %-28s baseline %8" PRId64
               " ns (p10 %" PRId64 " p90 %" PRId64 ") bytes %zu\n",
               solo ? "ONE" : "LOO", idx, kept[idx], t, p10, p90,
               sel->save_buffer_size);
        {
            int64_t t2 = fs_bench(fs_op_restore, sel, reps, batch, &p10, &p90);
            printf("fastsnap: PROFILE-%s %d %-28s measured %8" PRId64
                   " ns (p10 %" PRId64 " p90 %" PRId64 ") delta %" PRId64
                   "\n", solo ? "ONE" : "LOO", idx, kept[idx], t2, p10, p90,
                   solo ? t2 - t : t - t2);
        }
        fflush(stdout);
        _exit(0);
    }

    if (!strncmp(mode, "save", 4)) {
        FsSaveArg a_none = { DEVICE_SNAPSHOT_ALLOWLIST, fs_nomatch };
        FsSaveArg a_all  = { DEVICE_SNAPSHOT_ALL, NULL };
        int64_t base;

        base = fs_bench(fs_op_save, &a_none, reps, batch, &p10, &p90);
        printf("fastsnap: PROFILE-SAVE empty  %8" PRId64 " ns (p10 %" PRId64
               " p90 %" PRId64 ")\n", base, p10, p90);
        t = fs_bench(fs_op_alloc_only, NULL, reps, batch, &p10, &p90);
        printf("fastsnap: PROFILE-SAVE alloc  %8" PRId64 " ns (p10 %" PRId64
               " p90 %" PRId64 ")\n", t, p10, p90);
        if (idx < 0) {
            t = fs_bench(fs_op_save, &a_all, reps, batch, &p10, &p90);
            printf("fastsnap: PROFILE-SAVE all    %8" PRId64 " ns (p10 %"
                   PRId64 " p90 %" PRId64 ")\n", t, p10, p90);
        } else {
            char *pair[2] = { kept[idx], NULL };
            FsSaveArg a_one = { DEVICE_SNAPSHOT_ALLOWLIST, pair };
            DeviceSaveState *sel =
                device_save_kind(DEVICE_SNAPSHOT_ALLOWLIST, pair);
            t = fs_bench(fs_op_save, &a_one, reps, batch, &p10, &p90);
            printf("fastsnap: PROFILE-SAVE %d %-28s %8" PRId64 " ns "
                   "(p10 %" PRId64 " p90 %" PRId64 ") delta %" PRId64
                   " bytes %zu\n", idx, kept[idx], t, p10, p90, t - base,
                   sel->save_buffer_size - none->save_buffer_size);
        }
        fflush(stdout);
        _exit(0);
    }

    /* default: "all" */
    t = fs_bench(fs_op_restore, all, reps, batch, &p10, &p90);
    printf("fastsnap: PROFILE-ALL restore_all   %8" PRId64 " ns (p10 %" PRId64
           " p90 %" PRId64 ")\n", t, p10, p90);
    t = fs_bench(fs_op_restore, none, reps, batch, &p10, &p90);
    printf("fastsnap: PROFILE-ALL restore_empty %8" PRId64 " ns (p10 %" PRId64
           " p90 %" PRId64 ")\n", t, p10, p90);
    fflush(stdout);
    _exit(0);
}

/* ---------------- end TEMPORARY INSTRUMENTATION ---------------- */

/*
 * Phase 5: the dirty-page counter, and the three ways it could be believed
 * while being wrong.
 *
 * The number this instrument exists to produce -- pages written per fuzzing
 * iteration -- is one a log cannot sanity-check. A counter stuck at zero reads
 * as "a tiny working set, the design is cheap"; a counter that reports every
 * page reads as "a huge working set, the design is dead". Both are plausible
 * answers to the question being asked, so neither can be caught by looking at
 * it. Only a known perturbation can separate them:
 *
 *   arm; count -> c0          baseline for an interval with nothing poked
 *   arm; poke N known pages
 *   count -> c1               CONTROL A: at least N, and the report must NAME
 *                             all N addresses. A count alone would be passed
 *                             by anything that returns a big number.
 *   count -> c2               CONTROL B: 0. The set was cleared, and the
 *                             counter does not re-report pages that were
 *                             dirtied in an earlier interval.
 *   poke the SAME N pages
 *   count -> c3               CONTROL C: N again.
 *
 * C is the one that is easy to leave out and is the reason the mechanism can
 * be used in a loop at all. TCG only traps a store to a page while that page's
 * TLB entry carries TLB_NOTDIRTY, and it loses that flag on the first write of
 * an interval. If clearing the bitmap did not also walk every vCPU's TLB and
 * put the flag back -- which is what physical_memory_dirty_bits_cleared()
 * does -- then the FIRST interval would be right and every interval after it
 * would silently under-count, which is precisely the shape of a measurement
 * that gets believed. A and B both pass in that world; only C fails.
 */
static int fastsnap_dirty_poke(uint32_t word)
{
    for (int i = 0; i < FASTSNAP_DIRTY_POKES; i++) {
        uint64_t gpa = FASTSNAP_DIRTY_BASE + (uint64_t)i * FASTSNAP_DIRTY_STRIDE;
        uint32_t v = word + i;

        address_space_write(&address_space_memory, gpa,
                            MEMTXATTRS_UNSPECIFIED, &v, sizeof(v));
    }
    return 0;
}

/* Every poked page must appear in the report, by name and offset. */
static int fastsnap_dirty_named(const char *report)
{
    int missing = 0;

    for (int i = 0; i < FASTSNAP_DIRTY_POKES; i++) {
        uint64_t off = (FASTSNAP_DIRTY_BASE - FASTSNAP_RAM_BASE) +
                       (uint64_t)i * FASTSNAP_DIRTY_STRIDE;
        g_autofree char *want = g_strdup_printf("+0x%" PRIx64, off);

        if (!strstr(report, want)) {
            printf("fastsnap: dirty report does not name the poked page at "
                   "%s\n", want);
            missing++;
        }
    }
    return missing;
}

static int64_t fastsnap_dirty_do(int op)
{
    uint64_t seq = penguin_fastsnap_seq();

    penguin_fastsnap_schedule(op);
    if (fastsnap_await(seq + 1) || penguin_fastsnap_last_rc() != 0) {
        return -1;
    }
    return (int64_t)penguin_fastsnap_dirty_pages();
}

static int fastsnap_selftest_dirty_track(void)
{
    int64_t c0, c1, c2, c3;
    int failures = 0;

    if (fastsnap_dirty_do(PENGUIN_FASTSNAP_DIRTY_ARM) < 0) {
        printf("fastsnap: FAIL - dirty tracking could not be armed\n");
        return 1;
    }
    printf("fastsnap: dirty tracking armed, page size %" PRIu64 ", %"
           PRIu64 " pages cleared in %" PRId64 " us\n",
           penguin_fastsnap_dirty_page_size(),
           penguin_fastsnap_dirty_pages(), penguin_fastsnap_last_us());

    /*
     * The arm must have cleared a whole machine's worth of bits. RAM blocks
     * are created with every dirty bit set, so an arm that reports nothing
     * cleared did not find the bitmap at all -- and every count after it would
     * then report zero, which looks exactly like a guest that writes nothing.
     */
    if (penguin_fastsnap_dirty_pages() == 0) {
        printf("fastsnap: FAIL - arming cleared no dirty bits, but RAM is "
               "created with all of them set, so the bitmap was not found\n");
        failures++;
    }

    c0 = fastsnap_dirty_do(PENGUIN_FASTSNAP_DIRTY_COUNT);
    if (c0 < 0) {
        printf("fastsnap: FAIL - dirty count did not complete\n");
        return failures + 1;
    }
    if (penguin_fastsnap_dirty_pages_scanned() == 0) {
        printf("fastsnap: FAIL - the count examined zero pages, so a result "
               "of '%" PRId64 " dirty' means nothing\n", c0);
        failures++;
    }
    printf("fastsnap: dirty baseline %" PRId64 " pages of %" PRIu64
           " scanned, %" PRId64 " us\n", c0,
           penguin_fastsnap_dirty_pages_scanned(),
           penguin_fastsnap_last_us());

    if (fastsnap_dirty_do(PENGUIN_FASTSNAP_DIRTY_ARM) < 0) {
        printf("fastsnap: FAIL - re-arm failed\n");
        return failures + 1;
    }
    fastsnap_dirty_poke(0xa5a50000U);
    c1 = fastsnap_dirty_do(PENGUIN_FASTSNAP_DIRTY_COUNT);

    /* CONTROL A. */
    if (c1 < FASTSNAP_DIRTY_POKES) {
        printf("fastsnap: CONTROL FAILED - %d known pages were written and the "
               "dirty counter saw %" PRId64 ". It is not tracking writes, so "
               "no dirty-set measurement built on it can be believed.\n",
               FASTSNAP_DIRTY_POKES, c1);
        failures++;
    } else if (fastsnap_dirty_named(penguin_fastsnap_dirty_report()) != 0) {
        printf("fastsnap: CONTROL FAILED - the counter reported %" PRId64
               " dirty pages but not the ones that were written. A count that "
               "does not name the right pages is not a measurement of this "
               "guest's working set.\n", c1);
        printf("fastsnap: report was: %s\n", penguin_fastsnap_dirty_report());
        failures++;
    } else {
        printf("fastsnap: control OK - %d poked pages, counter reports %"
               PRId64 " and names all of them (%s) in [%s]\n",
               FASTSNAP_DIRTY_POKES, c1, penguin_fastsnap_dirty_blocks(),
               penguin_fastsnap_dirty_report());
        if (c1 == FASTSNAP_DIRTY_POKES) {
            printf("fastsnap: dirty count is exact - %" PRId64 " poked, %"
                   PRId64 " counted, nothing else was writing\n",
                   (int64_t)FASTSNAP_DIRTY_POKES, c1);
        } else {
            printf("fastsnap: %" PRId64 " pages above the %d poked were "
                   "dirtied by the machine itself in the same interval\n",
                   c1 - FASTSNAP_DIRTY_POKES, FASTSNAP_DIRTY_POKES);
        }
    }

    /* CONTROL B: the set was consumed, not merely read. */
    c2 = fastsnap_dirty_do(PENGUIN_FASTSNAP_DIRTY_COUNT);
    if (c2 > c0) {
        printf("fastsnap: FAIL - a count with nothing poked reported %" PRId64
               " pages against a baseline of %" PRId64 ". Either the previous "
               "interval's pages are being re-reported or the counter invents "
               "them; in both cases an interval's number is not that "
               "interval's.\n", c2, c0);
        failures++;
    } else {
        printf("fastsnap: dirty set returns to baseline (%" PRId64 ")\n", c2);
    }

    /*
     * CONTROL C: the same pages again, in a later interval. See the comment
     * above -- this is the one that fails if clearing the bitmap does not also
     * re-arm TCG's TLB, and it is the only one of the three that a
     * first-interval-only tracker does not pass.
     */
    fastsnap_dirty_poke(0x5a5a0000U);
    c3 = fastsnap_dirty_do(PENGUIN_FASTSNAP_DIRTY_COUNT);
    if (c3 < FASTSNAP_DIRTY_POKES ||
        fastsnap_dirty_named(penguin_fastsnap_dirty_report()) != 0) {
        printf("fastsnap: CONTROL FAILED - writing the same %d pages in a "
               "SECOND interval reported %" PRId64 ". Tracking did not re-arm "
               "after the first count, so every interval but the first "
               "under-counts -- which is exactly what a believable wrong "
               "number looks like.\n", FASTSNAP_DIRTY_POKES, c3);
        printf("fastsnap: report was: %s\n", penguin_fastsnap_dirty_report());
        failures++;
    } else {
        printf("fastsnap: control OK - dirty tracking re-arms, second interval "
               "counts %" PRId64 " for the same %d pages\n", c3,
               FASTSNAP_DIRTY_POKES);
    }

    fastsnap_dirty_do(PENGUIN_FASTSNAP_DIRTY_STOP);
    return failures;
}


/*
 * Phase 6: the closed loop -- arm, disturb, reset, and ask something
 * independent whether the guest came back.
 *
 * This is the first test here that exercises a COMPLETE reset rather than one
 * half of it, and the verdict comes from the fork oracle, which shares no code
 * with the restore: the reference lives in another process, made by fork(),
 * and is compared with process_vm_readv(). The restore reads an in-process
 * copy. Neither can launder the other's mistakes.
 *
 *   LOOP_ARM             device block + RAM snapshot + fork reference, one BH
 *   disturb RAM and a device register
 *   FORK_DIFF -> d1      CONTROL: must be > 0
 *   LOOP_RESET           device restore + dirty-page RAM restore
 *   FORK_DIFF -> d2      must be 0
 *   device digest        must equal what LOOP_ARM recorded
 *
 * d1 is what makes d2 mean anything. A diff wired to return zero, a reference
 * sharing memory with the parent, a restore that silently did nothing -- each
 * produces d2 == 0, and each fails at d1.
 */
static int fastsnap_selftest_loop(void)
{
    uint32_t poke = 0xcafebabeU, imsc = FASTSNAP_IMSC_VALUE;
    uint64_t seq, arm_digest;
    int64_t d1, d2;
    int failures = 0;
    int i;

    seq = penguin_fastsnap_seq();
    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_LOOP_ARM);
    if (fastsnap_await(seq + 1) || penguin_fastsnap_last_rc() != 0) {
        printf("fastsnap: FAIL - loop arm did not complete\n");
        return 1;
    }
    arm_digest = penguin_fastsnap_last_digest();
    printf("fastsnap: loop armed in %" PRId64 " us\n",
           penguin_fastsnap_last_us());

    /* Disturb both halves: several RAM pages and one device register. A reset
     * that fixed only RAM, or only devices, has to fail. */
    for (i = 0; i < 6; i++) {
        address_space_write(&address_space_memory,
                            FASTSNAP_RAM_PROBE + (hwaddr)i * 0x10000,
                            MEMTXATTRS_UNSPECIFIED, &poke, sizeof(poke));
    }
    address_space_write(&address_space_memory,
                        FASTSNAP_UART0_BASE + FASTSNAP_PL011_IMSC,
                        MEMTXATTRS_UNSPECIFIED, &imsc, sizeof(imsc));

    seq = penguin_fastsnap_seq();
    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_FORK_DIFF);
    fastsnap_await(seq + 1);
    d1 = (int64_t)penguin_fastsnap_diff_pages();

    if (d1 <= 0) {
        printf("fastsnap: CONTROL FAILED - six pages of guest RAM were "
               "rewritten and the oracle sees no difference, so a clean "
               "result after the reset would prove nothing\n");
        failures++;
    } else {
        printf("fastsnap: control OK - oracle sees the disturbance "
               "(%" PRId64 " pages differ)\n", d1);
    }

    /*
     * THE PREFILTER MUST NOT CHANGE THE ANSWER -- checked HERE, on a state
     * that has differences, and not after the reset where the answer is zero.
     * Agreeing on zero is worth nothing: a filter that skipped every page
     * would agree on zero. It has to agree on SIX.
     *
     * The filter skips a page only when parent and child still share its
     * physical frame, which the kernel guarantees means identical bytes. Every
     * uncertainty -- not present, swapped, a PFN that reads zero -- resolves
     * to "compare it", so it can only ever do extra work. This asserts that
     * property rather than trusting the argument for it.
     *
     * Strength depends on where this runs. PFNs read as zero without
     * CAP_SYS_ADMIN, and a nix build sandbox has none, so there the filter
     * reports UNAVAILABLE and this proves only that the path is harmless. Run
     * with the capability and it proves the real thing, which is why the
     * status is printed rather than assumed.
     */
    {
        int st_on = penguin_fastsnap_diff_pagemap_status();
        uint64_t proved_on = penguin_fastsnap_diff_pages_proved();
        uint64_t read_on = penguin_fastsnap_diff_pages_read();
        int64_t d1_full;

        g_setenv("FASTSNAP_FORK_PAGEMAP", "0", true);
        seq = penguin_fastsnap_seq();
        penguin_fastsnap_schedule(PENGUIN_FASTSNAP_FORK_DIFF);
        fastsnap_await(seq + 1);
        d1_full = (int64_t)penguin_fastsnap_diff_pages();
        g_unsetenv("FASTSNAP_FORK_PAGEMAP");

        if (d1_full != d1) {
            printf("fastsnap: FAIL - the pagemap prefilter changed the "
                   "answer: %" PRId64 " differing pages with it, %" PRId64
                   " without. It is only ever allowed to change how many "
                   "pages are READ to reach the answer.\n", d1, d1_full);
            failures++;
        } else if (penguin_fastsnap_diff_pages_proved() != 0) {
            printf("fastsnap: FAIL - FASTSNAP_FORK_PAGEMAP=0 still proved "
                   "%" PRIu64 " pages by PFN, so the switch does not switch "
                   "it off and the control above compared nothing.\n",
                   penguin_fastsnap_diff_pages_proved());
            failures++;
        } else if (st_on == FASTSNAP_PAGEMAP_ACTIVE_ST) {
            printf("fastsnap: prefilter OK - same answer (%" PRId64 " pages) "
                   "with %" PRIu64 " pages proven by PFN identity and only "
                   "%" PRIu64 " read back, against %" PRIu64 " read with it "
                   "off\n", d1, proved_on, read_on,
                   penguin_fastsnap_diff_pages_read());
        } else {
            printf("fastsnap: prefilter INERT here (status %d, PFNs read as "
                   "zero without CAP_SYS_ADMIN) - the answer agrees, which "
                   "shows the path is harmless but not that it filters. Run "
                   "this with the capability to test that.\n", st_on);
        }
    }

    seq = penguin_fastsnap_seq();
    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_LOOP_RESET);
    if (fastsnap_await(seq + 1) || penguin_fastsnap_last_rc() != 0) {
        printf("fastsnap: FAIL - loop reset did not complete\n");
        return failures + 1;
    }
    printf("fastsnap: loop reset %" PRId64 " us, %" PRIu64 " RAM pages "
           "restored of %" PRIu64 " bytes snapshotted\n",
           penguin_fastsnap_last_us(),
           penguin_fastsnap_ram_restored_pages(),
           penguin_fastsnap_ram_snapshot_bytes());

    seq = penguin_fastsnap_seq();
    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_FORK_DIFF);
    fastsnap_await(seq + 1);
    d2 = (int64_t)penguin_fastsnap_diff_pages();

    if (d2 != 0) {
        printf("fastsnap: FAIL - after a complete reset the guest still "
               "differs from the reference in %" PRId64 " pages (at %s). The "
               "reset did not put RAM back.\n", d2,
               penguin_fastsnap_diff_report());
        failures++;
    } else {
        printf("fastsnap: LOOP OK - after reset the guest is byte-identical "
               "to an independently forked reference across %" PRIu64
               " bytes\n", penguin_fastsnap_diff_bytes_checked());
    }

    seq = penguin_fastsnap_seq();
    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_PROBE);
    fastsnap_await(seq + 1);
    if (penguin_fastsnap_last_digest() != arm_digest) {
        printf("fastsnap: FAIL - device state did not return to what loop arm "
               "recorded (%" PRIu64 " vs %" PRIu64 ")\n",
               penguin_fastsnap_last_digest(), arm_digest);
        failures++;
    } else {
        printf("fastsnap: device state matches the armed block\n");
    }

    /*
     * The combined op, which is the only form usable on a guest that is
     * actually running. Everything above schedules LOOP_RESET and FORK_DIFF as
     * two bottom halves, and gets away with it only because this machine has
     * no firmware: the vCPU writes nothing between the two, so the split form
     * still reports zero. A booted target does write in that gap -- ordinary
     * kernel work -- and the split form then reports a few hundred differing
     * pages for a reset that was perfectly correct.
     *
     * So what is checked here is not "is the diff zero" a second time; phase 6
     * already established the oracle can see a disturbance and that the reset
     * clears it. It is that the op DID THE WORK and kept the two clocks apart:
     *
     *   diff_bytes_checked == the whole snapshot, so a zero is a zero over all
     *                         of RAM rather than over nothing
     *   diff_us            >  0, so the comparison actually ran
     *   last_us            <  diff_us, so the oracle's tens of milliseconds
     *                         are not being reported as the reset's cost
     *
     * That last one is the assertion with teeth. Folding the diff into
     * last_us() would make every reset measured with verification on look two
     * orders of magnitude more expensive than it is, and the number would look
     * entirely plausible -- it is the shape of mistake that gets published.
     */
    for (i = 0; i < 6; i++) {
        address_space_write(&address_space_memory,
                            FASTSNAP_RAM_PROBE + (hwaddr)i * 0x10000,
                            MEMTXATTRS_UNSPECIFIED, &poke, sizeof(poke));
    }

    seq = penguin_fastsnap_seq();
    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_LOOP_RESET_VERIFY);
    if (fastsnap_await(seq + 1) || penguin_fastsnap_last_rc() != 0) {
        printf("fastsnap: FAIL - loop reset+verify did not complete\n");
        failures++;
    } else {
        int64_t d3 = (int64_t)penguin_fastsnap_diff_pages();
        int64_t rus = penguin_fastsnap_last_us();
        int64_t dus = penguin_fastsnap_diff_us();
        uint64_t checked = penguin_fastsnap_diff_bytes_checked();

        if (d3 != 0) {
            printf("fastsnap: FAIL - reset+verify still differs in %" PRId64
                   " pages (at %s)\n", d3, penguin_fastsnap_diff_report());
            failures++;
        } else if (checked != penguin_fastsnap_ram_snapshot_bytes()) {
            printf("fastsnap: FAIL - reset+verify compared %" PRIu64 " bytes "
                   "but the snapshot is %" PRIu64 "; a zero over a subset is "
                   "not a clean reset\n", checked,
                   penguin_fastsnap_ram_snapshot_bytes());
            failures++;
        } else if (dus <= 0) {
            printf("fastsnap: FAIL - reset+verify reports no time in the "
                   "comparison, so it did not run\n");
            failures++;
        } else if (rus >= dus) {
            printf("fastsnap: FAIL - reset+verify reports the reset at %"
                   PRId64 " us and the diff at %" PRId64 " us; the oracle's "
                   "cost is being charged to the reset\n", rus, dus);
            failures++;
        } else if (penguin_fastsnap_dev_diff_sections() != 0 ||
                   penguin_fastsnap_dev_unrestorable_sections() != 0) {
            /*
             * The control for phase 8. This reset restored the FULL device
             * set, so every section must match the reference -- if the device
             * oracle cannot report zero here it is not a usable instrument,
             * and the non-zero it reports for a scoped reset below would mean
             * nothing.
             */
            /*
             * Machine-specific, and deliberately so on THIS machine. A section
             * whose save is not a pure function of its restorable state can
             * never match -- mc146818rtc reads the live clock in pre_save and
             * re-derives its timers in post_load, so a malta guest reports it
             * as unrestorable forever, correctly. -M virt has no such section,
             * which is what makes a zero here meaningful and what makes phase
             * 8's positive control below readable.
             */
            printf("fastsnap: FAIL - a full-block reset left %d device "
                   "sections out of scope and %d unrestorable (%s); the "
                   "device oracle cannot be trusted to score an allowlist\n",
                   penguin_fastsnap_dev_diff_sections(),
                   penguin_fastsnap_dev_unrestorable_sections(),
                   penguin_fastsnap_dev_diff_report());
            failures++;
        } else {
            printf("fastsnap: VERIFY OK - reset %" PRId64 " us, oracle %"
                   PRId64 " us over %" PRIu64 " bytes, %d device sections "
                   "differ, in one bottom half\n",
                   rus, dus, checked,
                   penguin_fastsnap_dev_diff_sections());
            printf("fastsnap: every device section on this machine "
                   "round-trips (0 unrestorable)\n");
        }
    }

    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_FORK_DROP);
    fastsnap_await(penguin_fastsnap_seq() + 1);
    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_RAM_RELEASE);
    fastsnap_await(penguin_fastsnap_seq() + 1);
    return failures;
}

/*
 * Phase 8: the device allowlist, and the oracle that has to be able to catch it.
 *
 * The allowlist is the largest lever on reset cost -- measured, a full block is
 * an order of magnitude more expensive than {cpu, timer} -- and the only one
 * whose failure mode is silence. A section left out of the block is not
 * restored, nothing reports it, and the guest misbehaves some thousands of
 * iterations later with nothing pointing back at the configuration.
 *
 * penguin_fastsnap_dev_diff_sections() is what makes it checkable, so this
 * phase is mostly about proving that number is not blind. Phase 7 has already
 * shown a full-block reset scores zero. Here a section is deliberately dropped
 * from the block and then dirtied, and the count MUST rise and MUST name it.
 * Without this pairing, a zero from a scoped reset is indistinguishable from an
 * oracle that never looks -- which is the failure this lane has now produced
 * often enough to design against rather than hope about.
 */
static int fastsnap_selftest_allowlist(void)
{
    uint32_t poke = 0xfeedfaceU, imsc = FASTSNAP_IMSC_VALUE;
    char *pl011_id = NULL;
    GString *csv;
    uint64_t seq;
    char **all;
    int failures = 0;
    int i, n;

    all = device_list_all();
    csv = g_string_new(NULL);
    for (n = 0; all[n]; n++) {
        if (strstr(all[n], "pl011")) {
            pl011_id = g_strdup(all[n]);
            continue;               /* the one section left out */
        }
        g_string_append_printf(csv, "%s%s", csv->len ? "," : "", all[n]);
    }
    g_free(all);

    if (!pl011_id) {
        /* Not a failure: a machine without a PL011 has nothing to drop that
         * this phase knows how to dirty on demand. Say so rather than
         * reporting a pass for a check that did not run. */
        printf("fastsnap: allowlist phase SKIPPED - no pl011 section on this "
               "machine to leave out of the block\n");
        g_string_free(csv, TRUE);
        return 0;
    }

    penguin_fastsnap_set_allowlist(csv->str);
    g_string_free(csv, TRUE);

    seq = penguin_fastsnap_seq();
    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_LOOP_ARM);
    if (fastsnap_await(seq + 1) || penguin_fastsnap_last_rc() != 0) {
        printf("fastsnap: FAIL - loop arm with an allowlist did not "
               "complete\n");
        penguin_fastsnap_set_allowlist(NULL);
        g_free(pl011_id);
        return 1;
    }
    printf("fastsnap: allowlist block covers %d of %d sections, %" PRIu64
           " bytes, armed in %" PRId64 " us\n",
           n - 1, n, penguin_fastsnap_block_size(),
           penguin_fastsnap_last_us());

    /* Dirty both halves, including the section that is NOT in the block. */
    for (i = 0; i < 6; i++) {
        address_space_write(&address_space_memory,
                            FASTSNAP_RAM_PROBE + (hwaddr)i * 0x10000,
                            MEMTXATTRS_UNSPECIFIED, &poke, sizeof(poke));
    }
    address_space_write(&address_space_memory,
                        FASTSNAP_UART0_BASE + FASTSNAP_PL011_IMSC,
                        MEMTXATTRS_UNSPECIFIED, &imsc, sizeof(imsc));

    seq = penguin_fastsnap_seq();
    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_LOOP_RESET_VERIFY);
    if (fastsnap_await(seq + 1) || penguin_fastsnap_last_rc() != 0) {
        printf("fastsnap: FAIL - scoped reset+verify did not complete\n");
        failures++;
    } else {
        int64_t dpages = (int64_t)penguin_fastsnap_diff_pages();
        int ndev = penguin_fastsnap_dev_diff_sections();
        const char *report = penguin_fastsnap_dev_diff_report();

        /* RAM is unaffected by device scoping, so this must still be clean --
         * if it is not, the allowlist broke something it has no business
         * touching. */
        if (dpages != 0) {
            printf("fastsnap: FAIL - a device allowlist changed the RAM "
                   "result: %" PRId64 " pages differ (at %s)\n", dpages,
                   penguin_fastsnap_diff_report());
            failures++;
        }
        if (ndev < 0) {
            printf("fastsnap: FAIL - the device oracle could not compare "
                   "(%d); a scoped reset cannot be scored\n", ndev);
            failures++;
        } else if (ndev == 0) {
            printf("fastsnap: CONTROL FAILED - '%s' was left out of the block "
                   "and then written to, and the device oracle still reports "
                   "0 sections differing. A clean score for any allowlist "
                   "would prove nothing.\n", pl011_id);
            failures++;
        } else if (penguin_fastsnap_dev_unrestorable_sections() != 0) {
            printf("fastsnap: FAIL - the oracle put %d sections in the "
                   "unrestorable bucket (%s) on a machine where phase 7 just "
                   "showed every section round-trips. A section that was in "
                   "the block cannot be a scope miss and vice versa; if the "
                   "two buckets can be confused, an allowlist derived from "
                   "them adds sections it already has.\n",
                   penguin_fastsnap_dev_unrestorable_sections(), report);
            failures++;
        } else if (!strstr(report, pl011_id)) {
            printf("fastsnap: FAIL - the device oracle reports %d sections "
                   "differing (%s) but not '%s', which is the one that was "
                   "dropped and dirtied\n", ndev, report, pl011_id);
            failures++;
        } else {
            printf("fastsnap: ALLOWLIST OK - reset %" PRId64 " us over %d "
                   "sections, RAM clean, and the oracle names the dropped "
                   "section: %d differ (%s)\n",
                   penguin_fastsnap_last_us(), n - 1, ndev, report);
        }
    }

    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_FORK_DROP);
    fastsnap_await(penguin_fastsnap_seq() + 1);
    penguin_fastsnap_schedule(PENGUIN_FASTSNAP_RAM_RELEASE);
    fastsnap_await(penguin_fastsnap_seq() + 1);
    penguin_fastsnap_set_allowlist(NULL);
    g_free(pl011_id);
    return failures;
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
    /*
     * Printed in full because the ids are NOT unique -- -M virt registers two
     * called "pflash_cfi01" -- and every list-shaped thing built on top of them
     * (allowlists, denylists, the per-section device oracle) has to be written
     * knowing that. It cost one false positive to learn.
     */
    for (int k = 0; k < nkept; k++) {
        printf("fastsnap:   section[%d] %s\n", k, kept[k]);
    }
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

    printf("fastsnap: phase 1 %s\n", failures ? "FAILED" : "passed");

    g_free(a_copy);
    device_free_all(a);
    device_free_all(b);
    device_free_all(c);
    g_free(a);
    g_free(b);
    g_free(c);

    fflush(stdout);

    /*
     * Hand off to phase 2, which needs the VM running. Recorded rather than
     * exited on, so a phase-1 failure still shows up in the final verdict.
     */
    fastsnap_profile_run();   /* TEMPORARY: env-gated */

    phase1_failures = failures;
    running_watch = qemu_add_vm_change_state_handler(fastsnap_on_running, NULL);
}

static void fastsnap_on_running(void *opaque, bool running, RunState state)
{
    int failures;

    if (!running || phase2_done) {
        return;
    }
    phase2_done = true;

    failures = phase1_failures + fastsnap_selftest_scheduled();
    failures += fastsnap_selftest_state_digest();
    failures += fastsnap_selftest_fork_oracle();
    failures += fastsnap_selftest_dirty_track();
    failures += fastsnap_selftest_loop();
    failures += fastsnap_selftest_allowlist();
    printf("fastsnap: SELFTEST %s\n", failures ? "FAILED" : "PASSED");
    fflush(stdout);
    /* _exit, not exit: returning through QEMU's atexit teardown from a
     * change-state handler segfaults, which would leave a PASS carrying a
     * core-dump exit status. */
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
