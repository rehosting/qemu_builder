struct qemu_plugin_tb;
CPUState *panda_cpu_by_index(int index);
CPUState *panda_cpu_in_translate(void);
TranslationBlock *panda_get_tb(struct qemu_plugin_tb *tb);

/* What the TCG plugin bridge instruments a TB for, from the PANDA callbacks registered (enabled or
 * not: enabling one later needs no retranslation). Nothing registered, nothing instrumented. */
#define PANDA_BRIDGE_INSN        1  /* insn_translate (which asks for insn_exec) */
#define PANDA_BRIDGE_START_BLOCK 2
#define PANDA_BRIDGE_END_BLOCK   4
#define PANDA_BRIDGE_TB_PC       8  /* a callback gets the TB: bind it so tb->pc is its address */
unsigned panda_bridge_wants(void);
/* The qemu_plugin_mem_rw (direction and phases) to register for reads (is_store false) or
 * writes; 0 for none. */
int panda_memcb_rw(bool is_store);
/* The physical address of @vaddr by a page walk, or -1: for a before-access event, whose page
 * may not be in the TLB yet (the plugin API then has no hwaddr). */
uint64_t panda_bridge_virt_to_phys(CPUState *cpu, uint64_t vaddr);
/* end_block_exec runs after a block's last instruction. QEMU has no hook there, so the block a vCPU
 * starts is kept pending (panda_end_block_defer, from the bridge's block start) and its
 * end_block_exec runs at whatever the vCPU does next (panda_end_block_flush): start another block,
 * chained or not; return from the TB (after_block_exec); take an exception
 * (before_handle_exception); go idle or exit. The CPU state is then the block's final state. A
 * code cache flush drops what is pending: its TBs are gone. */
void panda_end_block_defer(CPUState *cpu, TranslationBlock *tb);
void panda_end_block_flush(CPUState *cpu);
void panda_end_block_drop_all(void);
