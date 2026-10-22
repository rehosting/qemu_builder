#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "panda/debug.h"
#include "panda/plugin.h"
#include "panda/common.h"
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
