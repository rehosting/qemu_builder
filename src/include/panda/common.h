/*!
 * @file panda/common.h
 * @brief Common PANDA utility functions.
 *
 * @note Functions that are both simple and frequently called are
 * defined here as inlines. Functions that are either complex or
 * infrequently called are decalred here and defined in `src/common.c`.
 */
#pragma once
#if !defined(__cplusplus)
#include <stdint.h>
#include <stdbool.h>
#else
#include <cstdint>
#include <cstdbool>
#endif
#include "qemu/osdep.h"
#include "qemu/accel.h"
#include "cpu.h"
#include "system/address-spaces.h"
#include "exec/target_page.h"
#include "system/memory.h"
#include "panda/types.h"
#include "gdbstub/internals.h"

/*
 * @brief Branch predition hint macros.
 */
#if !defined(likely)
#define likely(x) __builtin_expect(!!(x), 1)
#endif
#if !defined(unlikely)
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#ifdef __cplusplus
extern "C" {
#endif
    
#if defined(TARGET_MIPS)
#define MIPS_HFLAG_KSU    0x00003 /* kernel/supervisor/user mode mask   */
#define MIPS_HFLAG_KM     0x00000 /* kernel mode flag                   */
/*
 *  Register values from: http://www.cs.uwm.edu/classes/cs315/Bacon/Lecture/HTML/ch05s03.html
 */
#define MIPS_SP           29      /* value for MIPS stack pointer offset into GPR */
#define MIPS_V0           2
#define MIPS_V1           3
#endif

void panda_cleanup(void);
void panda_set_os_name(char *os_name);
void panda_before_find_fast(void);
void panda_disas(FILE *out, void *code, unsigned long size);
void panda_break_main_loop(void);
MemoryRegion* panda_find_ram(void);
    
extern bool panda_exit_loop;
extern bool panda_break_vl_loop_req;


target_ulong panda_current_asid(CPUState *env);
    
target_ulong panda_current_pc(CPUState *cpu);

Int128 panda_find_max_ram_address(void);

int panda_physical_memory_rw(hwaddr addr, uint8_t *buf, int len, bool is_write);
int panda_physical_memory_read(hwaddr addr, uint8_t *buf, int len);
int panda_physical_memory_write(hwaddr addr, uint8_t *buf, int len);
hwaddr panda_virt_to_phys(CPUState * env, target_ulong addr);
bool enter_priv(CPUState * cpu);
void exit_priv(CPUState * cpu);
int panda_virtual_memory_rw(CPUState * cpu, target_ulong addr,
                            uint8_t * buf, int len, bool is_write);
int panda_virtual_memory_read(CPUState * env, target_ulong addr,
                              uint8_t * buf, int len);
int panda_virtual_memory_write(CPUState * env, target_ulong addr,
                               uint8_t * buf, int len);
void *panda_map_virt_to_host(CPUState * env, target_ulong addr, int len);

/*
 * Opt-in, non-standard: guest virtual memory that is not paged in.
 *
 * panda_virtual_memory_rw() and friends never change the guest: they translate with a debug page
 * walk and fail (-1) on a page that is not mapped (never touched, swapped out, evicted). The
 * functions below are a separate, explicitly selected path that lets the guest fault such a page
 * in, as the program would by touching it itself. That changes guest state: the guest takes a page
 * fault (and its kernel runs: page cache, swap-in, copy-on-write, a new anonymous page), and the
 * instruction it was about to run, with every PANDA callback for it, runs again.
 *
 * How to use it:
 * 1. panda_enable_vmem_fault() once, before the code you want to access memory from is translated
 *    (it flushes the translation cache). Until it is called nothing changes for anyone; after it,
 *    insn_exec and block-start callbacks are entered through a trampoline (a few instructions each).
 * 2. From a PANDA insn_exec, before_block_exec or start_block_exec callback (served by the bridge),
 *    call panda_virtual_memory_read_fault() / _write_fault() / _rw_fault(). NOT from syscalls2's
 *    on_*_enter/return or the hooks plugin's callbacks: those plugins register their own QEMU
 *    callbacks (NO_REGS, no trampoline), so the call returns PANDA_VMEM_ECONTEXT there.
 * 3. PANDA_VMEM_OK: done. PANDA_VMEM_FAULT_PENDING: a page needs the guest; nothing was read or
 *    written. Return from your callback without acting on the access. Once every callback for
 *    this event has returned, the guest takes the fault the program's own access would take
 *    (#PF, data abort, load/store page fault), resolves it, and resumes at the same instruction,
 *    so your callback (and every other one for the event) runs again; call it again then.
 *    A range over several pages may need several rounds. Callbacks must therefore tolerate
 *    running more than once for one guest instruction.
 *    This holds for EVERY plugin, not only the one that asked: while the opt-in is used, all
 *    insn_exec callbacks for that instruction (or before_block_exec/start_block_exec for that
 *    block) run again on the re-run, so counters, coverage and loggers see the event twice.
 *    A block start the fault aborts gets no end_block_exec/after_block_exec (it ran nothing);
 *    an instruction's fault ends its block early as any guest fault does (after_block_exec with
 *    TB_EXIT_REQUESTED).
 *
 * The access is made as the guest program would make it: user-mode permissions, in the current
 * address space. A write to a copy-on-write or clean page is a store fault the kernel resolves (the
 * write lands in the process's private copy); a read never writes. Kernel memory (mapped, but not
 * for user mode) is never faulted: PANDA_VMEM_ERROR, use panda_virtual_memory_rw() for it. An
 * address that the program itself could not access (no mapping, a write to a read-only mapping)
 * gets what the program would get: SIGSEGV (or, from kernel code, whatever the kernel does with a
 * user fault there; that is the caller's responsibility). If the same page faults again at the same
 * instruction without being mapped, the call returns PANDA_VMEM_ERROR instead of faulting again.
 *
 * Called from anywhere else (another callback type, syscalls2/hooks callbacks, the main loop,
 * another thread, another vCPU, before panda_enable_vmem_fault(), under KVM, or while recording or
 * replaying: the fault would not be in the log) it returns PANDA_VMEM_ECONTEXT and touches nothing:
 * there is no instruction there to re-run. Memory callbacks (virt/phys_mem_*) are not supported.
 * It does not drop the BQL and is independent of patch 0028.
 */
typedef enum panda_vmem_fault_result {
    PANDA_VMEM_OK = 0,
    PANDA_VMEM_FAULT_PENDING = 1,   /* return from the callback; it runs again after the fault */
    PANDA_VMEM_ERROR = -1,          /* not accessible to the program, and not faulted */
    PANDA_VMEM_EIO = -2,            /* not RAM (MMIO, ROM store): never accessed */
    PANDA_VMEM_ECONTEXT = -3,       /* not from a callback that may fault (see above) */
    PANDA_VMEM_EINVAL = -4,         /* len <= 0 or buf NULL */
} panda_vmem_fault_result;

/* Make insn_exec and block-start callbacks able to fault pages in (flushes translated code). */
void panda_enable_vmem_fault(void);
/* Back to the standard callbacks (flushes translated code). */
void panda_disable_vmem_fault(void);
bool panda_vmem_fault_enabled(void);
/* Read or write @len bytes at guest virtual @addr, faulting pages in: returns a
 * panda_vmem_fault_result (see above). */
int panda_virtual_memory_rw_fault(CPUState * cpu, target_ulong addr, uint8_t * buf, int len,
                                  bool is_write);
int panda_virtual_memory_read_fault(CPUState * cpu, target_ulong addr, uint8_t * buf, int len);
int panda_virtual_memory_write_fault(CPUState * cpu, target_ulong addr, uint8_t * buf, int len);
// MemTxResult PandaPhysicalAddressToRamOffset(ram_addr_t* out, hwaddr addr, bool is_write);
// MemTxResult PandaVirtualAddressToRamOffset(ram_addr_t* out, CPUState* cpu, target_ulong addr, bool is_write);

bool panda_in_kernel_mode(const CPUState *cpu);
bool panda_in_kernel(const CPUState *cpu);
bool address_in_kernel_code_linux(target_ulong addr);
bool panda_in_kernel_code_linux(CPUState * cpu);
target_ulong panda_current_ksp(CPUState * cpu);
target_ulong panda_current_sp(const CPUState *cpu);
target_ulong panda_get_retval(const CPUState *cpu);
void panda_set_retval(CPUState *cpu, target_ulong arg);
target_ulong panda_get_syscall_arg(CPUState *cpu, int arg);
void panda_set_syscall_arg(CPUState *cpu, int arg, target_ulong value);
#ifdef __cplusplus
}
#endif

/* vim:set tabstop=4 softtabstop=4 expandtab: */
