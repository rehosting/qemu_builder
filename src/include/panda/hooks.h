/*
 * PANDA hook points called from QEMU core and target code.
 *
 * With --enable-panda these are the dispatchers in panda/src/cb-support.c. Without it they are
 * empty inlines that return "no callback acted", so a default build compiles every hook site
 * away and is identical to one without PANDA.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PANDA_HOOKS_H
#define PANDA_HOOKS_H

#ifdef CONFIG_PANDA

struct qemu_plugin_tb;

void panda_callbacks_before_block_exec(CPUState *cpu, TranslationBlock *tb);
void panda_callbacks_after_block_exec(CPUState *cpu, TranslationBlock *tb, uint8_t exitCode);
void panda_callbacks_before_block_translate(CPUState *cpu, uint64_t pc);
void panda_callbacks_after_block_translate(CPUState *cpu, TranslationBlock *tb);
int32_t panda_callbacks_before_handle_exception(CPUState *cpu, int32_t exception_index);
/* true: a plugin vetoed the change, leave the old value in place */
bool panda_callbacks_asid_changed(CPUState *cpu, uint64_t oldval, uint64_t newval);
/* true: a plugin handled the hypercall and set the guest's registers itself */
bool panda_callbacks_guest_hypercall(CPUState *cpu);
/* false after panda_disable_tb_chaining(): every TB returns to cpu_exec, so the block_exec
 * callbacks there run once per TB rather than once per chain */
bool panda_tb_chaining_enabled(void);

#else

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
