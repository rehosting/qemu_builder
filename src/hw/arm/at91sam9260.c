/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Atmel AT91SAM9260 (ARM926EJ-S) — minimal board for kernel-lift Tier-A
 * faithful-binary rehosting. Boots an UNMODIFIED vendor kernel far enough for
 * live-memory introspection (init_task) and, with the PIT+AIC timer/interrupt
 * model below, toward userspace.
 *
 * Synthesized peripherals (system-controller window 0xFFFFE800-0xFFFFFFFF):
 *   - DBGU (0xFFFFF200): SR TX-ready; CIDR = AT91SAM9260 chip id (SoC detect);
 *     THR writes -> console.
 *   - PMC  (0xFFFFFC00): SR all-ready, MCFR main-clock settled.
 *   - AIC  (0xFFFFF000): a minimal interrupt controller driving the CPU IRQ.
 *   - PIT  (0xFFFFFD30): periodic timer raising the system interrupt (AIC src 1)
 *     so jiffies advance past calibrate_delay.
 * Everything else reads 0 (ignore_memory_transaction_failures keeps strays
 * non-fatal). Mirrors kernel_lift.boot.at91sam9260 on the introspection side.
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "hw/core/sysbus.h"
#include "hw/net/cadence_gem.h"
#include "net/net.h"
#include "qemu/host-utils.h"
#include "system/system.h"
#include "system/address-spaces.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "system/penguin.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/cpu.h"
#include "qom/object.h"
#include "qemu/log.h"

#define AT91_SDRAM_BASE     0x20000000
/* Cover the whole APB peripheral tail: TC/USARTs (0xFFFA_0000+) through the
 * system controller. The vendor console is an atmel_serial USART at 0xFFFBxxxx,
 * below the old 0xFFFFE800 window. */
#define AT91_SYSC_BASE      0xFFFA0000
#define AT91_SYSC_SIZE      (0x100000000ULL - AT91_SYSC_BASE)
#define AT91_SYSC_TOP       0xFFFFE800   /* sysc (AIC/DBGU/PMC/PIT) at/above here */

/* EMAC (macb) — enough of the MDIO handshake that the PHY scan terminates. */
#define AT91_MACB           0xFFFC4000
#define MACB_NSR            0x08
#define MACB_MAN            0x34
#define MACB_NSR_MDIO       0x02   /* MDIO not busy */
#define MACB_NSR_IDLE       0x04   /* PHY management idle: mdio xfer complete */
#define MACH_TYPE_AT91SAM9260 1099

/* atmel_serial USART registers (per-port, 0x4000 apart, below AT91_SYSC_TOP). */
#define US_IER 0x08   /* interrupt enable */
#define US_IDR 0x0C   /* interrupt disable */
#define US_IMR 0x10   /* interrupt mask (read) */
#define US_CSR 0x14   /* channel status: TXRDY=1<<1, TXEMPTY=1<<9 */
#define US_THR 0x1C   /* transmit holding */
#define US_TXRDY   (1u << 1)
#define US_ENDTX   (1u << 4)   /* PDC end of transmit */
#define US_TXEMPTY (1u << 9)
#define US_TXBUFE  (1u << 11)  /* PDC transmit buffer empty */
/* The vendor atmel_serial uses the PDC (peripheral DMA) for TX: it writes the
 * console buffer's physical address to US_TPR + byte count to US_TCR and starts
 * the transfer via US_PTCR(TXTEN); completion raises ENDTX|TXBUFE (tx_done_mask)
 * — NOT TXRDY. So userspace console output only appears if the PDC is modeled:
 * on TXTEN we read the buffer from guest RAM, emit it, and raise ENDTX|TXBUFE. */
#define US_CR   0x00   /* control (write-only): RSTSTA, STTTO, ... */
#define US_RHR  0x18   /* receive holding register */
#define US_RPR  0x100
#define US_RCR  0x104
#define US_TPR  0x108
#define US_TCR  0x10C
#define US_RNPR 0x110
#define US_RNCR 0x114
#define US_TNPR 0x118
#define US_TNCR 0x11C
#define US_PTCR 0x120
#define US_PTSR 0x124
#define PDC_RXTEN  (1u << 0)
#define PDC_RXTDIS (1u << 1)
#define PDC_TXTEN  (1u << 8)
#define PDC_TXTDIS (1u << 9)
#define US_RXRDY   (1u << 0)
#define US_ENDRX   (1u << 3)   /* PDC end of receive */
#define US_TIMEOUT (1u << 8)   /* receiver idle timeout */
#define US_CR_RSTSTA (1u << 8) /* reset status bits */
#define US_CR_STTTO  (1u << 11)/* start timeout (clears TIMEOUT) */

#define AT91_AIC    0xFFFFF000
#define AT91_DBGU   0xFFFFF200
#define AT91_PMC    0xFFFFFC00
#define AT91_PIT    0xFFFFFD30

/* PIOC (0xFFFFF800): the NAND ready/busy pin (rdy_pin=77=PC13) is read here via
 * PIO_PDSR. Report it high so atmel_nand_device_ready() sees the chip ready. */
#define AT91_PIOC       0xFFFFF800
#define PIO_PDSR        0x3C
#define NAND_RDY_BIT    (1u << 13)   /* PC13 */

/* NAND flash on EBI CS3 (0x40000000). Board wiring: CLE = addr bit 22, ALE =
 * addr bit 21 (from atmel_nand_data.cle/ale = 22/21). Small-page chip: 512B
 * page + 16B OOB, 16KiB erase block, 32MiB total, 8-bit — matches the JFFS2
 * root image's 0x4000 erase spacing. ECC is Atmel HW ECC, but the ECC status
 * register (0xFFFFE800+0x08) reads 0 (no error) so raw page data is accepted. */
#define AT91_NAND_BASE  0x40000000
#define AT91_NAND_WIN   0x00800000   /* covers data / ALE(0x200000) / CLE(0x400000) */
#define NAND_CLE_BIT    (1u << 22)
#define NAND_ALE_BIT    (1u << 21)
#define NAND_PAGE       512
#define NAND_OOB        16
#define NAND_ERASE      0x4000
#define NAND_SIZE       (32 * 1024 * 1024)
#define NAND_ID_MAKER   0xEC         /* Samsung */
#define NAND_ID_DEVICE  0x75         /* "NAND 32MiB 3,3V 8-bit" (nand_flash_ids) */
#define NAND_ROOT_OFF   0x280000     /* partition[4] "root" (JFFS2) offset in NAND */

/* NAND command opcodes */
#define NC_READ0    0x00
#define NC_READ1    0x01
#define NC_READOOB  0x50
#define NC_READID   0x90
#define NC_RESET    0xFF
#define NC_STATUS   0x70
#define NC_SEQIN    0x80
#define NC_PAGEPROG 0x10
#define NC_ERASE1   0x60
#define NC_ERASE2   0xD0
#define NC_PARAM    0xEC
#define NAND_STATUS_OK 0xC0   /* READY | ~WP */

/* DBGU */
#define DBGU_SR 0x14
#define DBGU_THR 0x1C
#define DBGU_CIDR 0x40
#define DBGU_TXRDY (1u << 1)
#define DBGU_TXEMPTY (1u << 9)
#define CIDR_AT91SAM9260 0x019803A0u

/* PMC */
#define PMC_MCFR 0x24
#define PMC_PLLAR 0x28
#define PMC_PLLBR 0x2c
#define PMC_MCKR  0x30
/* Bootloader-programmed clock tree the kernel READS (it doesn't reprogram PLLA
 * on at91sam9260). main=18.432MHz; PLLA = main/DIV*(MUL+1) with DIV=1,MUL=9 =
 * 184.32MHz; MCKR: CSS=PLLA, PRES=1, MDIV=/2 => MCK = 92.16MHz. A nonzero MCK
 * is what atmel_serial needs so uart_update_timeout doesn't divide by zero. */
#define PMC_PLLAR_INIT 0x00090001u
#define PMC_MCKR_INIT  0x00000102u
#define PMC_SR   0x68
#define PMC_MCFR_MAINRDY (1u << 16)
#define MAIN_HZ 18432000u

/* AIC register offsets */
#define AIC_SMR0 0x000   /* .. 0x7c : 32 source-mode regs */
#define AIC_SVR0 0x080   /* .. 0xfc : 32 source-vector regs */
#define AIC_IVR  0x100
#define AIC_ISR  0x108
#define AIC_IPR  0x10c
#define AIC_IMR  0x110
#define AIC_CISR 0x114
#define AIC_IECR 0x120
#define AIC_IDCR 0x124
#define AIC_ICCR 0x128
#define AIC_ISCR 0x12c
#define AIC_EOICR 0x130
#define AIC_SPU  0x134

/* PIT register offsets (from PIT base) */
#define PIT_MR   0x00
#define PIT_SR   0x04
#define PIT_PIVR 0x08
#define PIT_PIIR 0x0c
#define PIT_MR_PITEN  (1u << 24)
#define PIT_MR_PITIEN (1u << 25)
#define PIT_SR_PITS   (1u << 0)

#define IRQ_SYS 1   /* system-controller interrupt (PIT lives here) */

typedef struct AT91State {
    MemoryRegion sysc;
    qemu_irq cpu_irq;
    QEMUTimer *pit_timer;

    /* AIC */
    uint32_t aic_svr[32];
    uint32_t aic_ipr;   /* pending */
    uint32_t aic_imr;   /* mask (1 = enabled) */
    uint32_t aic_cur_src; /* source number IVR last returned (read via ISR) */
    uint32_t aic_spu;   /* spurious vector (returned by IVR when idle) */
    bool aic_in_service;

    /* PIT */
    uint32_t pit_mr;
    uint32_t pit_pits;    /* PITS status */
    uint32_t pit_picnt;   /* whole periods pending since last PIVR read */
    int64_t pit_last_tick_ns; /* when the last periodic tick fired */

    /* PMC: track PLLA/PLLB programming so SR lock bits follow enable/disable. */
    uint32_t pmc_pllar;
    uint32_t pmc_pllbr;
    uint32_t pmc_mckr;

    /* USART: interrupt mask + PDC (DMA) TX/RX state per port. Completion flags
     * (ENDTX|TXBUFE|ENDRX|TIMEOUT) latched in us_csr_int drive the AIC source.
     * USART0 (console) is wired to a CharBackend for interactive TX+RX. */
    CharFrontend cons;         /* USART0 console chardev */
    uint32_t us_imr[6];
    uint32_t us_csr_int[6];   /* latched CSR interrupt flags */
    uint32_t us_tpr[6];       /* PDC transmit pointer (guest phys addr) */
    uint32_t us_tcr[6];       /* PDC transmit counter */
    uint32_t us_rpr[6];       /* PDC receive pointer (guest phys addr) */
    uint32_t us_rcr[6];       /* PDC receive counter */
    uint32_t us_rnpr[6];      /* PDC receive next pointer */
    uint32_t us_rncr[6];      /* PDC receive next counter */
    uint32_t us_ptsr[6];      /* PDC status (TXTEN/RXTEN) */

    /* NAND controller state machine (small-page, see defines above). */
    MemoryRegion nand;
    uint8_t *nand_store;      /* NAND_SIZE backing (0xFF + JFFS2 at root offset) */
    uint8_t  nand_cmd;        /* command latched via CLE */
    uint8_t  nand_prev;       /* previous command (for ERASE1->ERASE2 sequencing) */
    int      nand_acycle;     /* address-cycle index for the current command */
    uint32_t nand_col;        /* column address (READ path) */
    uint32_t nand_row;        /* row (page) address (READ path) */
    uint32_t nand_off;        /* byte cursor within page (data then OOB) for reads */
    /* DEDICATED program-path address latches: a page program is SEQIN -> addr ->
     * data -> PAGEPROG, but the atmel driver can issue a READ (status/verify)
     * mid-sequence which would clobber shared nand_row/col/off and make PAGEPROG
     * commit to the wrong page (data corruption surfacing as jffs2 GC crashes
     * under write load). Keep the program target separate from the read cursor. */
    uint32_t nand_prog_col;   /* column latched at SEQIN */
    uint32_t nand_prog_row;   /* row latched at SEQIN */
    uint32_t nand_prog_off;   /* data cursor for the SEQIN program buffer */
    uint32_t nand_prog_half;  /* small-page half selected by the pre-SEQIN pointer
                                 cmd: 0 (READ0/0-255), 256 (READ1), 512 (READOOB) */
    uint32_t nand_erase_row;  /* row latched at ERASE1 (dedicated, same reason) */
    int      nand_idaddr;     /* READID address byte (0x00 real id, 0x20 ONFI) */
    int      nand_idptr;      /* READID byte pointer */
    uint8_t  nand_prog[NAND_PAGE + NAND_OOB];  /* SEQIN program buffer */
} AT91State;

/* PMC_SR: LOCKA/LOCKB reflect whether each PLL is programmed on (MUL!=0), so
 * both the lock-wait on enable AND the unlock-wait on disable terminate. MOSCS
 * and the high ready bits (PCKRDYx etc.) stay set so the other clk waits pass. */
#define PMC_SR_MOSCS  (1u << 0)
#define PMC_SR_LOCKA  (1u << 1)
#define PMC_SR_LOCKB  (1u << 2)
static uint32_t pmc_sr(AT91State *s)
{
    uint32_t sr = 0xFFFFFFF8u | PMC_SR_MOSCS;   /* all ready bits except LOCKA/B */
    if ((s->pmc_pllar >> 16) & 0x7ff) sr |= PMC_SR_LOCKA;
    if ((s->pmc_pllbr >> 16) & 0x7ff) sr |= PMC_SR_LOCKB;
    return sr;
}

/* CPIV (low 20 bits): position within the current period, advancing with
 * virtual time, so the clocksource (read_pit_clk -> PIIR) actually moves. One
 * period == PIT_PERIOD_NS (the tick interval). */
#define PIT_PERIOD_NS (10 * 1000000LL)
static uint32_t pit_cpiv(AT91State *s)
{
    uint32_t piv = s->pit_mr & 0xfffff;
    int64_t dt = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->pit_last_tick_ns;
    if (dt < 0) dt = 0;
    uint32_t c = (uint32_t)((dt * (piv + 1)) / PIT_PERIOD_NS);
    return c > piv ? piv : c;
}

static void aic_update(AT91State *s)
{
    bool assert_irq = !s->aic_in_service && (s->aic_ipr & s->aic_imr) != 0;
    qemu_set_irq(s->cpu_irq, assert_irq);
}

/* EMAC (Cadence GEM / macb) interrupt = AIC source 21, level-sensitive: the
 * GEM drives this line from its own ISR&IMR, so mirror the level into the AIC
 * pending register and re-evaluate. */
#define AT91_EMAC_SRC 21
static void at91_emac_set_irq(void *opaque, int n, int level)
{
    AT91State *s = opaque;
    if (level) {
        s->aic_ipr |= (1u << AT91_EMAC_SRC);
    } else {
        s->aic_ipr &= ~(1u << AT91_EMAC_SRC);
    }
    aic_update(s);
}

/* USART port index / AIC source for a base address (0x4000-aligned). */
static int usart_index(hwaddr base)
{
    switch (base) {
    case 0xFFFB0000: return 0;   /* USART0 (ttyS0, console) */
    case 0xFFFB4000: return 1;   /* USART1 */
    case 0xFFFB8000: return 2;   /* USART2 */
    case 0xFFFD0000: return 3;   /* USART3 */
    case 0xFFFD4000: return 4;   /* USART4 */
    case 0xFFFD8000: return 5;   /* USART5 */
    default: return -1;
    }
}
static const int usart_src_tbl[6] = { 6, 7, 8, 23, 24, 25 };

/* Assert the port's AIC source when any unmasked CSR interrupt is live. TXRDY/
 * TXEMPTY are always set (idle transmitter); ENDTX/TXBUFE (TX) and ENDRX/
 * TIMEOUT/RXRDY (RX) are latched in us_csr_int by the PDC engine. */
static void usart_update(AT91State *s, int idx)
{
    uint32_t csr = US_TXRDY | US_TXEMPTY | s->us_csr_int[idx];
    uint32_t bit = 1u << usart_src_tbl[idx];
    if (s->us_imr[idx] & csr) {
        s->aic_ipr |= bit;
    } else {
        s->aic_ipr &= ~bit;
    }
    aic_update(s);
}

/* Emit console bytes: USART0 through its CharBackend (interactive), any other
 * port to stdout (best-effort trace). */
static void usart_emit(AT91State *s, int idx, const uint8_t *buf, uint32_t n)
{
    if (idx == 0 && qemu_chr_fe_backend_connected(&s->cons)) {
        qemu_chr_fe_write_all(&s->cons, buf, n);
    } else {
        for (uint32_t i = 0; i < n; i++) {
            putchar(buf[i]);
        }
        fflush(stdout);
    }
}

/* Run the PDC transmit: emit the counted buffer straight from guest RAM to the
 * console, then latch ENDTX|TXBUFE so the driver's TX ISR/tasklet advances. */
static void usart_pdc_tx(AT91State *s, int idx)
{
    if (!(s->us_ptsr[idx] & PDC_TXTEN)) {
        return;
    }
    uint32_t n = s->us_tcr[idx];
    if (n) {                              /* a counted buffer: emit it */
        uint8_t buf[256];
        uint32_t addr = s->us_tpr[idx];
        while (n) {
            uint32_t chunk = n > sizeof(buf) ? sizeof(buf) : n;
            address_space_read(&address_space_memory, addr,
                               MEMTXATTRS_UNSPECIFIED, buf, chunk);
            usart_emit(s, idx, buf, chunk);
            addr += chunk; n -= chunk;
        }
        s->us_tpr[idx] += s->us_tcr[idx];
        s->us_tcr[idx] = 0;
    }
    /* TCR is now 0 (either it was, or we just drained it): the transmit buffer
     * is empty, so ENDTX|TXBUFE are set. With TCR==0 at TXTEN time this is what
     * kicks the driver's ISR/tasklet to program the first real transfer. */
    s->us_csr_int[idx] |= US_ENDTX | US_TXBUFE;
    usart_update(s, idx);
}

/* CharBackend can-receive: report the full PDC RX buffer room so the chardev
 * hands us a whole burst per callback (one DMA + one TIMEOUT), instead of one
 * byte per interrupt round-trip — orders of magnitude faster console input. */
static int cons_can_rx(void *opaque)
{
    AT91State *s = opaque;
    if (!(s->us_ptsr[0] & PDC_RXTEN)) {
        return 0;
    }
    uint32_t room = s->us_rcr[0];
    return room > 256 ? 256 : room;
}

/* CharBackend receive: DMA each byte into the PDC RX buffer in guest RAM and
 * latch TIMEOUT (idle) so the driver's RX path reads it promptly; latch ENDRX
 * and swap to the next buffer when the current one fills. */
static void cons_rx(void *opaque, const uint8_t *buf, int size)
{
    AT91State *s = opaque;
    for (int i = 0; i < size; i++) {
        if (!(s->us_ptsr[0] & PDC_RXTEN) || s->us_rcr[0] == 0) {
            break;
        }
        address_space_write(&address_space_memory, s->us_rpr[0],
                            MEMTXATTRS_UNSPECIFIED, &buf[i], 1);
        s->us_rpr[0]++;
        s->us_rcr[0]--;
        if (s->us_rcr[0] == 0) {           /* buffer full: ENDRX + load next */
            s->us_csr_int[0] |= US_ENDRX;
            s->us_rpr[0] = s->us_rnpr[0];
            s->us_rcr[0] = s->us_rncr[0];
            s->us_rncr[0] = 0;
        }
    }
    s->us_csr_int[0] |= US_TIMEOUT;        /* RX idle after this burst */
    usart_update(s, 0);
}

static void pit_rearm(AT91State *s)
{
    if (s->pit_mr & PIT_MR_PITEN) {
        /* ~10ms tick in virtual time; enough for jiffies to advance. */
        timer_mod(s->pit_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 10 * 1000000LL);
    }
}

static void pit_tick(void *opaque)
{
    AT91State *s = opaque;
    s->pit_pits = PIT_SR_PITS;
    s->pit_last_tick_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->pit_picnt < 0xfff) {
        s->pit_picnt++;
    }
    if (s->pit_mr & PIT_MR_PITIEN) {
        s->aic_ipr |= (1u << IRQ_SYS);
        aic_update(s);
    }
    pit_rearm(s);
}

/* ---- NAND controller (small-page state machine) ------------------------- */

/* Command latched on a CLE strobe. */
static void nand_cle(AT91State *s, uint8_t cmd)
{
    uint8_t prev = s->nand_cmd;   /* command latched just before this one */
    s->nand_cmd = cmd;
    s->nand_acycle = 0;
    switch (cmd) {
    case NC_RESET:
        s->nand_off = 0; s->nand_idptr = 0; s->nand_idaddr = 0;
        break;
    case NC_READID:
        s->nand_idptr = 0; s->nand_idaddr = 0;
        break;
    case NC_SEQIN:
        memset(s->nand_prog, 0xFF, sizeof(s->nand_prog));
        s->nand_prog_col = 0; s->nand_prog_row = 0; s->nand_prog_off = 0;
        /* small-page program: the driver sends a pointer cmd (READ0/READ1/
         * READOOB) immediately before SEQIN to select the page third, then an
         * ALREADY-ADJUSTED column byte (col-256 for READ1, col-512 for READOOB).
         * Re-add that base so second-half/OOB writes land at the right offset;
         * otherwise the driver's write-verify readback mismatches -> retlen 0. */
        if (prev == NC_READ1)        s->nand_prog_half = 256;
        else if (prev == NC_READOOB) s->nand_prog_half = NAND_PAGE;
        else                         s->nand_prog_half = 0;
        break;
    case NC_PAGEPROG: {                 /* commit program buffer (AND semantics) */
        uint64_t base = (uint64_t)s->nand_prog_row * NAND_PAGE;  /* dedicated latch */
        for (int i = 0; i < NAND_PAGE; i++) {
            uint64_t a = base + i;
            if (a < NAND_SIZE) s->nand_store[a] &= s->nand_prog[i];
        }
        break;
    }
    case NC_ERASE2: {                   /* erase the block holding nand_erase_row */
        uint64_t base = (uint64_t)s->nand_erase_row * NAND_PAGE;
        uint64_t blk = base & ~((uint64_t)NAND_ERASE - 1);
        if (blk + NAND_ERASE <= NAND_SIZE)
            memset(s->nand_store + blk, 0xFF, NAND_ERASE);
        break;
    }
    default:
        break;
    }
}

/* Address byte on an ALE strobe. Format depends on the latched command. */
static void nand_ale(AT91State *s, uint8_t b)
{
    switch (s->nand_cmd) {
    case NC_READID:
        s->nand_idaddr = b; s->nand_idptr = 0;
        break;
    case NC_READ0: case NC_READ1: case NC_READOOB:
        if (s->nand_acycle == 0)      s->nand_col = b;
        else if (s->nand_acycle == 1) s->nand_row = b;
        else if (s->nand_acycle == 2) s->nand_row |= (uint32_t)b << 8;
        else                          s->nand_row |= (uint32_t)b << 16;
        s->nand_acycle++;
        {
            uint32_t base = s->nand_col;
            if (s->nand_cmd == NC_READ1)        base += 256;
            else if (s->nand_cmd == NC_READOOB) base += NAND_PAGE;
            s->nand_off = base;
        }
        break;
    case NC_SEQIN:   /* program-path addressing -> dedicated latches */
        if (s->nand_acycle == 0)      s->nand_prog_col = b;
        else if (s->nand_acycle == 1) s->nand_prog_row = b;
        else if (s->nand_acycle == 2) s->nand_prog_row |= (uint32_t)b << 8;
        else                          s->nand_prog_row |= (uint32_t)b << 16;
        s->nand_acycle++;
        s->nand_prog_off = s->nand_prog_half + s->nand_prog_col;  /* half-adjusted */
        break;
    case NC_ERASE1:   /* erase-path addressing -> dedicated latch */
        if (s->nand_acycle == 0)      s->nand_erase_row = b;
        else if (s->nand_acycle == 1) s->nand_erase_row |= (uint32_t)b << 8;
        else                          s->nand_erase_row |= (uint32_t)b << 16;
        s->nand_acycle++;
        break;
    default:
        break;
    }
}

/* One data byte read from the chip. */
static uint8_t nand_din(AT91State *s)
{
    switch (s->nand_cmd) {
    case NC_READID: {
        uint8_t id0 = 0, id1 = 0;
        if (s->nand_idaddr == 0x00) { id0 = NAND_ID_MAKER; id1 = NAND_ID_DEVICE; }
        /* idaddr 0x20/0x40 -> zeros: reports neither ONFI nor JEDEC */
        uint8_t v = (s->nand_idptr == 0) ? id0 : (s->nand_idptr == 1) ? id1 : 0;
        s->nand_idptr++;
        return v;
    }
    case NC_STATUS:
        return NAND_STATUS_OK;
    case NC_READ0: case NC_READ1: case NC_READOOB: {
        uint64_t pbase = (uint64_t)s->nand_row * NAND_PAGE;
        uint32_t off = s->nand_off++;
        if (off < NAND_PAGE) {
            uint64_t a = pbase + off;
            return (a < NAND_SIZE) ? s->nand_store[a] : 0xFF;
        }
        return 0xFF;   /* OOB region: 0xFF (good-block marker; ECC unused) */
    }
    default:
        return 0xFF;
    }
}

/* One data byte written to the chip (SEQIN program phase). */
static void nand_dout(AT91State *s, uint8_t v)
{
    if (s->nand_cmd == NC_SEQIN) {
        uint32_t off = s->nand_prog_off++;
        if (off < NAND_PAGE + NAND_OOB) s->nand_prog[off] = v;
    }
}

static uint64_t nand_mm_read(void *opaque, hwaddr off, unsigned size)
{
    return nand_din((AT91State *)opaque);
}

static void nand_mm_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    AT91State *s = opaque;
    if (off & NAND_CLE_BIT)      nand_cle(s, val & 0xFF);
    else if (off & NAND_ALE_BIT) nand_ale(s, val & 0xFF);
    else                         nand_dout(s, val & 0xFF);
}

static const MemoryRegionOps nand_ops = {
    .read = nand_mm_read,
    .write = nand_mm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static uint64_t at91_read(void *opaque, hwaddr off, unsigned size)
{
    AT91State *s = opaque;
    hwaddr addr = AT91_SYSC_BASE + off;

    /* PIOC pin-data status: NAND ready/busy pin (PC13) reads high (ready). */
    if (addr == AT91_PIOC + PIO_PDSR) {
        return NAND_RDY_BIT;
    }

    /* EMAC (macb) at 0xFFFC4000 is a real Cadence GEM device (instantiated in
     * at91sam9260_init, overlapping this window at higher priority), so reads
     * here never reach the sysc handler. */

    /* USART/TC region below the system controller: report the console UART
     * always ready to transmit (CSR TXRDY|TXEMPTY) so atmel_console_putchar
     * doesn't spin. */
    if (addr < AT91_SYSC_TOP) {
        uint32_t reg = addr & 0x3fff;
        int idx = usart_index(addr & ~0x3fffULL);
        if (reg == US_CSR) {
            uint32_t base = DBGU_TXRDY | DBGU_TXEMPTY;
            return (idx >= 0) ? (base | s->us_csr_int[idx]) : base;
        }
        if (idx >= 0) {
            switch (reg) {
            case US_IMR:  return s->us_imr[idx];
            case US_PTSR: return s->us_ptsr[idx];
            case US_TCR:  return s->us_tcr[idx];
            case US_TPR:  return s->us_tpr[idx];
            case US_RCR:  return s->us_rcr[idx];
            case US_RPR:  return s->us_rpr[idx];  /* driver derives rx count */
            default: break;
            }
        }
        return 0;
    }

    if (addr >= AT91_DBGU && addr < AT91_DBGU + 0x200) {
        switch (addr - AT91_DBGU) {
        case DBGU_SR:   return DBGU_TXRDY | DBGU_TXEMPTY;
        case DBGU_CIDR: return CIDR_AT91SAM9260;
        default:        return 0;
        }
    }
    if (addr >= AT91_PMC && addr < AT91_PMC + 0x100) {
        switch (addr - AT91_PMC) {
        case PMC_SR:   return pmc_sr(s);
        case PMC_MCFR: return PMC_MCFR_MAINRDY | ((MAIN_HZ * 16) / 32768);
        case PMC_PLLAR: return s->pmc_pllar;
        case PMC_PLLBR: return s->pmc_pllbr;
        case PMC_MCKR:  return s->pmc_mckr;
        default:       return 0;
        }
    }
    if (addr >= AT91_AIC && addr < AT91_AIC + 0x200) {
        uint32_t o = addr - AT91_AIC;
        if (o == AIC_IVR) {
            /* Reading IVR "enters" the interrupt: deassert nIRQ. If a source is
             * pending, latch it (ISR returns its number) and return its vector;
             * otherwise it's spurious -> ISR reads 0 and IVR returns SPU. */
            uint32_t pend = s->aic_ipr & s->aic_imr;
            if (pend) {
                uint32_t src = ctz32(pend);
                s->aic_cur_src = src;
                s->aic_in_service = true;
                aic_update(s);
                return s->aic_svr[src];
            }
            s->aic_cur_src = 0;
            return s->aic_spu;
        }
        if (o == AIC_IPR)  return s->aic_ipr;
        if (o == AIC_IMR)  return s->aic_imr;
        if (o == AIC_ISR)  return s->aic_cur_src;   /* current source number */
        if (o >= AIC_SVR0 && o < AIC_SVR0 + 0x80) return s->aic_svr[(o - AIC_SVR0) / 4];
        return 0;
    }
    if (addr >= AT91_PIT && addr < AT91_PIT + 0x10) {
        switch (addr - AT91_PIT) {
        case PIT_MR:  return s->pit_mr;
        case PIT_SR:  return s->pit_pits;
        case PIT_PIVR: {
            /* The ISR does `do { ... } while (--nr_ticks)` on PICNT — PICNT MUST
             * be >= 1 or it underflows to ~4e9 (the storm). PITS is set here, so
             * at least one period elapsed: clamp. */
            uint32_t picnt = s->pit_picnt ? s->pit_picnt : 1;
            uint32_t v = (picnt << 20) | pit_cpiv(s);
            s->pit_pits = 0;      /* PIVR read acknowledges -> line goes low */
            s->pit_picnt = 0;
            /* System interrupt is level-sensitive: clearing PITS lowers the AIC
             * source line, else EOICR re-fires forever. */
            s->aic_ipr &= ~(1u << IRQ_SYS);
            aic_update(s);
            return v;
        }
        case PIT_PIIR:
            return (s->pit_picnt << 20) | pit_cpiv(s);
        default: return 0;
        }
    }
    return 0;
}

static void at91_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    AT91State *s = opaque;
    hwaddr addr = AT91_SYSC_BASE + off;

    /* DBGU THR: polled console write (kernel printk on DBGU, if any). */
    if (addr == AT91_DBGU + DBGU_THR) {
        putchar((int)(val & 0xFF));
        fflush(stdout);
        return;
    }
    if (addr < AT91_SYSC_TOP) {
        uint32_t reg = addr & 0x3fff;
        int idx = usart_index(addr & ~0x3fffULL);
        if (reg == US_THR) {          /* polled single-char TX (kernel console) */
            uint8_t ch = val & 0xFF;
            usart_emit(s, idx >= 0 ? idx : 1, &ch, 1);
            return;
        }
        if (idx >= 0) {
            switch (reg) {
            case US_CR:
                if (val & US_CR_STTTO)  s->us_csr_int[idx] &= ~US_TIMEOUT;
                if (val & US_CR_RSTSTA) s->us_csr_int[idx] &= ~(US_ENDRX | US_TIMEOUT);
                usart_update(s, idx);
                break;
            case US_IER: s->us_imr[idx] |= val; usart_update(s, idx); break;
            case US_IDR: s->us_imr[idx] &= ~val; usart_update(s, idx); break;
            case US_TPR: s->us_tpr[idx] = val; break;
            case US_TCR: s->us_tcr[idx] = val;
                if (val) s->us_csr_int[idx] &= ~(US_ENDTX | US_TXBUFE); /* new xfer */
                break;
            case US_RPR:  s->us_rpr[idx] = val; break;
            case US_RCR:  s->us_rcr[idx] = val;
                if (val) s->us_csr_int[idx] &= ~US_ENDRX;
                /* RX buffer re-armed: pull any pending console input now instead
                 * of waiting for the chardev's own poll (fast paste). */
                if (val && idx == 0) qemu_chr_fe_accept_input(&s->cons);
                break;
            case US_RNPR: s->us_rnpr[idx] = val; break;
            case US_RNCR: s->us_rncr[idx] = val; break;
            case US_PTCR:
                if (val & PDC_TXTEN)  { s->us_ptsr[idx] |= PDC_TXTEN;  usart_pdc_tx(s, idx); }
                if (val & PDC_TXTDIS) { s->us_ptsr[idx] &= ~PDC_TXTEN; }
                if (val & PDC_RXTEN)  { s->us_ptsr[idx] |= PDC_RXTEN;
                    if (idx == 0) qemu_chr_fe_accept_input(&s->cons); }
                if (val & PDC_RXTDIS) { s->us_ptsr[idx] &= ~PDC_RXTEN; }
                break;
            default: break;
            }
        }
        return;  /* other USART/TC writes: accept and drop */
    }
    if (addr >= AT91_AIC && addr < AT91_AIC + 0x200) {
        uint32_t o = addr - AT91_AIC;
        if (o >= AIC_SVR0 && o < AIC_SVR0 + 0x80) { s->aic_svr[(o - AIC_SVR0) / 4] = val; return; }
        switch (o) {
        case AIC_IECR: s->aic_imr |= val; aic_update(s); return;
        case AIC_IDCR: s->aic_imr &= ~val; aic_update(s); return;
        case AIC_ICCR: s->aic_ipr &= ~val; aic_update(s); return;
        case AIC_ISCR: s->aic_ipr |= val; aic_update(s); return;
        case AIC_SPU: s->aic_spu = val; return;
        case AIC_EOICR: s->aic_in_service = false; aic_update(s); return;
        default: return;
        }
    }
    if (addr >= AT91_PMC && addr < AT91_PMC + 0x100) {
        switch (addr - AT91_PMC) {
        case PMC_PLLAR: s->pmc_pllar = val; return;   /* SR.LOCKA tracks MUL */
        case PMC_PLLBR: s->pmc_pllbr = val; return;   /* SR.LOCKB tracks MUL */
        case PMC_MCKR:  s->pmc_mckr = val; return;
        default: return;
        }
    }
    if (addr >= AT91_PIT && addr < AT91_PIT + 0x10) {
        if ((addr - AT91_PIT) == PIT_MR) {
            uint32_t was_en = s->pit_mr & PIT_MR_PITEN;
            s->pit_mr = val;
            if ((val & PIT_MR_PITEN) && !was_en) {
                s->pit_last_tick_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                pit_rearm(s);
            }
        }
        return;
    }
    /* Other writes accepted and dropped. */
}

/* ---- IGLOO Portal host: drive igloo.ko's OSI over penguin-qemu's cp7 ------
 * hypercall interception. penguin-qemu (target/arm patch) routes the guest's
 * `mcr p7,0,r0,c0,c0,0` to penguin_handle_guest_hypercall -> this callback.
 * We implement the minimal host side of the Portal: when igloo issues its
 * portal hypercall (IGLOO_HYPER_PORTAL_INTERRUPT) with the shared region, we
 * ask it to run HYPER_OP_OSI_PROC_ALL, then read the returned osi_proc_node
 * array straight out of guest RAM and print the process list — OSI produced by
 * the real igloo_driver, using the recovered+validated vendor offsets. */
#define IGLOO_HYPER_PORTAL_INTERRUPT 0x7902ULL
#define HYPER_OP_OSI_PROC_ALL 9u
#define HYPER_RESP_READ_OK    0xf0000001u
/* region_header: u32 op@0, u32 pid@4, u64 addr@8, u64 size@16 (24 bytes).
 * data at region+24: osi_result_header{u64 result_count@24,u64 total_count@32};
 * node[] at region+40; osi_proc_node = pid@+0,ppid@+8,...,comm[16]@+40 (56 B). */
#define RH_OP     0
#define RH_ADDR   8
#define PORTAL_DATA_OFF   24
#define OSI_NODE_OFF      40
#define OSI_NODE_SZ       56
#define OSI_NODE_COMM     40

static uint32_t kl_v2p(uint32_t va) { return va - 0xC0000000u + AT91_SDRAM_BASE; }
static uint32_t kl_rd32(uint32_t pa)
{
    uint32_t v = 0;
    address_space_read(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, &v, 4);
    return v;
}
static uint64_t kl_rd64(uint32_t pa)
{
    uint64_t v = 0;
    address_space_read(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, &v, 8);
    return v;
}
static void kl_wr32(uint32_t pa, uint32_t v)
{
    address_space_write(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, &v, 4);
}

static int kl_portal_osi_host(CPUState *cs, uint64_t nr, uint64_t a0, uint64_t a1,
                              uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                              uint64_t *ret, void *opaque)
{
    static int done = 0;
    *ret = 0;
    if (nr != IGLOO_HYPER_PORTAL_INTERRUPT || a2 == 0 || done) {
        return 0;
    }
    uint32_t region = kl_v2p((uint32_t)a2);   /* region kernel-virt -> phys */
    uint32_t op = (uint32_t)a3;               /* current region->header.op */

    if (op != HYPER_RESP_READ_OK) {
        /* fresh region: ask igloo to enumerate all processes (skip=0) */
        kl_wr32(region + RH_ADDR, 0);
        kl_wr32(region + RH_ADDR + 4, 0);
        kl_wr32(region + RH_OP, HYPER_OP_OSI_PROC_ALL);
        return 0;
    }
    /* result ready: parse and print the osi_proc_node array */
    uint64_t n = kl_rd64(region + PORTAL_DATA_OFF);          /* result_count */
    uint64_t total = kl_rd64(region + PORTAL_DATA_OFF + 8);  /* total_count  */
    fprintf(stderr, "PENGUIN_OSI: igloo HYPER_OP_OSI_PROC_ALL -> %llu procs "
            "(total %llu):\n", (unsigned long long)n, (unsigned long long)total);
    for (uint64_t i = 0; i < n && i < 128; i++) {
        uint32_t node = region + OSI_NODE_OFF + (uint32_t)(i * OSI_NODE_SZ);
        uint64_t pid  = kl_rd64(node + 0);
        uint64_t ppid = kl_rd64(node + 8);
        char comm[17];
        address_space_read(&address_space_memory, node + OSI_NODE_COMM,
                           MEMTXATTRS_UNSPECIFIED, comm, 16);
        comm[16] = '\0';
        fprintf(stderr, "PENGUIN_OSI:   pid=%-5llu ppid=%-5llu comm=%s\n",
                (unsigned long long)pid, (unsigned long long)ppid, comm);
    }
    fflush(stderr);
    done = 1;
    kl_wr32(region + RH_OP, 0);   /* HYPER_OP_NONE: end the guest portal loop */
    return 0;
}

static const MemoryRegionOps at91_ops = {
    .read = at91_read,
    .write = at91_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static struct arm_boot_info at91sam9260_binfo;

static void at91sam9260_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    AT91State *s = g_new0(AT91State, 1);
    Object *cpuobj;
    ARMCPU *cpu;

    cpuobj = object_new(machine->cpu_type);
    qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
    cpu = ARM_CPU(cpuobj);
    s->cpu_irq = qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ);

    s->pmc_pllar = PMC_PLLAR_INIT;   /* bootloader-programmed clock tree */
    s->pmc_mckr = PMC_MCKR_INIT;

    /* Host side of the IGLOO Portal. Under penguin, penguin's Python registers
     * the cp7 guest-hypercall callback and owns the Portal (OSI runs from the
     * osi pyplugin), and there is a single callback slot -- so the board must NOT
     * also register kl_portal_osi_host or the two collide. Gate the board's
     * standalone OSI host behind KL_STANDALONE_OSI: bare qemu-system-arm sets it
     * to reproduce the C-driven OSI demo; the default (under penguin) leaves the
     * Portal to the pyplugin. */
    if (getenv("KL_STANDALONE_OSI")) {
        set_penguin_guest_hypercall_callback(kl_portal_osi_host, s);
        penguin_register_guest_hypercall(IGLOO_HYPER_PORTAL_INTERRUPT);
    }

    /* USART0 (ttyS0 console) on a CharBackend for interactive TX+RX. */
    if (serial_hd(0)) {
        qemu_chr_fe_init(&s->cons, serial_hd(0), &error_abort);
        qemu_chr_fe_set_handlers(&s->cons, cons_can_rx, cons_rx, NULL, NULL,
                                 s, NULL, true);
    }

    memory_region_add_subregion(sysmem, AT91_SDRAM_BASE, machine->ram);

    s->pit_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pit_tick, s);
    memory_region_init_io(&s->sysc, OBJECT(machine), &at91_ops, s,
                          "at91.sysc", AT91_SYSC_SIZE);
    memory_region_add_subregion(sysmem, AT91_SYSC_BASE, &s->sysc);

    /* NAND flash on EBI CS3: backing store 0xFF-filled, with the JFFS2 root
     * image loaded at the "root" partition offset. Image path from the
     * AT91_NAND_IMAGE env var keeps the board generic. */
    s->nand_store = g_malloc(NAND_SIZE);
    memset(s->nand_store, 0xFF, NAND_SIZE);
    const char *nand_img = getenv("AT91_NAND_IMAGE");
    if (nand_img && nand_img[0]) {
        FILE *fp = fopen(nand_img, "rb");
        if (fp) {
            size_t n = fread(s->nand_store + NAND_ROOT_OFF, 1,
                             NAND_SIZE - NAND_ROOT_OFF, fp);
            fclose(fp);
            fprintf(stderr, "at91 NAND: loaded %zu bytes of %s at 0x%x\n",
                    n, nand_img, NAND_ROOT_OFF);
        } else {
            fprintf(stderr, "at91 NAND: cannot open %s\n", nand_img);
        }
    }
    memory_region_init_io(&s->nand, OBJECT(machine), &nand_ops, s,
                          "at91.nand", AT91_NAND_WIN);
    memory_region_add_subregion(sysmem, AT91_NAND_BASE, &s->nand);

    /* EMAC: reuse QEMU's Cadence GEM. Its core register bank (NWCTRL/NWCFG/
     * NWSTATUS/TSR/RBQP/TBQP/RSR/ISR..IMR/PHYMNTNC at 0x00-0x34) matches the
     * classic AT91 macb the vendor driver programs, and NWSTATUS reads IDLE so
     * the MDIO poll completes. Map it over the sysc window at 0xFFFC4000 at a
     * higher priority; route its IRQ to AIC source 21. NIC backend + PHY come
     * from -nic/-netdev on the command line. */
    {
        DeviceState *gem = qdev_new(TYPE_CADENCE_GEM);
        SysBusDevice *gsbd = SYS_BUS_DEVICE(gem);
        qemu_configure_nic_device(gem, true, NULL);
        object_property_set_int(OBJECT(gem), "phy-addr", 0, &error_abort);
        sysbus_realize_and_unref(gsbd, &error_fatal);
        sysbus_mmio_map_overlap(gsbd, 0, AT91_MACB, 2);
        sysbus_connect_irq(gsbd, 0,
                           qemu_allocate_irq(at91_emac_set_irq, s, 0));
    }

    at91sam9260_binfo.ram_size = machine->ram_size;
    at91sam9260_binfo.board_id = MACH_TYPE_AT91SAM9260;
    at91sam9260_binfo.loader_start = AT91_SDRAM_BASE;
    arm_load_kernel(cpu, machine, &at91sam9260_binfo);
}

static void at91sam9260_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Atmel AT91SAM9260 (ARM926EJ-S), kernel-lift Tier-A";
    mc->init = at91sam9260_init;
    mc->ignore_memory_transaction_failures = true;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm926");
    mc->default_ram_id = "at91.sdram";
    mc->default_ram_size = 64 * MiB;
}

static const TypeInfo at91sam9260_type = {
    .name = MACHINE_TYPE_NAME("at91sam9260"),
    .parent = TYPE_MACHINE,
    .class_init = at91sam9260_class_init,
    .interfaces = arm_machine_interfaces,
};

static void at91sam9260_machine_init(void)
{
    type_register_static(&at91sam9260_type);
}

type_init(at91sam9260_machine_init)
