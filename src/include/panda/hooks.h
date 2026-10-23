/*
 * PANDA hook points called from QEMU core and target code.
 *
 * The PANDA runtime (panda/src, libpanda-<target>.so) is a component: the engine does not link it.
 * With --enable-panda each hook site below is an inline that tests one pointer, panda_hooks, and
 * calls through the runtime's table when one is installed (panda_hooks_install(), from the runtime's
 * constructor when it is loaded into the engine). No runtime loaded: every site is a load and a
 * not-taken branch, and returns "no callback acted".
 *
 * Without --enable-panda they are empty inlines and the hook sites compile away.
 *
 * The runtime itself is built with PANDA_RUNTIME defined: it sees the real functions (defined in
 * panda/src/cb-support.c and friends), which it puts in its table.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PANDA_HOOKS_H
#define PANDA_HOOKS_H

#ifdef CONFIG_PANDA

#include "qemu/atomic.h"

/*
 * The table a PANDA runtime installs. Bump PANDA_HOOKS_VERSION when a member is added, removed or
 * changes signature; append new members at the end. The engine refuses a table whose version or
 * size differs from its own, and a runtime built against a different engine ABI (panda_hooks_install).
 */
#define PANDA_HOOKS_VERSION 1

typedef struct PandaHooks {
    uint32_t version;   /* PANDA_HOOKS_VERSION */
    uint32_t size;      /* sizeof(PandaHooks) */
    void (*before_block_exec)(CPUState *cpu, TranslationBlock *tb);
    void (*after_block_exec)(CPUState *cpu, TranslationBlock *tb, uint8_t exitCode);
    void (*before_block_translate)(CPUState *cpu, uint64_t pc);
    void (*after_block_translate)(CPUState *cpu, TranslationBlock *tb);
    int32_t (*before_handle_exception)(CPUState *cpu, int32_t exception_index);
    bool (*asid_changed)(CPUState *cpu, uint64_t oldval, uint64_t newval);
    bool (*guest_hypercall)(CPUState *cpu);
    void (*block_longjmp)(CPUState *cpu);
    void (*before_find_fast)(void);
    void (*main_loop_wait)(void);
    void (*after_loadvm)(CPUState *env);
} PandaHooks;

/* Installed table, NULL until a runtime is loaded. Engine side: panda/hooks.c. */
extern const PandaHooks *panda_hooks;
/* panda_disable_tb_chaining(): every TB returns to cpu_exec (read by curr_cflags; patch 0046) */
extern bool panda_hooks_nochain;

/*
 * Called by the runtime when it is loaded. abi is the panda ABI hash the runtime was compiled
 * against (panda-abi.h). Fails (and installs nothing) on a second runtime, a table of another
 * version or size, or another ABI; PANDA_MODULE_ABI=ignore makes the ABI check a warning.
 */
bool panda_hooks_install(const PandaHooks *hooks, const char *abi, Error **errp);
/* The engine's panda ABI hash. */
const char *panda_hooks_abi(void);

#ifdef PANDA_RUNTIME

/* The real functions are defined in panda/src and declared in panda/callbacks/cb-support.h,
 * except these: */
/* false after panda_disable_tb_chaining(): every TB returns to cpu_exec. The block-exec callbacks
 * don't need it (they run from the block's own code); kept for plugins that want it. */
bool panda_tb_chaining_enabled(void);
/* A longjmp left the block a vCPU was running (a fault, an exception its last instruction raised,
 * cpu_loop_exit_noexc): its after_block_exec runs now, with TB_EXIT_REQUESTED, before the exception
 * is delivered. Nothing pending (the usual case: no block-exec callback), nothing done. */
void panda_callbacks_block_longjmp(CPUState *cpu);

#else /* the engine: dispatch through the installed table */

#define PANDA_HOOK(h) const PandaHooks *h = qatomic_read(&panda_hooks); if (unlikely(h))

static inline void panda_callbacks_before_block_exec(CPUState *cpu, TranslationBlock *tb)
{
    PANDA_HOOK(h) { h->before_block_exec(cpu, tb); }
}
static inline void panda_callbacks_after_block_exec(CPUState *cpu, TranslationBlock *tb,
                                                    uint8_t exitCode)
{
    PANDA_HOOK(h) { h->after_block_exec(cpu, tb, exitCode); }
}
static inline void panda_callbacks_before_block_translate(CPUState *cpu, uint64_t pc)
{
    PANDA_HOOK(h) { h->before_block_translate(cpu, pc); }
}
static inline void panda_callbacks_after_block_translate(CPUState *cpu, TranslationBlock *tb)
{
    PANDA_HOOK(h) { h->after_block_translate(cpu, tb); }
}
static inline int32_t panda_callbacks_before_handle_exception(CPUState *cpu,
                                                              int32_t exception_index)
{
    PANDA_HOOK(h) { return h->before_handle_exception(cpu, exception_index); }
    return exception_index;
}
static inline bool panda_tb_chaining_enabled(void)
{
    return likely(!qatomic_read(&panda_hooks_nochain));
}
static inline void panda_callbacks_block_longjmp(CPUState *cpu)
{
    PANDA_HOOK(h) { h->block_longjmp(cpu); }
}
static inline void panda_callbacks_before_find_fast(void)
{
    PANDA_HOOK(h) { h->before_find_fast(); }
}
static inline void panda_callbacks_main_loop_wait(void)
{
    PANDA_HOOK(h) { h->main_loop_wait(); }
}
static inline void panda_callbacks_after_loadvm(CPUState *env)
{
    PANDA_HOOK(h) { h->after_loadvm(env); }
}
static inline bool panda_callbacks_asid_changed(CPUState *cpu, uint64_t oldval, uint64_t newval)
{
    PANDA_HOOK(h) { return h->asid_changed(cpu, oldval, newval); }
    return false;
}
static inline bool panda_callbacks_guest_hypercall(CPUState *cpu)
{
    PANDA_HOOK(h) { return h->guest_hypercall(cpu); }
    return false;
}

#undef PANDA_HOOK

#endif /* PANDA_RUNTIME */

#else /* !CONFIG_PANDA */

static inline void panda_callbacks_before_block_exec(CPUState *cpu, TranslationBlock *tb) {}
static inline void panda_callbacks_after_block_exec(CPUState *cpu, TranslationBlock *tb,
                                                    uint8_t exitCode) {}
static inline void panda_callbacks_before_block_translate(CPUState *cpu, uint64_t pc) {}
static inline void panda_callbacks_after_block_translate(CPUState *cpu, TranslationBlock *tb) {}
static inline int32_t panda_callbacks_before_handle_exception(CPUState *cpu,
                                                              int32_t exception_index)
{
    return exception_index;
}
static inline bool panda_tb_chaining_enabled(void)
{
    return true;
}
static inline void panda_callbacks_block_longjmp(CPUState *cpu) {}
static inline void panda_callbacks_before_find_fast(void) {}
static inline void panda_callbacks_main_loop_wait(void) {}
static inline void panda_callbacks_after_loadvm(CPUState *env) {}
static inline bool panda_callbacks_asid_changed(CPUState *cpu, uint64_t oldval, uint64_t newval)
{
    return false;
}
static inline bool panda_callbacks_guest_hypercall(CPUState *cpu)
{
    return false;
}

#endif

#endif
