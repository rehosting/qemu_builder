/*
 * Broadcom BCM47094 ("BCM4709C0", Northstar) SoC on a dual-switch router board
 * (BCM53012 internal switch plus an RTL8365MB on GPIO-bitbanged SMI).
 *
 * Goal: run the UNMODIFIED vendor kernel (2.6.36.4brcmarm, MACH_BRCM_NS 9990)
 * on a model of the hardware it was
 * built for. Every address and value below is either read from the GPL
 * kernel's own platform code (arch/arm/plat-brcm, mach-brcm-hnd, shared/) or
 * marked [assume] when the source does not pin it down; the [assume] list is
 * the board's caveat list.
 *
 * Boot handoff mirrors CFE: the raw kernel Image is loaded at PA 0x8000 and
 * CPU0 enters it in Secure SVC, MMU and caches off ("Starting program at
 * 0x00008000"). The kernel itself then:
 *   - reads ChipCommon chipid; for 53030 it jumps to the ACP alias of DRAM at
 *     0x40000000 and runs there (head.S bx_acp; PHYS_OFFSET = 0x40000000);
 *   - discards r1/r2 (plat-head.S forces MACH_BRCM_NS, no ATAGs);
 *   - sizes DRAM by probing for aliasing in the 0x80000000 window;
 *   - releases CPU1 through the mask-ROM LUT at 0xffff0400.
 * Those behaviours are modelled rather than worked around.
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/core/loader.h"
#include "hw/cpu/a9mpcore.h"
#include "hw/char/serial-mm.h"
#include "hw/misc/unimp.h"
#include "hw/misc/bcm_ns.h"
#include "hw/net/bcm_ns_eth.h"
#include "hw/net/rtl8365mb.h"
#include "net/net.h"
#include "system/system.h"
#include "system/blockdev.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "exec/cpu-common.h"
#include "qemu/bswap.h"
#include "bcm4709_lzma.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/cpu.h"

/* --- Memory map (plat-bcm5301x.h, mach/memory.h, bcm-ns.dtsi) ------------ */
#define NS_DRAM_LO_BASE      0x00000000  /* first 128 MiB aliased at 0 */
#define NS_DRAM_LO_SIZE      (128 * MiB)
#define NS_DRAM_ACP_BASE     0x40000000  /* 4709C0: 1 GiB ACP (coherent) region */
#define NS_DRAM_HI_BASE      0x80000000  /* 1 GiB non-ACP region */
#define NS_DRAM_WINDOW       (1 * GiB)
#define NS_SOC_BASE          0x18000000  /* SI_ENUM_BASE */
#define NS_SOC_SIZE          0x01000000  /* 16 MiB static IO map (bcm5301x.c) */
#define NS_CCA_BASE          0x18000000  /* ChipCommon-A, core 0 */
#define NS_UART0_BASE        0x18000300  /* -> ttyS0 (console) */
#define NS_UART1_BASE        0x18000400  /* -> ttyS1 */
#define NS_DMU_BASE          0x1800c000  /* DMU / CRU (LCPLL, GENPLL, straps) */
#define NS_EROM_BASE         0x18130000  /* SOC_SDOM_BASE_PA "System discover ROM" */
#define NS_IHOST_BASE        0x19000000  /* iHost CRU (ARM PLL, clock policy) */
#define NS_IHOST_SIZE        0x00020000
#define NS_MPCORE_BASE       0x19020000  /* SCU/GIC/timers; CP15 CBAR */
#define NS_L2CC_BASE         0x19022000  /* PL310 */
#define NS_ROM_BASE          0xffff0000  /* SOC_ROM_BASE_PA */
#define NS_ROM_SIZE          (64 * KiB)
#define NS_ROM_LUT_OFF       0x400       /* SOC_ROM_LUT_OFF: secondary entry */

#define NS_NUM_IRQ           256
#define NS_SPI_CCA           85          /* IRQ_CCA 117: both CCA UARTs + GPIO */

#define MACH_TYPE_BRCM_NS    9990
#define NS_KERNEL_LOAD       0x00008000  /* CFE "Entry at 0x00008000" */

/* Clocks. 25 MHz crystal (board_ns.c clk_ref). CFE si_arm_setclock(1400 MHz)
 * leaves PLLARMA = 0x1007001 | POST_RESETB | LOCK and POLICY_FREQ = 0x87070707
 * (fid 7 -> ARMPLL ch1 = 25 MHz * 112 / pdiv 1 / mdiv 2 = 1400 MHz);
 * periph_clk = arm_clk / 2 = 700 MHz (iproc_cru.c), matching mainline's
 * "sched_clock ... 700MHz" on this board. */
#define NS_ARM_HZ            1400000000u
#define NS_PERIPH_HZ         (NS_ARM_HZ / 2)
#define NS_ALP_HZ            125000000u  /* si_ns_alp_clock() from GENPLL below */

/* ChipCommon-A identity: chip 53030 (BCM47094), rev 0, pkg 0 (4709), AI bus. */
#define NS_CC_CHIPID         ((1u << 28) | (0u << 20) | (0u << 16) | 53030u)
/* [assume] capabilities: 2 UARTs, UART clock = ALP (sel 0), no flash type
 * (mainline: "Flash type not supported"), no PMU, no SROM/OTP. */
#define NS_CC_CAPS           0x00000002u

/*
 * Boot ROM. The real mask ROM is not available; this models only what the
 * kernel relies on: a secondary core waits in a holding pen at 0xffff002c and
 * branches to whatever the LUT at 0xffff0400 holds once it is no longer the
 * pen itself. plat-head.S writes 0xffff002c into the LUT; platsmp.c later
 * writes virt_to_phys(platform_secondary_startup) there and issues SEV.
 */
static const uint32_t ns_bootrom_reset[] = {
    /* 0xffff0000: reset vector (CPU1 starts here) */
    0xe59ff000, /* ldr  pc, [pc, #0]    ; -> 0xffff002c */
    0xe320f000, /* nop */
    0xffff002c, /* literal */
};
static const uint32_t ns_bootrom_pen[] = {
    /* 0xffff002c: holding pen */
    0xe320f002, /* wfe */
    0xe59f0004, /* ldr  r0, [pc, #4]    ; =0xffff0400 */
    0xe5900000, /* ldr  r0, [r0] */
    0xe12fff10, /* bx   r0              ; LUT == 0xffff002c loops back here */
    0xffff0400, /* literal */
};

/* ---- EROM ---------------------------------------------------------------
 * Core list, IDs and revisions are the ones bcma enumerates on real hardware
 * (an OpenWrt boot log). Register bases come from
 * the vendor platform code / mainline DTS. Wrapper addresses marked [assume]
 * are not in any source; the kernel only needs them self-consistent.
 */
typedef struct NSCore {
    const char *name;
    uint16_t cid;
    uint8_t rev;
    uint32_t base;
    uint32_t base2;     /* second 4 KiB slave range on port 0, or 0 */
    uint32_t wrap;      /* slave wrapper (DMP) page, or 0 for none */
} NSCore;

static const NSCore ns_cores[] = {
    { "chipcommon-a", 0x800, 0x2a, 0x18000000, 0,          0x18100000 },
    { "chipcommon-b", 0x50b, 0x05, 0x18001000, 0,          0 },          /* no wrapper */
    { "dma",          0x502, 0x04, 0x18020000, 0,          0x18114000 }, /* [assume] */
    { "gmac0",        0x82d, 0x07, 0x18024000, 0,          0x18110000 }, /* wrap [assume] */
    { "gmac1",        0x82d, 0x07, 0x18025000, 0,          0x18111000 },
    { "gmac2",        0x82d, 0x07, 0x18026000, 0,          0x18112000 },
    { "gmac3",        0x82d, 0x07, 0x18027000, 0,          0x18113000 },
    { "pcie0",        0x501, 0x07, 0x18012000, 0,          0x18101000 }, /* wrap [assume] */
    { "pcie1",        0x501, 0x07, 0x18013000, 0,          0x18102000 },
    { "pcie2",        0x501, 0x07, 0x18014000, 0,          0x18103000 },
    { "ihost",        0x510, 0x07, 0x19000000, 0,          0x18106000 }, /* [assume] */
    { "usb2",         0x504, 0x07, 0x18021000, 0x18022000, 0x18115000 },
    { "usb3",         0x505, 0x07, 0x18023000, 0,          0x18105000 },
    { "sdio3",        0x503, 0x01, 0x1802b000, 0,          0x18116000 }, /* [assume] */
    { "i2s",          0x834, 0x03, 0x1802a000, 0,          0x18117000 },
    { "a9-jtag",      0x506, 0x01, 0x18210000, 0,          0x18118000 }, /* [assume] */
    { "ddr",          0x507, 0x01, 0x18010000, 0,          0x18108000 },
    { "rom",          0x508, 0x08, 0xfffd0000, 0,          0x18109000 }, /* wrap [assume] */
    { "nand",         0x509, 0x01, 0x18028000, 0,          0x1811a000 },
    { "qspi",         0x50a, 0x01, 0x18029000, 0,          0x1811b000 }, /* wrap [assume] */
};
#define NS_ROM_WRAP   0x18109000
#define NS_NAND_WRAP  0x1811a000
/* UART clock 125 MHz: OpenWrt on real hardware reports base_baud = 7812500 */
#define NS_UART_BAUDBASE 7812500
#define NS_NAND_REGS  0x18028000
#define NS_NAND_WINDOW 0x1c000000
#define NS_GMAC_BASE  0x18024000  /* GMAC0-3, 4 KiB apart */
#define NS_GMAC_WRAP  0x18110000  /* their IDM wrappers */
#define NS_SPI_GMAC   147         /* IRQ 179-182: si_bus_irq_map in bcm5301x_pcie.c */
#define NS_SRAB_BASE  0x18007000  /* BCM53012 switch register access bridge */

static size_t ns_build_erom(uint32_t *e)
{
    size_t n = 0;

    for (size_t i = 0; i < ARRAY_SIZE(ns_cores); i++) {
        const NSCore *c = &ns_cores[i];
        uint32_t nsw = c->wrap ? 1 : 0;

        e[n++] = (0x4bfu << 20) | ((uint32_t)c->cid << 8) | 1;          /* CIA */
        e[n++] = ((uint32_t)c->rev << 24) | (nsw << 19) | (0 << 14) |  /* CIB */
                 (1 << 9) | (0 << 4) | 1;                              /* nsp=1 */
        e[n++] = c->base | (0 << 8) | 0x00 | 0x00 | 0x4 | 1;           /* SP0 slave 4K */
        if (c->base2) {
            e[n++] = c->base2 | (0 << 8) | 0x00 | 0x00 | 0x4 | 1;
        }
        if (c->wrap) {
            e[n++] = c->wrap | (0 << 8) | 0x80 | 0x00 | 0x4 | 1;       /* SWRAP 4K */
        }
    }
    e[n++] = 0x0000000f;                                               /* END */
    return n;
}

/* ---- DMU / CRU at 0x1800c000 ([assume] values where noted) -------------- */
static const BCMNSRegInit ns_dmu_init[] = {
    /* GENPLL: ndiv 80, pdiv 1, ch5 mdiv 16 -> ALP / iproc_slow 125 MHz.
     * [assume] exact words; the kernel only derives clock rates from them. */
    { 0x144, 0x00000000, 0 },            /* ctrl1: all channels enabled */
    { 0x154, 0x05000000, 0 },            /* ctrl5: ndiv_int 80 */
    { 0x158, 0x01080d42, 0 },            /* ctrl6: pdiv 1, mdiv ch0/1/2 */
    { 0x15c, 0x00040810, 0 },            /* ctrl7: mdiv ch3/4/5 */
    { 0x160, 0x00001000, 0x00001000 },   /* status: locked */
    { 0x2a0, 0x00000010, 0 },            /* straps: bit4 = USB3 (not 3rd PCIe) */
    /* PVTMON status, read-only. The kernel reports 418 - 0.5556 * value degC
     * (/proc/dmu/temperature, shown in the web UI); 0 would read as 418 degC.
     * 0x284 is about 60 degC, a plausible idle SoC. [assume] the value. */
    { 0x2c8, 0x00000284, 0xffffffff },
    { UINT32_MAX, 0, 0 },
};

/* ---- iHost CRU at 0x19000000, as CFE si_arm_setclock(1400) leaves it ---- */
static const BCMNSRegInit ns_ihost_init[] = {
    { 0x008, 0x87070707, 0 },            /* POLICY_FREQ: fid 7 for all policies */
    { 0x200, 0x00010303, 0 },            /* CORE0_CLKGATE */
    { 0x204, 0x00000303, 0 },            /* CORE1_CLKGATE */
    { 0x210, 0x00010303, 0 },            /* ARM_SWITCH_CLKGATE */
    { 0x300, 0x00010303, 0 },            /* ARM_PERIPH_CLKGATE */
    { 0x400, 0x00010303, 0 },            /* APB0_CLKGATE */
    { 0xc00, 0x11007003, 0x10000000 },   /* PLLARMA: lock|pdiv1|ndiv112|post_resetb */
    { 0xc20, 0x00000002, 0 },            /* PLLARMCTRL5: mdiv 2 [assume: hw default] */
    { 0xec0, 0x00007000, 0 },            /* POLICY_DBG: active fid 7 */
    { UINT32_MAX, 0, 0 },
};

typedef struct {
    ARMCPU *cpu[2];
    MemoryRegion dram_lo, dram_acp[2], dram_hi_wrap, rom, erom;
    DeviceState *nand;
} NSBoard;

/*
 * CFE-style boot from flash, used when no -kernel is given: on every reset,
 * read the TRX at the start of the "linux" partition from the NAND's current
 * contents, check its header CRC as CFE does, LZMA-decompress partition 0 (the
 * kernel) and place it at 0x8000. A firmware upgrade written by the guest is
 * therefore what the next reset boots. CFE's console, its second-image
 * fallback and its recovery web server are not modelled: a bad image stops
 * the machine instead.
 */
#define NS_NAND_LINUX_OFF    0x00200000  /* MTD "linux" (board_ns.c, boot log) */
#define NS_NAND_LINUX_SIZE   0x03e00000
#define NS_TRX_MAGIC         0x30524448  /* "HDR0" */
#define NS_TRX_HDR_LEN       28

static uint32_t ns_trx_crc(const uint8_t *p, size_t n)
{
    static uint32_t tab[256];
    uint32_t c = 0xffffffff;            /* hndcrc32: init ~0, no final xor */

    if (!tab[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;

            for (int k = 0; k < 8; k++) {
                v = (v >> 1) ^ (0xedb88320 & -(v & 1));
            }
            tab[i] = v;
        }
    }
    while (n--) {
        c = tab[(c ^ *p++) & 0xff] ^ (c >> 8);
    }
    return c;
}

static bool ns_flash_boot_load(NSBoard *s, const char **err)
{
    uint8_t hdr[NS_TRX_HDR_LEN];
    g_autofree uint8_t *trx = NULL;
    g_autofree uint8_t *kernel = NULL;
    uint32_t len, crc, off0, off1;
    size_t klen;

    if (!bcm_ns_nand_read(s->nand, NS_NAND_LINUX_OFF, hdr, sizeof(hdr))) {
        *err = "flash too small for the linux partition";
        return false;
    }
    len = ldl_le_p(hdr + 4);
    crc = ldl_le_p(hdr + 8);
    off0 = ldl_le_p(hdr + 16);
    off1 = ldl_le_p(hdr + 20);
    if (ldl_le_p(hdr) != NS_TRX_MAGIC) {
        *err = "no TRX header (HDR0) at the start of the linux partition";
        return false;
    }
    if (len < NS_TRX_HDR_LEN || len > NS_NAND_LINUX_SIZE) {
        *err = "TRX length out of range";
        return false;
    }
    trx = g_malloc(len);
    bcm_ns_nand_read(s->nand, NS_NAND_LINUX_OFF, trx, len);
    if (ns_trx_crc(trx + 12, len - 12) != crc) {
        *err = "TRX CRC mismatch";
        return false;
    }
    if (!off1) {
        off1 = len;
    }
    if (off0 < NS_TRX_HDR_LEN || off1 <= off0 || off1 > len) {
        *err = "TRX partition 0 out of range";
        return false;
    }
    kernel = ns_lzma_alone_decode(trx + off0, off1 - off0,
                                  NS_DRAM_LO_SIZE - NS_KERNEL_LOAD, &klen, err);
    if (!kernel) {
        return false;
    }
    address_space_write(&address_space_memory, NS_KERNEL_LOAD,
                        MEMTXATTRS_UNSPECIFIED, kernel, klen);
    info_report("bcm47094-router: booting the TRX kernel from flash "
                "(%u bytes LZMA -> %zu bytes at 0x%x)",
                off1 - off0, klen, NS_KERNEL_LOAD);
    return true;
}

static void ns_flash_boot_reset(void *opaque)
{
    const char *err = NULL;

    if (!ns_flash_boot_load(opaque, &err)) {
        error_report("bcm47094-router: flash boot failed: %s", err);
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_PANIC);
    }
}

/*
 * A CCA UART. tx-pacing: the stock kernel probes these as plain 16550s (no
 * FIFO, on the real unit too), so its ISR moves one byte per THRE. Without
 * line-rate THRE, any console burst over 256 bytes trips 8250's PASS_LIMIT
 * and logs "too much work for irq117"; the real unit never does.
 */
static void ns_uart(MemoryRegion *sysmem, hwaddr base, qemu_irq irq,
                    Chardev *chr)
{
    DeviceState *d = qdev_new(TYPE_SERIAL_MM);

    qdev_prop_set_uint8(d, "regshift", 0);
    qdev_prop_set_uint32(d, "baudbase", NS_UART_BAUDBASE);
    qdev_prop_set_chr(d, "chardev", chr);
    qdev_prop_set_bit(d, "tx-pacing", true);
    qdev_set_legacy_instance_id(d, base, 2);
    qdev_prop_set_uint8(d, "endianness", DEVICE_LITTLE_ENDIAN);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(d), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(d), 0, irq);
    memory_region_add_subregion(sysmem, base,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(d), 0));
}

static void ns_cpu_reset(void *opaque)
{
    ARMCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    if (cs->cpu_index == 0) {
        /* CFE handoff: r0 = 0, r1 = machine type, r2 = 0 (no ATAGs). */
        cpu->env.regs[0] = 0;
        cpu->env.regs[1] = MACH_TYPE_BRCM_NS;
        cpu->env.regs[2] = 0;
        cpu_set_pc(cs, NS_KERNEL_LOAD);
    } else {
        cpu_set_pc(cs, NS_ROM_BASE);
    }
}

static void ns_map_dram(NSBoard *s, MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    uint64_t sz = machine->ram_size;

    /*
     * The DDR controller decodes each 1 GiB window modulo the fitted size, so
     * 512 MiB appears twice in it. plat-head.S szmem finds the size by writing
     * at 0x80000000+N and looking for the write at 0x80000000.
     */
    memory_region_add_subregion(sysmem, NS_DRAM_HI_BASE, machine->ram);
    memory_region_init_alias(&s->dram_hi_wrap, NULL, "ns.dram-hi-wrap",
                             machine->ram, 0, sz);
    memory_region_add_subregion(sysmem, NS_DRAM_HI_BASE + sz, &s->dram_hi_wrap);

    for (int i = 0; i < 2; i++) {
        memory_region_init_alias(&s->dram_acp[i], NULL, "ns.dram-acp",
                                 machine->ram, 0, sz);
        memory_region_add_subregion(sysmem, NS_DRAM_ACP_BASE + i * sz, &s->dram_acp[i]);
    }

    memory_region_init_alias(&s->dram_lo, NULL, "ns.dram-lo", machine->ram, 0,
                             MIN(sz, NS_DRAM_LO_SIZE));
    memory_region_add_subregion(sysmem, NS_DRAM_LO_BASE, &s->dram_lo);
}

static void ns_rtl_to_robo(void *opaque, const uint8_t *buf, size_t len)
{
    bcm53012_ingress(BCM53012(opaque), 5, buf, len);
}

static void ns_robo_to_rtl(void *opaque, const uint8_t *buf, size_t len)
{
    rtl8365mb_from_uplink(RTL8365MB(opaque), buf, len);
}

static void ns_init(MachineState *machine)
{
    NSBoard *s = g_new0(NSBoard, 1);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *mpcore, *cc, *idm;
    SysBusDevice *busdev;
    qemu_irq cpu_irq[2];
    uint32_t erom[256];
    size_t erom_words;
    int n;

    if (machine->ram_size != 512 * MiB) {
        error_report("bcm47094-router: the board has 512 MiB of DDR3 (-m 512)");
        exit(1);
    }

    for (n = 0; n < machine->smp.cpus; n++) {
        Object *cpuobj = object_new(machine->cpu_type);
        object_property_set_int(cpuobj, "reset-cbar", NS_MPCORE_BASE, &error_fatal);
        /* BCM4709C0 integrates Cortex-A9 r3p0 (mainline log: CPU [413fc090]). */
        object_property_set_uint(cpuobj, "midr", 0x413fc090, &error_fatal);
        qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
        s->cpu[n] = ARM_CPU(cpuobj);
        cpu_irq[n] = qdev_get_gpio_in(DEVICE(cpuobj), ARM_CPU_IRQ);
        qemu_register_reset(ns_cpu_reset, s->cpu[n]);
    }

    ns_map_dram(s, machine);

    /* Boot ROM + LUT (the LUT is written by the kernel, so the region is RAM). */
    memory_region_init_ram(&s->rom, NULL, "ns.bootrom", NS_ROM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, NS_ROM_BASE, &s->rom);
    rom_add_blob_fixed("ns.bootrom.reset", ns_bootrom_reset,
                       sizeof(ns_bootrom_reset), NS_ROM_BASE);
    rom_add_blob_fixed("ns.bootrom.pen", ns_bootrom_pen,
                       sizeof(ns_bootrom_pen), NS_ROM_BASE + 0x2c);
    {
        static const uint32_t lut0 = 0xffff002c;
        rom_add_blob_fixed("ns.bootrom.lut", &lut0, sizeof(lut0),
                           NS_ROM_BASE + NS_ROM_LUT_OFF);
    }

    /* Everything on the SoC bus not modelled below: logged (-d unimp), RAZ/WI. */
    create_unimplemented_device("ns.soc", NS_SOC_BASE, NS_SOC_SIZE);
    create_unimplemented_device("ns.ihost-misc", NS_IHOST_BASE, NS_IHOST_SIZE);

    /* Cortex-A9 MPCore private region: SCU, GIC, global timer, TWD, WDT. */
    mpcore = qdev_new(TYPE_A9MPCORE_PRIV);
    qdev_prop_set_uint32(mpcore, "num-cpu", machine->smp.cpus);
    qdev_prop_set_uint32(mpcore, "num-irq", NS_NUM_IRQ);
    qdev_prop_set_uint32(mpcore, "periphclk-frequency", NS_PERIPH_HZ);
    busdev = SYS_BUS_DEVICE(mpcore);
    sysbus_realize_and_unref(busdev, &error_fatal);
    sysbus_mmio_map(busdev, 0, NS_MPCORE_BASE);
    for (n = 0; n < machine->smp.cpus; n++) {
        sysbus_connect_irq(busdev, n, cpu_irq[n]);
    }
    /* PL310: 16 ways x 16 KiB = 256 KiB. AUX_CTRL is what CFE leaves (OpenWrt
     * on real hardware: "modifies aux control register: 0x0a130000 -> ..."). */
    {
        DeviceState *l2 = qdev_new("l2x0");

        qdev_prop_set_uint32(l2, "aux-ctrl", 0x0a130000);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(l2), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(l2), 0, NS_L2CC_BASE);
    }

    /* ChipCommon-A; the UARTs sit on top of its register block. */
    cc = qdev_new(TYPE_BCM_NS_CHIPCOMMON);
    qdev_prop_set_uint32(cc, "chipid", NS_CC_CHIPID);
    qdev_prop_set_uint32(cc, "capabilities", NS_CC_CAPS);
    qdev_prop_set_uint32(cc, "eromptr", NS_EROM_BASE);
    qdev_prop_set_uint32(cc, "watchdog-hz", NS_ALP_HZ);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(cc), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(cc), 0, NS_CCA_BASE, -1);
    sysbus_connect_irq(SYS_BUS_DEVICE(cc), 0, qdev_get_gpio_in(mpcore, NS_SPI_CCA));
    /* priority 0 > ChipCommon's -1, so the UART windows win */
    ns_uart(sysmem, NS_UART0_BASE, qdev_get_gpio_in_named(cc, "uart-irq", 0),
            serial_hd(0));
    ns_uart(sysmem, NS_UART1_BASE, qdev_get_gpio_in_named(cc, "uart-irq", 1),
            serial_hd(1));

    /* IDM wrappers. Straps: ROM wrapper iostatus = NAND boot; NAND controller
     * ready. */
    idm = qdev_new(TYPE_BCM_NS_IDM);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(idm), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(idm), 0, BCM_NS_IDM_BASE);
    bcm_ns_idm_set_iostatus(BCM_NS_IDM(idm), NS_ROM_WRAP, 0x1);   /* SISF_NS_BOOTDEV_NAND */
    bcm_ns_idm_set_iostatus(BCM_NS_IDM(idm), NS_NAND_WRAP, 0x1);  /* NAND_RO_CTRL_READY */

    /* NAND controller (nandcore). Its IDM wrapper page overlays the generic
     * IDM so the endian toggle (ioctrl bit 24) reaches the data path. The
     * part is the ESMT F59L1G81 the stock boot log reports (C8 D1 80 95 40);
     * config/acc_control are what CFE leaves programmed. */
    {
        DeviceState *nand = qdev_new("bcm-ns-nand");
        DriveInfo *dinfo = drive_get(IF_MTD, 0, 0);

        if (dinfo) {
            qdev_prop_set_drive_err(nand, "drive", blk_by_legacy_dinfo(dinfo),
                                    &error_fatal);
        }
        qdev_prop_set_uint64(nand, "device-id", 0x409580d1c8ULL);
        qdev_prop_set_uint32(nand, "revision", 0x0601);         /* [assume] v6.1 */
        qdev_prop_set_uint32(nand, "config-cs0", 0x25152300);
        qdev_prop_set_uint32(nand, "acc-control-cs0", 0xc0080010);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(nand), &error_fatal);
        s->nand = dinfo ? nand : NULL;
        sysbus_mmio_map(SYS_BUS_DEVICE(nand), 0, NS_NAND_REGS);
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(nand), 1, NS_NAND_WRAP, 1);
        sysbus_mmio_map(SYS_BUS_DEVICE(nand), 2, NS_NAND_WINDOW);
    }

    /* Ethernet. GMAC2 is the IMP port (8) of the BCM53012, GMAC1 is port 7,
     * GMAC0 is not wired (port 5 carries the RTL8365MB), GMAC3 is unused.
     * The first -nic goes to LAN1 (switch port 3), a second (or model=wan)
     * to WAN (port 4); -global bcm53012.netdev-pN=<id> picks any port 0-4. */
    {
        DeviceState *sw = qdev_new(TYPE_BCM53012);
        DeviceState *gmac[4], *rtl;
        static const struct { int port; const char *alias; } ext[] = {
            { 3, "lan" }, { 4, "wan" },
        };

        for (n = 0; n < ARRAY_SIZE(ext); n++) {
            NICInfo *nd = qemu_find_nic_info(TYPE_BCM53012, true, ext[n].alias);
            g_autofree char *prop = g_strdup_printf("netdev-p%d", ext[n].port);

            if (nd) {
                qdev_prop_set_netdev(sw, prop, nd->netdev);
                nd->instantiated = true;
            }
        }
        sysbus_realize_and_unref(SYS_BUS_DEVICE(sw), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(sw), 0, NS_SRAB_BASE);

        for (n = 0; n < 4; n++) {
            gmac[n] = qdev_new(TYPE_BCM_NS_GMAC);
            sysbus_realize_and_unref(SYS_BUS_DEVICE(gmac[n]), &error_fatal);
            sysbus_mmio_map(SYS_BUS_DEVICE(gmac[n]), 0, NS_GMAC_BASE + n * 0x1000);
            sysbus_connect_irq(SYS_BUS_DEVICE(gmac[n]), 0,
                               qdev_get_gpio_in(mpcore, NS_SPI_GMAC + n));
            /* SISF_DMA64: hnddma uses the dma64 engine only when this is set */
            bcm_ns_idm_set_iostatus(BCM_NS_IDM(idm), NS_GMAC_WRAP + n * 0x1000, 0x1000);
        }
        bcm53012_connect_gmac(BCM53012(sw), 7, BCM_NS_GMAC(gmac[1]));
        bcm53012_connect_gmac(BCM53012(sw), 8, BCM_NS_GMAC(gmac[2]));

        /* RTL8365MB: Realtek SMI on GPIO 6 (SCK) / 7 (SDA), reset on GPIO 10
         * (active low); EXT1 RGMII into BCM53012 port 5. UTP ports 0-3 are
         * LAN5-LAN8: -nic ...,model=lan5 .. lan8, or -global rtl8365mb.netdev-pN. */
        rtl = qdev_new(TYPE_RTL8365MB);
        for (n = 0; n < RTL8365MB_NUTP; n++) {
            g_autofree char *alias = g_strdup_printf("lan%d", 5 + n);
            g_autofree char *prop = g_strdup_printf("netdev-p%d", n);
            NICInfo *nd = qemu_find_nic_info(TYPE_RTL8365MB, false, alias);

            if (nd) {
                qdev_prop_set_netdev(rtl, prop, nd->netdev);
                nd->instantiated = true;
            }
        }
        qdev_realize_and_unref(rtl, NULL, &error_fatal);
        qdev_connect_gpio_out_named(cc, "gpio-out", 6, qdev_get_gpio_in_named(rtl, "sck", 0));
        qdev_connect_gpio_out_named(cc, "gpio-out", 7, qdev_get_gpio_in_named(rtl, "sda", 0));
        qdev_connect_gpio_out_named(cc, "gpio-out", 10,
                                    qdev_get_gpio_in_named(rtl, "reset-n", 0));
        qdev_connect_gpio_out_named(rtl, "sda-out", 0, qdev_get_gpio_in_named(cc, "gpio-in", 7));
        rtl8365mb_connect_uplink(RTL8365MB(rtl), ns_rtl_to_robo, sw);
        bcm53012_connect_mac(BCM53012(sw), 5, ns_robo_to_rtl, rtl);
    }

    /* EROM */
    erom_words = ns_build_erom(erom);
    memory_region_init_rom(&s->erom, NULL, "ns.erom", 0x1000, &error_fatal);
    memory_region_add_subregion(sysmem, NS_EROM_BASE, &s->erom);
    rom_add_blob_fixed("ns.erom", erom, erom_words * sizeof(uint32_t), NS_EROM_BASE);

    bcm_ns_regfile_create("ns.dmu", NS_DMU_BASE, ns_dmu_init);
    bcm_ns_regfile_create("ns.ihost-cru", NS_IHOST_BASE, ns_ihost_init);

    /*
     * Kernel: raw Image at 0x8000, as CFE leaves it after LZMA decompression.
     * With -kernel, that file is loaded (and reloaded on every reset); without
     * it, the kernel comes from the TRX in flash on every reset, as on the
     * device, so a reset after a guest firmware upgrade boots the new kernel.
     */
    if (!machine->kernel_filename) {
        if (!s->nand) {
            error_report("bcm47094-router: needs -kernel <raw Image> or a NAND "
                         "image (-drive if=mtd) holding a TRX to boot from");
            exit(1);
        }
        qemu_register_reset(ns_flash_boot_reset, s);
        return;
    }
    if (load_image_targphys(machine->kernel_filename, NS_KERNEL_LOAD,
                            NS_DRAM_LO_SIZE - NS_KERNEL_LOAD, NULL) < 0) {
        error_report("bcm47094-router: could not load kernel '%s'", machine->kernel_filename);
        exit(1);
    }
}

static void ns_machine_init(MachineClass *mc)
{
    mc->desc = "Broadcom BCM47094 Northstar router board (Cortex-A9 MPCore)";
    mc->init = ns_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a9");
    mc->max_cpus = 2;
    mc->min_cpus = 2;
    mc->default_cpus = 2;
    mc->default_ram_size = 512 * MiB;
    mc->default_ram_id = "ns.dram";
}

DEFINE_MACHINE_ARM("bcm47094-router", ns_machine_init)
