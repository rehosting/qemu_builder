#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "panda/debug.h"
#include "panda/plugin.h"
#include "panda/common.h"
#include "panda/callbacks/cb-support.h"
#include "exec/translator.h"
#include "panda/panda_qemu_plugin_helpers.h"


CPUState* panda_cpu_by_index(int index){
    CPUState *cpu = qemu_get_cpu(index);
    return cpu;
}

CPUState* panda_cpu_in_translate(void){
    return tcg_ctx->cpu;
}

TranslationBlock * panda_get_tb(struct qemu_plugin_tb *tb){
    const DisasContextBase *db = tcg_ctx->plugin_db;
    return db->tb;
}

static bool panda_has_callback_registered(panda_cb_type type){
    return panda_cbs[type] != NULL;
}

unsigned panda_bridge_wants(void){
    unsigned wants = 0;
    if (panda_has_callback_registered(PANDA_CB_INSN_TRANSLATE)) {
        wants |= PANDA_BRIDGE_INSN;
    }
    if (panda_has_callback_registered(PANDA_CB_START_BLOCK_EXEC)) {
        wants |= PANDA_BRIDGE_START_BLOCK;
    }
    if (panda_has_callback_registered(PANDA_CB_END_BLOCK_EXEC)) {
        wants |= PANDA_BRIDGE_END_BLOCK;
    }
    if (wants & (PANDA_BRIDGE_START_BLOCK | PANDA_BRIDGE_END_BLOCK)
        || panda_has_callback_registered(PANDA_CB_BEFORE_BLOCK_EXEC)
        || panda_has_callback_registered(PANDA_CB_AFTER_BLOCK_EXEC)
        || panda_has_callback_registered(PANDA_CB_BEFORE_BLOCK_EXEC_INVALIDATE_OPT)
        || panda_has_callback_registered(PANDA_CB_AFTER_BLOCK_TRANSLATE)
        || panda_has_callback_registered(PANDA_CB_BEFORE_TCG_CODEGEN)
        || panda_has_callback_registered(PANDA_CB_CPU_RESTORE_STATE)) {
        wants |= PANDA_BRIDGE_TB_PC;
    }
    return wants;
}

uint64_t panda_bridge_virt_to_phys(CPUState *cpu, uint64_t vaddr){
    hwaddr pa = panda_virt_to_phys(cpu, vaddr);
    return pa == (hwaddr) -1 ? (uint64_t) -1 : (uint64_t) pa;
}

int panda_memcb_rw(bool is_store){
    int rw = 0;
    if (is_store ? panda_has_callback_registered(PANDA_CB_VIRT_MEM_BEFORE_WRITE)
                   || panda_has_callback_registered(PANDA_CB_PHYS_MEM_BEFORE_WRITE)
                 : panda_has_callback_registered(PANDA_CB_VIRT_MEM_BEFORE_READ)
                   || panda_has_callback_registered(PANDA_CB_PHYS_MEM_BEFORE_READ)) {
        rw |= QEMU_PLUGIN_MEM_BEFORE;
    }
    if (is_store ? panda_has_callback_registered(PANDA_CB_VIRT_MEM_AFTER_WRITE)
                   || panda_has_callback_registered(PANDA_CB_PHYS_MEM_AFTER_WRITE)
                 : panda_has_callback_registered(PANDA_CB_VIRT_MEM_AFTER_READ)
                   || panda_has_callback_registered(PANDA_CB_PHYS_MEM_AFTER_READ)) {
        rw |= QEMU_PLUGIN_MEM_AFTER;
    }
    return rw ? rw | (is_store ? QEMU_PLUGIN_MEM_W : QEMU_PLUGIN_MEM_R) : 0;
}

bool panda_memcb_phys_before(bool is_store){
    return panda_has_callback_registered(is_store ? PANDA_CB_PHYS_MEM_BEFORE_WRITE
                                                  : PANDA_CB_PHYS_MEM_BEFORE_READ);
}

#define PANDA_END_SLOTS 1024
static TranslationBlock *pending_end[PANDA_END_SLOTS];

void panda_end_block_flush(CPUState *cpu){
    if (cpu->cpu_index < PANDA_END_SLOTS) {
        TranslationBlock *tb = pending_end[cpu->cpu_index];
        if (unlikely(tb)) {
            pending_end[cpu->cpu_index] = NULL;
            panda_callbacks_end_block_exec(cpu, tb);
        }
    }
}

void panda_end_block_defer(CPUState *cpu, TranslationBlock *tb){
    if (cpu->cpu_index < PANDA_END_SLOTS) {
        pending_end[cpu->cpu_index] = tb;
    } else {
        panda_callbacks_end_block_exec(cpu, tb);   // no slot: before the block, as it used to
    }
}

void panda_end_block_drop_all(void){
    memset(pending_end, 0, sizeof(pending_end));
}
