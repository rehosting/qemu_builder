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