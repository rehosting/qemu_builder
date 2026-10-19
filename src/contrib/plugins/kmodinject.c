/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * kmodinject: load a kernel module into an unmodified 32-bit ARM or AArch64
 * Linux guest from the hypervisor, by borrowing one user process's syscalls.
 *
 * The guest image is not touched and nothing runs in guest userspace. The
 * plugin hooks one instruction in the kernel's SWI entry (vector_swi), at a
 * point where the user registers are saved in the pt_regs frame on the
 * kernel stack and r8 holds SPSR. On the first user-mode EABI syscall it
 * sees, it replaces that syscall's number and arguments, and rewinds the
 * saved user PC onto the SWI, so the process re-enters the kernel right
 * after the injected call returns. Each re-entry runs the next step:
 *
 *   1. mmap2(NULL, size, RW, PRIVATE|ANONYMOUS|POPULATE)  -> buffer
 *      the plugin copies the module (and its parameter string) into it
 *   2. init_module(buffer, len, params)
 *   3. munmap(buffer, size)
 *   4. the original syscall, with its original r0 put back
 *
 * The process sees only its own syscall, a little late. Registers r1-r12
 * are restored from the frame on every return, so only r0 needs putting
 * back; the frame copies of r0 and ORIG_r0 are fixed too, so a restarted
 * syscall gets the right first argument.
 *
 * Arguments (all but ko have defaults for a 2.6.x-4.x ARM entry-common.S):
 *   ko=<path>          module file to load (required)
 *   hook=<va>          instruction in vector_swi after "mrs r8, spsr" and
 *                      the frame store (2.6.36: vector_swi+0x20)
 *   params=<string>    module parameters (default empty)
 *   frame_pc=<off>     offset of the saved PC in the frame (S_PC, 60)
 *   frame_r0=<off>     offset of saved r0 (S_R0, 0)
 *   frame_orig_r0=<off> offset of ORIG_r0 (S_OLD_R0, 68)
 *   skip=<n>           let n eligible syscalls pass before injecting (0)
 *   abi=eabi|oabi      the kernel's syscall ABI (eabi). An OABI kernel takes
 *                      the number from the SWI immediate (swi 0x9000NN), so
 *                      hook= must be after vector_swi decodes it into r7
 *                      ("eor r7, r7, #0x900000"; 3.4: vector_swi+0x4c), where
 *                      writing r7 sets the number. r8 is no longer SPSR
 *                      there, so SPSR is read from the frame instead.
 *   frame_psr=<off>    offset of the saved CPSR in the frame (S_PSR, 64),
 *                      for abi=oabi
 *   rearm=<va>         load the module again after each guest reset. The va
 *                      is an instruction only a boot runs, such as the
 *                      kernel's entry with the MMU off (0x8000 for a 32-bit
 *                      ARM zImage or Image at the usual offset). Once the
 *                      module is in, only that instruction stays
 *                      instrumented; when it runs again the plugin arms
 *                      itself for the new boot's first syscall.
 *
 * AArch64 works the same way, with these differences:
 *   - hook= is an instruction where a register holds the pt_regs pointer of
 *     an EL0 SVC: the entry of do_el0_svc (5.x and later; el0_svc_handler
 *     on 4.19-5.4), where x0 holds it (frame=x0, the default), or the
 *     el0_svc label of the older assembly entry, where sp does (frame=sp).
 *   - The C entry reads the number (x8) and arguments (x0-x5) from the
 *     frame, the assembly entry from the live registers, so frame=sp writes
 *     both (frame=x0 only the frame: x0 is the pointer there).
 *   - The steps are mmap, init_module and munmap, and every register the
 *     injection changed (x0-x5, x8) is put back for the original syscall.
 *   - A 32-bit (compat) process enters through do_el0_svc_compat (x0 holds
 *     the pointer) and uses the ARM EABI: number in r7 (x7), mmap2, ARM or
 *     Thumb SVC. The saved PSTATE says which ABI the victim uses, so hook
 *     the entry its processes take: a 32-bit userland never reaches
 *     do_el0_svc.
 *   - frame_pc (default 256) and frame_r0 (default 0) are the pt_regs
 *     offsets of pc and regs[0]; frame_pstate (264) is the saved PSTATE,
 *     which must say EL0 (AArch64 EL0t, or AArch32 User).
 *
 * The result goes to the QEMU log ("-d plugin" or "-D file"), one line per
 * step.
 */
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define NR_MMAP2        192
#define NR_INIT_MODULE  128
#define NR_MUNMAP       91
#define PROT_RW         0x3
#define MAP_FLAGS       (0x02 | 0x20 | 0x8000)  /* PRIVATE|ANONYMOUS|POPULATE */
#define SWI_ARM_EABI    0xef000000u
#define SWI_ARM_OABI    0x0f900000u     /* swi 0x9000NN, condition ignored */
#define SWI_ARM_OABI_M  0x0ff00000u
#define SWI_THUMB       0xdf00u
#define PSR_T           0x20u
#define PSR_MODE        0x1fu
#define MODE_USR        0x10u
#define THREAD_SHIFT    13

#define A64_NR_MMAP         222
#define A64_NR_MUNMAP       215
#define A64_NR_INIT_MODULE  105
#define A64_SVC0            0xd4000001u
#define A64_PSTATE_MODE     0x1fu       /* EL0t in AArch64 is 0 */
#define A64_PSTATE_RW32     0x10u       /* AArch32: M[3:0] = 0 is User */

enum step { ARMED, MMAPPED, LOADED, UNMAPPED, DONE };

static qemu_plugin_id_t plugin_id;
static GMutex lock;
static uint64_t hook_va;
static uint32_t off_pc = 60, off_r0 = 0, off_orig_r0 = 68, off_psr = 64;
static bool oabi;
static unsigned long skip, skip0;
static uint64_t rearm_va;
static bool watching;           /* loaded: only rearm_va is instrumented */
static char *ko_path, *params = "";
static GByteArray *ko;

static struct qemu_plugin_register *reg[16];
static const char *reg_names[16] = {
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
    "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc",
};

/* injection state: one victim task, identified by its kernel stack */
static enum step step = ARMED;
static uint32_t victim, swi_pc, ret_pc, orig_r0, buf, buf_size;

/* AArch64 */
static bool a64, a64_frame_sp;
static uint32_t off_pstate = 264;
static struct qemu_plugin_register *xreg[33];    /* x0-x30, sp, pc */
static uint64_t a64_victim, a64_svc_pc, a64_ret_pc, a64_buf, a64_saved[9];
static bool a64_compat;         /* the victim is a 32-bit process */

static void logf_(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    g_autofree char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    g_autofree char *line = g_strdup_printf("kmodinject: %s\n", msg);
    qemu_plugin_outs(line);
}

static uint32_t rd(struct qemu_plugin_register *r)
{
    g_autoptr(GByteArray) b = g_byte_array_new();
    qemu_plugin_read_register(r, b);
    uint32_t v = 0;
    memcpy(&v, b->data, MIN(b->len, sizeof(v)));
    return GUINT32_FROM_LE(v);
}

static void wr(int i, uint32_t v)
{
    g_autoptr(GByteArray) b = g_byte_array_new();
    uint32_t le = GUINT32_TO_LE(v);
    g_byte_array_append(b, (guint8 *)&le, 4);
    qemu_plugin_write_register(reg[i], b);
}

static bool rd_mem32(uint64_t va, uint32_t *v)
{
    g_autoptr(GByteArray) b = g_byte_array_new();
    if (!qemu_plugin_read_memory_vaddr(va, b, 4)) {
        return false;
    }
    memcpy(v, b->data, 4);
    *v = GUINT32_FROM_LE(*v);
    return true;
}

static bool wr_mem32(uint64_t va, uint32_t v)
{
    g_autoptr(GByteArray) b = g_byte_array_new();
    uint32_t le = GUINT32_TO_LE(v);
    g_byte_array_append(b, (guint8 *)&le, 4);
    return qemu_plugin_write_memory_vaddr(va, b);
}

/* Make the current syscall nr(a0..a5) instead, and come back to the SWI. */
static void inject(uint32_t frame, uint32_t nr, const uint32_t *a, int n)
{
    for (int i = 0; i < n; i++) {
        wr(i, a[i]);
    }
    wr(7, nr);
    wr_mem32(frame + off_pc, swi_pc);
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *u);

static void all_done(void *u)
{
}

/* After a reset of the plugin's callbacks: watch for the next boot. */
static void watch(void *u)
{
    watching = true;
    qemu_plugin_register_vcpu_tb_trans_cb(plugin_id, vcpu_tb_trans, NULL);
}

static void rearm(void *u)
{
    watching = false;
    qemu_plugin_register_vcpu_tb_trans_cb(plugin_id, vcpu_tb_trans, NULL);
}

static void loaded(void)
{
    step = DONE;
    qemu_plugin_reset(plugin_id, rearm_va ? watch : all_done, NULL);
}

static void on_entry(unsigned int vcpu, void *u)
{
    g_mutex_lock(&lock);
    if (watching && step == DONE) {
        logf_("guest boot at 0x%" PRIx64 "; armed again", rearm_va);
        step = ARMED;
        skip = skip0;
        watching = false;       /* once, until the reset below completes */
        qemu_plugin_reset(plugin_id, rearm, NULL);
    }
    g_mutex_unlock(&lock);
}

static void on_swi(unsigned int vcpu, void *u)
{
    uint32_t frame = rd(reg[13]);
    uint32_t spsr = 0;
    if (!oabi) {
        spsr = rd(reg[8]);
    } else if (!rd_mem32(frame + off_psr, &spsr)) {
        return;
    }
    uint32_t lr = rd(reg[14]);
    uint32_t task = frame >> THREAD_SHIFT;

    if ((spsr & PSR_MODE) != MODE_USR) {
        return;
    }

    g_mutex_lock(&lock);
    switch (step) {
    case ARMED: {
        /* EABI: svc #0 with the number in r7. OABI: swi 0x9000NN, already
         * decoded into r7 at the hook, so r7 is what the kernel indexes by. */
        uint32_t insn = 0, pc = (spsr & PSR_T) ? lr - 2 : lr - 4;
        if (!rd_mem32(pc & ~3u, &insn)) {
            break;
        }
        if (oabi) {
            if ((spsr & PSR_T) || (insn & SWI_ARM_OABI_M) != SWI_ARM_OABI) {
                break;
            }
        } else if (spsr & PSR_T) {
            insn = (pc & 2) ? insn >> 16 : insn & 0xffff;
            if (insn != SWI_THUMB) {
                break;
            }
        } else if (insn != SWI_ARM_EABI) {
            break;
        }
        if (skip) {
            skip--;
            break;
        }
        victim = task;
        swi_pc = pc;
        ret_pc = lr;
        orig_r0 = rd(reg[0]);
        buf_size = (ko->len + strlen(params) + 1 + 4095) & ~4095u;
        logf_("victim task stack 0x%08x, syscall %u at pc 0x%08x%s; "
              "mmap2 %u bytes", task << THREAD_SHIFT, rd(reg[7]), pc,
              (spsr & PSR_T) ? " (thumb)" : "", buf_size);
        uint32_t a[6] = { 0, buf_size, PROT_RW, MAP_FLAGS, (uint32_t)-1, 0 };
        inject(frame, NR_MMAP2, a, 6);
        step = MMAPPED;
        break;
    }
    case MMAPPED:
    case LOADED:
    case UNMAPPED: {
        if (task != victim || lr != ret_pc) {
            break;          /* someone else, or a signal handler */
        }
        int32_t ret = (int32_t)rd(reg[0]);
        if (step == MMAPPED) {
            if (ret < 0 && ret > -4096) {
                logf_("mmap2 failed: %d; giving up", ret);
                goto restore;
            }
            buf = (uint32_t)ret;
            g_autoptr(GByteArray) b = g_byte_array_sized_new(buf_size);
            g_byte_array_append(b, ko->data, ko->len);
            g_byte_array_append(b, (guint8 *)params, strlen(params) + 1);
            if (!qemu_plugin_write_memory_vaddr(buf, b)) {
                logf_("copy into 0x%08x failed; unmapping", buf);
                uint32_t a[2] = { buf, buf_size };
                inject(frame, NR_MUNMAP, a, 2);
                step = UNMAPPED;
                break;
            }
            logf_("buffer 0x%08x, %u bytes copied; init_module(\"%s\")",
                  buf, ko->len, params);
            uint32_t a[3] = { buf, ko->len, buf + ko->len };
            inject(frame, NR_INIT_MODULE, a, 3);
            step = LOADED;
        } else if (step == LOADED) {
            logf_("init_module returned %d%s", ret,
                  ret == 0 ? " (module loaded)" : "");
            uint32_t a[2] = { buf, buf_size };
            inject(frame, NR_MUNMAP, a, 2);
            step = UNMAPPED;
        } else {
            logf_("munmap returned %d; resuming the original syscall", ret);
        restore:
            wr(0, orig_r0);
            wr_mem32(frame + off_r0, orig_r0);
            wr_mem32(frame + off_orig_r0, orig_r0);
            loaded();
        }
        break;
    }
    default:
        break;
    }
    g_mutex_unlock(&lock);
}

static uint64_t rd64(struct qemu_plugin_register *r)
{
    g_autoptr(GByteArray) b = g_byte_array_new();
    qemu_plugin_read_register(r, b);
    uint64_t v = 0;
    memcpy(&v, b->data, MIN(b->len, sizeof(v)));
    return GUINT64_FROM_LE(v);
}

static void wr64(int i, uint64_t v)
{
    g_autoptr(GByteArray) b = g_byte_array_new();
    uint64_t le = GUINT64_TO_LE(v);
    g_byte_array_append(b, (guint8 *)&le, 8);
    qemu_plugin_write_register(xreg[i], b);
}

static bool rd_mem64(uint64_t va, uint64_t *v)
{
    g_autoptr(GByteArray) b = g_byte_array_new();
    if (!qemu_plugin_read_memory_vaddr(va, b, 8)) {
        return false;
    }
    memcpy(v, b->data, 8);
    *v = GUINT64_FROM_LE(*v);
    return true;
}

static bool wr_mem64(uint64_t va, uint64_t v)
{
    g_autoptr(GByteArray) b = g_byte_array_new();
    uint64_t le = GUINT64_TO_LE(v);
    g_byte_array_append(b, (guint8 *)&le, 8);
    return qemu_plugin_write_memory_vaddr(va, b);
}

/* Set saved (and, with frame=sp, live) x<i>. */
static void a64_set(uint64_t frame, int i, uint64_t v)
{
    wr_mem64(frame + off_r0 + 8 * i, v);
    if (a64_frame_sp) {
        wr64(i, v);
    }
}

/* Make the current syscall nr(a0..a5) instead, and come back to the SVC. */
static void a64_inject(uint64_t frame, uint64_t nr, const uint64_t *a, int n)
{
    for (int i = 0; i < n; i++) {
        a64_set(frame, i, a[i]);
    }
    a64_set(frame, a64_compat ? 7 : 8, nr);
    wr_mem64(frame + off_pc, a64_svc_pc);
}

static void on_svc64(unsigned int vcpu, void *u)
{
    uint64_t frame, pc, pstate;
    uint32_t insn;

    /* plugin insn callbacks can fire for another VA of the same code */
    if (rd64(xreg[32]) != hook_va) {
        return;
    }
    frame = rd64(xreg[a64_frame_sp ? 31 : 0]);
    if (!rd_mem64(frame + off_pc, &pc) ||
        !rd_mem64(frame + off_pstate, &pstate)) {
        return;
    }
    bool compat = pstate & A64_PSTATE_RW32;
    if ((pstate & (compat ? 0xfu : A64_PSTATE_MODE)) != 0) {
        return;             /* not from EL0 / User mode */
    }

    g_mutex_lock(&lock);
    switch (step) {
    case ARMED: {
        bool thumb = compat && (pstate & PSR_T);
        uint64_t svc_pc = pc - (thumb ? 2 : 4);
        if (!rd_mem32(svc_pc & ~3ull, &insn)) {
            break;
        }
        if (thumb) {
            insn = (svc_pc & 2) ? insn >> 16 : insn & 0xffff;
            if (insn != SWI_THUMB) {
                break;
            }
        } else if (insn != (compat ? SWI_ARM_EABI : A64_SVC0)) {
            break;
        }
        if (skip) {
            skip--;
            break;
        }
        a64_victim = frame;
        a64_compat = compat;
        a64_svc_pc = svc_pc;
        a64_ret_pc = pc;
        for (int i = 0; i < 9; i++) {
            rd_mem64(frame + off_r0 + 8 * i, &a64_saved[i]);
        }
        buf_size = (ko->len + strlen(params) + 1 + 4095) & ~4095u;
        logf_("victim frame 0x%016" PRIx64 ", %s syscall %" PRIu64
              " at pc 0x%016" PRIx64 "%s; mmap%s %u bytes",
              frame, compat ? "32-bit" : "64-bit",
              a64_saved[compat ? 7 : 8], svc_pc, thumb ? " (thumb)" : "",
              compat ? "2" : "", buf_size);
        uint64_t a[6] = { 0, buf_size, PROT_RW, MAP_FLAGS,
                          compat ? 0xffffffffull : (uint64_t)-1, 0 };
        a64_inject(frame, compat ? NR_MMAP2 : A64_NR_MMAP, a, 6);
        step = MMAPPED;
        break;
    }
    case MMAPPED:
    case LOADED:
    case UNMAPPED: {
        uint64_t x0;
        if (frame != a64_victim || pc != a64_ret_pc ||
            !rd_mem64(frame + off_r0, &x0)) {
            break;          /* someone else, or a signal handler */
        }
        /* a 32-bit process's x0 holds its r0, zero-extended */
        int64_t ret = a64_compat ? (int32_t)x0 : (int64_t)x0;
        if (step == MMAPPED) {
            if (ret < 0 && ret > -4096) {
                logf_("mmap failed: %" PRId64 "; giving up", ret);
                goto restore;
            }
            a64_buf = a64_compat ? (uint32_t)x0 : x0;
            g_autoptr(GByteArray) b = g_byte_array_sized_new(buf_size);
            g_byte_array_append(b, ko->data, ko->len);
            g_byte_array_append(b, (guint8 *)params, strlen(params) + 1);
            if (!qemu_plugin_write_memory_vaddr(a64_buf, b)) {
                logf_("copy into 0x%016" PRIx64 " failed; unmapping", a64_buf);
                uint64_t a[2] = { a64_buf, buf_size };
                a64_inject(frame, a64_compat ? NR_MUNMAP : A64_NR_MUNMAP, a, 2);
                step = UNMAPPED;
                break;
            }
            logf_("buffer 0x%016" PRIx64 ", %u bytes copied; "
                  "init_module(\"%s\")", a64_buf, ko->len, params);
            uint64_t a[3] = { a64_buf, ko->len, a64_buf + ko->len };
            a64_inject(frame, a64_compat ? NR_INIT_MODULE : A64_NR_INIT_MODULE,
                       a, 3);
            step = LOADED;
        } else if (step == LOADED) {
            logf_("init_module returned %" PRId64 "%s", ret,
                  ret == 0 ? " (module loaded)" : "");
            uint64_t a[2] = { a64_buf, buf_size };
            a64_inject(frame, a64_compat ? NR_MUNMAP : A64_NR_MUNMAP, a, 2);
            step = UNMAPPED;
        } else {
            logf_("munmap returned %" PRId64 "; resuming the original syscall",
                  ret);
        restore:
            for (int i = 0; i < 9; i++) {
                a64_set(frame, i, a64_saved[i]);
            }
            loaded();
        }
        break;
    }
    default:
        break;
    }
    g_mutex_unlock(&lock);
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *u)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t va = qemu_plugin_insn_vaddr(insn);
        if (watching) {
            if (va == rearm_va) {
                qemu_plugin_register_vcpu_insn_exec_cb(insn, on_entry,
                                                       QEMU_PLUGIN_CB_NO_REGS,
                                                       NULL);
            }
        } else if (va == hook_va) {
            qemu_plugin_register_vcpu_insn_exec_cb(insn, a64 ? on_svc64 : on_swi,
                                                   QEMU_PLUGIN_CB_RW_REGS,
                                                   NULL);
        }
    }
}

static void vcpu_init(unsigned int vcpu, void *u)
{
    g_autoptr(GArray) regs = qemu_plugin_get_registers();
    for (guint i = 0; i < regs->len; i++) {
        qemu_plugin_reg_descriptor *d =
            &g_array_index(regs, qemu_plugin_reg_descriptor, i);
        if (a64) {
            if (d->name[0] == 'x' && g_ascii_isdigit(d->name[1])) {
                guint64 n = g_ascii_strtoull(d->name + 1, NULL, 10);
                if (n <= 30) {
                    xreg[n] = d->handle;
                }
            } else if (!strcmp(d->name, "sp")) {
                xreg[31] = d->handle;
            } else if (!strcmp(d->name, "pc")) {
                xreg[32] = d->handle;
            }
            continue;
        }
        for (int r = 0; r < 16; r++) {
            if (!strcmp(d->name, reg_names[r])) {
                reg[r] = d->handle;
            }
        }
    }
}

QEMU_PLUGIN_EXPORT
int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                        int argc, char **argv)
{
    plugin_id = id;
    if (!strcmp(info->target_name, "aarch64")) {
        a64 = true;
        off_pc = 256;
    } else if (strcmp(info->target_name, "arm")) {
        fprintf(stderr, "kmodinject: ARM and AArch64 only (target %s)\n",
                info->target_name);
        return -1;
    }
    for (int i = 0; i < argc; i++) {
        g_auto(GStrv) kv = g_strsplit(argv[i], "=", 2);
        if (!kv[0] || !kv[1]) {
            fprintf(stderr, "kmodinject: bad argument '%s'\n", argv[i]);
            return -1;
        }
        if (!strcmp(kv[0], "ko")) {
            ko_path = g_strdup(kv[1]);
        } else if (!strcmp(kv[0], "hook")) {
            hook_va = g_ascii_strtoull(kv[1], NULL, 0);
        } else if (!strcmp(kv[0], "params")) {
            params = g_strdup(kv[1]);
        } else if (!strcmp(kv[0], "frame_pc")) {
            off_pc = g_ascii_strtoull(kv[1], NULL, 0);
        } else if (!strcmp(kv[0], "frame_r0")) {
            off_r0 = g_ascii_strtoull(kv[1], NULL, 0);
        } else if (!strcmp(kv[0], "frame_orig_r0")) {
            off_orig_r0 = g_ascii_strtoull(kv[1], NULL, 0);
        } else if (!strcmp(kv[0], "frame_pstate")) {
            off_pstate = g_ascii_strtoull(kv[1], NULL, 0);
        } else if (!strcmp(kv[0], "frame") && a64) {
            if (!strcmp(kv[1], "sp")) {
                a64_frame_sp = true;
            } else if (strcmp(kv[1], "x0")) {
                fprintf(stderr, "kmodinject: frame must be x0 or sp\n");
                return -1;
            }
        } else if (!strcmp(kv[0], "frame_psr")) {
            off_psr = g_ascii_strtoull(kv[1], NULL, 0);
        } else if (!strcmp(kv[0], "abi") && !a64) {
            if (!strcmp(kv[1], "oabi")) {
                oabi = true;
            } else if (strcmp(kv[1], "eabi")) {
                fprintf(stderr, "kmodinject: abi must be eabi or oabi\n");
                return -1;
            }
        } else if (!strcmp(kv[0], "skip")) {
            skip = skip0 = g_ascii_strtoull(kv[1], NULL, 0);
        } else if (!strcmp(kv[0], "rearm")) {
            rearm_va = g_ascii_strtoull(kv[1], NULL, 0);
        } else {
            fprintf(stderr, "kmodinject: unknown argument '%s'\n", kv[0]);
            return -1;
        }
    }
    if (!ko_path || !hook_va) {
        fprintf(stderr, "kmodinject: ko= and hook= are required\n");
        return -1;
    }
    gchar *data;
    gsize len;
    g_autoptr(GError) err = NULL;
    if (!g_file_get_contents(ko_path, &data, &len, &err)) {
        fprintf(stderr, "kmodinject: %s\n", err->message);
        return -1;
    }
    ko = g_byte_array_new_take((guint8 *)data, len);
    g_mutex_init(&lock);
    qemu_plugin_register_vcpu_init_cb(id, vcpu_init, NULL);
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans, NULL);
    return 0;
}
