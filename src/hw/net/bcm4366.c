/*
 * Broadcom BCM4366 802.11ac radio as a PCIe full-dongle endpoint: the chip.
 *
 * What the host driver (the vendor's dhd) sees and programs, from the HND
 * sources' contracts (siutils/aiutils, pcicfg.h, pcie_core.h, sbsysmem.h):
 *   - Config space: a unit's (14e4:4365 rev 3, class 0x0280; PM, MSI, a
 *     vendor capability, PCIe v2 Gen2 x1, AER, DSN, power budget, VC, LTR,
 *     resizable BAR, L1 substates). Broadcom's own registers live in the
 *     vendor capability's range: BAR0 window 1 (0x80), window 2 (0x70),
 *     core-2 windows (0x74, 0x78), SPROM control (0x88), the host-to-dongle
 *     mailbox (0x98).
 *   - BAR0 (32 KiB): 4 KiB at the backplane address in 0x80, the next 4 KiB
 *     at 0x70 (a core's wrapper), the PCIe2 core's own registers at 8 KiB,
 *     ChipCommon at 12 KiB, and two more windows (0x74, 0x78) at 16 and 20
 *     KiB.
 *   - BAR2 (4 MiB): the dongle's address space from 0, so SYSMEM's RAM
 *     (2 MiB at 0x200000) is BAR2 + 0x200000. Below it is the CA7's vector
 *     page (dhd writes the reset instruction at 0); the mask ROM behind it is
 *     not modelled and reads as zero.
 *   - The backplane: ChipCommon (chip 0x4366 rev 3, a unit's capabilities
 *     0x58400009), D11 (rev 64, as the unit's firmware reports), PCIe2, ARM
 *     CA7, SYSMEM and GCI, each with an AI wrapper, listed in an enumeration
 *     ROM that ChipCommon's eromptr points at. Core revisions other than
 *     D11's and the core addresses other than ChipCommon's are [assume]; the
 *     host only needs them self-consistent. The PCIe2 core's window and
 *     wrapper (0x18002000, 0x18106000) are what a unit's config space holds
 *     after dhd's last si_setcore, which is to the PCIe2 core.
 *   - PCIe2 core: mailboxint (0x48, write 1 to clear) and its mask (0x4c)
 *     drive INTx; H2D doorbell 0 (0x140) and 1 (0x144).
 *   - The CA7's wrapper: when the host takes the ARM out of reset and halt
 *     after a download, the firmware half starts; putting it back in reset or
 *     halt stops it.
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pcie.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/dma.h"
#include "hw/net/bcm4366.h"

#define BAR0_SIZE           (32 * KiB)
#define BP_ENUM_BASE        0x18000000
#define BP_WRAP_BASE        0x18100000
#define BP_EROM_BASE        0x18110000

/* config space */
#define CFG_BAR0_WIN2       0x70
#define CFG_BAR0_CORE2_WIN  0x74
#define CFG_BAR0_CORE2_WIN2 0x78
#define CFG_BAR0_WIN        0x80
#define CFG_SPROM_CONTROL   0x88
#define CFG_SBMBX           0x98

/* wrapper (aidmp.h) */
#define AI_IOCTRL           0x408
#define AI_IOSTATUS         0x500
#define AI_RESETCTRL        0x800
#define AI_RESETSTATUS      0x804
#define SICF_CLOCK_EN       0x0001
#define SICF_FGC            0x0002
#define SICF_CPUHALT        0x0020

/* PCIe2 core */
#define PCIE_MAILBOXINT     0x48
#define PCIE_MAILBOXMASK    0x4c
#define PCIE_CONFIGADDR     0x120
#define PCIE_CONFIGDATA     0x124
#define PCIE_H2D_DB0        0x140
#define PCIE_H2D_DB1        0x144
#define PCIE_CLK_CTL_ST     0x1e0
#define MB_D2H0_DB0         0x10000
#define MB_FN0_0            0x0100

enum { C_CC, C_D11, C_PCIE, C_CA7, C_SYSMEM, C_GCI };

static const struct {
    const char *name;
    uint16_t cid;
    uint8_t rev;
    int wrap;                       /* wrapper page, or -1 */
} cores[BCM4366_NCORES] = {
    [C_CC]     = { "chipcommon", 0x800, 54, 0 },
    [C_D11]    = { "d11",        0x812, 64, 1 },
    [C_PCIE]   = { "pcie2",      0x83c, 16, 6 },
    [C_CA7]    = { "armca7",     0x847, 2,  2 },
    [C_SYSMEM] = { "sysmem",     0x849, 3,  3 },
    [C_GCI]    = { "gci",        0x840, 7,  -1 },
};

#define CORE_BASE(i)        (BP_ENUM_BASE + (i) * 0x1000)
#define WRAP_BASE(w)        (BP_WRAP_BASE + (w) * 0x1000)

/* ChipCommon: AI backplane, the cores above, package 0 [assume], rev 3. */
#define CC_CHIPID           ((1u << 28) | (BCM4366_NCORES << 24) | (0u << 20) | \
                             (3u << 16) | 0x4366)
#define CC_CAPS             0x58400009  /* a unit's (firmware console: cccap) */
#define CC_PMUCAPS          0x0000001c  /* PMU rev 28 [assume] */
#define CC_PMUSTATUS        0x0000000c  /* HT and ALP available */
#define CC_CLKCTLST         0x00030000  /* HT and ALP available */

/* SYSMEM: coreinfo gives the RAM bank count in bits 4:0 and the ROM bank
 * count in bits 9:5; RAM banks are numbered after the ROM's (DHD 1.363's
 * si_sysmem_size). RAM: 8 banks of 256 KiB = 2 MiB, the size the image's
 * own RAMS header gives. ROM: 8 such banks [assume]. */
#define SYSMEM_NBANKS       8
#define SYSMEM_NROMBANKS    8
#define SYSMEM_BANKSZ_FIELD ((BCM4366_RAM_SIZE / SYSMEM_NBANKS) / 8192 - 1)

/* A unit's config space above the header (both radios read the same). */
static const struct { uint16_t off; uint32_t val; } ep_cfg[] = {
    { 0x048, 0x06035801 }, { 0x04c, 0x00004008 },       /* PM: D1 D2 */
    { 0x058, 0x008a6805 },                              /* MSI, 64-bit */
    { 0x068, 0x0044ac09 }, { 0x06c, 0x00000114 },       /* vendor-specific */
    { CFG_SPROM_CONTROL, 0x00000300 },
    { 0x0ac, 0x00020010 }, { 0x0b0, 0x00008f82 },       /* PCIe v2 endpoint */
    { 0x0b4, 0x00102810 }, { 0x0b8, 0x0046dc12 },       /* Gen2 x1 */
    { 0x0bc, 0x10120000 }, { 0x0d0, 0x0008081f }, { 0x0dc, 0x00000001 },
    { 0x100, 0x13c10001 }, { 0x10c, 0x00062030 },       /* AER */
    { 0x114, 0x00002000 }, { 0x118, 0x000000a0 },
    { 0x13c, 0x15010003 }, { 0x140, 0xff000000 }, { 0x144, 0x000000ff }, /* DSN */
    { 0x150, 0x16010004 }, { 0x158, 0x000562c4 }, { 0x15c, 0x00000001 },
    { 0x160, 0x1b010002 }, { 0x174, 0x800000ff },       /* VC */
    { 0x1b0, 0x22010018 },                              /* LTR */
    { 0x220, 0x24010015 }, { 0x224, 0x00000070 }, { 0x228, 0x00000222 },
    { 0x240, 0x0001001e }, { 0x244, 0x00fa081f }, { 0x24c, 0x00000028 },
};

/* ---- helpers for the firmware -------------------------------------------- */

void bcm4366_tcm_read(BCM4366State *s, uint32_t addr, void *buf, size_t len)
{
    uint8_t *ram = memory_region_get_ram_ptr(&s->tcm);

    if (addr >= BCM4366_TCM_SIZE || len > BCM4366_TCM_SIZE - addr) {
        memset(buf, 0, len);
        return;
    }
    memcpy(buf, ram + addr, len);
}

void bcm4366_tcm_write(BCM4366State *s, uint32_t addr, const void *buf, size_t len)
{
    uint8_t *ram = memory_region_get_ram_ptr(&s->tcm);

    if (addr >= BCM4366_TCM_SIZE || len > BCM4366_TCM_SIZE - addr) {
        return;
    }
    memcpy(ram + addr, buf, len);
    memory_region_set_dirty(&s->tcm, addr, len);
}

bool bcm4366_dma_read(BCM4366State *s, uint64_t addr, void *buf, size_t len)
{
    return pci_dma_read(PCI_DEVICE(s), addr, buf, len) == MEMTX_OK;
}

bool bcm4366_dma_write(BCM4366State *s, uint64_t addr, const void *buf, size_t len)
{
    return pci_dma_write(PCI_DEVICE(s), addr, buf, len) == MEMTX_OK;
}

static void bcm4366_update_irq(BCM4366State *s)
{
    uint32_t *r = s->creg[C_PCIE];

    pci_set_irq(PCI_DEVICE(s), !!(r[PCIE_MAILBOXINT / 4] & r[PCIE_MAILBOXMASK / 4]));
}

void bcm4366_d2h_doorbell(BCM4366State *s)
{
    s->creg[C_PCIE][PCIE_MAILBOXINT / 4] |= MB_D2H0_DB0;
    bcm4366_update_irq(s);
}

void bcm4366_d2h_mailbox(BCM4366State *s)
{
    s->creg[C_PCIE][PCIE_MAILBOXINT / 4] |= MB_FN0_0;
    bcm4366_update_irq(s);
}

/* ---- the backplane -------------------------------------------------------- */

static size_t bcm4366_build_erom(uint32_t *e)
{
    size_t n = 0;

    for (int i = 0; i < BCM4366_NCORES; i++) {
        uint32_t nsw = cores[i].wrap >= 0;

        e[n++] = (0x4bfu << 20) | ((uint32_t)cores[i].cid << 8) | 1;      /* CIA */
        e[n++] = ((uint32_t)cores[i].rev << 24) | (nsw << 19) | (1 << 9) | 1;
        e[n++] = CORE_BASE(i) | 0x4 | 1;                                  /* slave 4K */
        if (nsw) {
            e[n++] = WRAP_BASE(cores[i].wrap) | 0x80 | 0x4 | 1;           /* wrapper */
        }
    }
    e[n++] = 0x0000000f;                                                  /* END */
    return n;
}

static int core_of(BCM4366State *s, MemoryRegion *mr)
{
    return mr - s->core;
}

static uint64_t core_read(void *opaque, hwaddr off, unsigned size)
{
    MemoryRegion *mr = opaque;
    BCM4366State *s = BCM4366(mr->owner);
    int c = core_of(s, mr);
    uint32_t *r = s->creg[c];

    switch (c) {
    case C_CC:
        switch (off) {
        case 0x000: return CC_CHIPID;
        case 0x004: return CC_CAPS;
        case 0x0fc: return BP_EROM_BASE;
        case 0x1e0: return CC_CLKCTLST | (r[off / 4] & 0xffff);
        case 0x604: return CC_PMUCAPS;
        case 0x608: return CC_PMUSTATUS;
        }
        break;
    case C_PCIE:
        switch (off) {
        case PCIE_CONFIGDATA:
            return s->pcie_cfg[(r[PCIE_CONFIGADDR / 4] & 0xffc) / 4];
        case PCIE_H2D_DB0:
        case PCIE_H2D_DB1:
            return 0;
        case PCIE_CLK_CTL_ST:
            return CC_CLKCTLST | (r[off / 4] & 0xffff);
        }
        break;
    case C_SYSMEM:
        switch (off) {
        case 0x000:                                     /* coreinfo */
            return (SYSMEM_NROMBANKS << 5) | SYSMEM_NBANKS;
        case 0x040:                                     /* bankinfo */
            return (r[0x10 / 4] & 0xff) < SYSMEM_NROMBANKS + SYSMEM_NBANKS
                   ? SYSMEM_BANKSZ_FIELD : 0;
        }
        break;
    }
    return r[off / 4];
}

static void core_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MemoryRegion *mr = opaque;
    BCM4366State *s = BCM4366(mr->owner);
    int c = core_of(s, mr);
    uint32_t *r = s->creg[c];

    if (c == C_PCIE) {
        switch (off) {
        case PCIE_MAILBOXINT:
            r[off / 4] &= ~(uint32_t)val;
            bcm4366_update_irq(s);
            return;
        case PCIE_MAILBOXMASK:
            r[off / 4] = val;
            bcm4366_update_irq(s);
            return;
        case PCIE_CONFIGDATA:
            s->pcie_cfg[(r[PCIE_CONFIGADDR / 4] & 0xffc) / 4] = val;
            return;
        case PCIE_H2D_DB0:
            bcm4366_fw_doorbell(s);
            return;
        case PCIE_H2D_DB1:
            bcm4366_fw_h2d_mailbox(s);
            return;
        }
    }
    r[off / 4] = val;
}

static const MemoryRegionOps core_ops = {
    .read = core_read,
    .write = core_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* CA7 running: out of reset, clocked, not halted, no forced gated clock. */
static void bcm4366_arm_update(BCM4366State *s)
{
    int w = cores[C_CA7].wrap;
    bool run = !(s->resetctrl[w] & 1) &&
               (s->ioctrl[w] & (SICF_CLOCK_EN | SICF_FGC | SICF_CPUHALT)) == SICF_CLOCK_EN;

    if (!run) {
        if (!s->arm_halted) {
            BCM4366_LOG(s, "CA7 halted or in reset");
            s->arm_halted = true;
            bcm4366_fw_stop(s);
        }
    } else if (s->arm_halted) {
        BCM4366_LOG(s, "CA7 released");
        s->arm_halted = false;
        bcm4366_fw_start(s);
    }
}

static uint64_t wrap_read(void *opaque, hwaddr off, unsigned size)
{
    BCM4366State *s = opaque;
    int w = off >> 12;

    switch (off & 0xfff) {
    case AI_IOCTRL:     return s->ioctrl[w];
    case AI_RESETCTRL:  return s->resetctrl[w];
    }
    return 0;                           /* iostatus, resetstatus, error log */
}

static void wrap_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    BCM4366State *s = opaque;
    int w = off >> 12;

    switch (off & 0xfff) {
    case AI_IOCTRL:
        s->ioctrl[w] = val & 0xffff;
        break;
    case AI_RESETCTRL:
        s->resetctrl[w] = val & 1;
        break;
    default:
        return;
    }
    if (w == cores[C_CA7].wrap) {
        bcm4366_arm_update(s);
    }
}

static const MemoryRegionOps wrap_ops = {
    .read = wrap_read,
    .write = wrap_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* Nothing decodes here: reads 0, writes vanish, both logged. */
static uint64_t bg_read(void *opaque, hwaddr off, unsigned size)
{
    BCM4366State *s = opaque;

    qemu_log_mask(LOG_UNIMP, "bcm4366[%u]: backplane read 0x%" HWADDR_PRIx "/%u\n",
                  s->radio, off, size);
    return 0;
}

static void bg_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    BCM4366State *s = opaque;

    qemu_log_mask(LOG_UNIMP, "bcm4366[%u]: backplane write 0x%" HWADDR_PRIx
                  " = 0x%" PRIx64 "/%u\n", s->radio, off, val, size);
}

static const MemoryRegionOps bg_ops = {
    .read = bg_read,
    .write = bg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* ---- config space ---------------------------------------------------------- */

static void bcm4366_map_windows(BCM4366State *s)
{
    static const uint8_t reg[4] = {
        CFG_BAR0_WIN, CFG_BAR0_WIN2, CFG_BAR0_CORE2_WIN, CFG_BAR0_CORE2_WIN2,
    };
    uint8_t *c = PCI_DEVICE(s)->config;

    for (int i = 0; i < 4; i++) {
        memory_region_set_alias_offset(&s->bar0_win[i], pci_get_long(c + reg[i]) & ~0xfffu);
    }
}

static void bcm4366_config_write(PCIDevice *d, uint32_t addr, uint32_t val, int len)
{
    BCM4366State *s = BCM4366(d);

    pci_default_write_config(d, addr, val, len);
    if (ranges_overlap(addr, len, CFG_BAR0_WIN2, 12) ||
        ranges_overlap(addr, len, CFG_BAR0_WIN, 4)) {
        bcm4366_map_windows(s);
    }
    if (ranges_overlap(addr, len, CFG_SBMBX, 4) && val) {
        bcm4366_fw_h2d_mailbox(s);
    }
}

static void ep_load_cfg(PCIDevice *d)
{
    for (size_t i = 0; i < ARRAY_SIZE(ep_cfg); i++) {
        pci_set_long(d->config + ep_cfg[i].off, ep_cfg[i].val);
    }
    pci_set_long(d->config + CFG_BAR0_WIN, BP_ENUM_BASE);
    pci_set_long(d->config + CFG_BAR0_WIN2, BP_WRAP_BASE);
    pci_set_long(d->config + CFG_BAR0_CORE2_WIN, BP_ENUM_BASE);
    pci_set_long(d->config + CFG_BAR0_CORE2_WIN2, BP_WRAP_BASE);
}

static void bcm4366_reset(DeviceState *dev)
{
    BCM4366State *s = BCM4366(dev);

    bcm4366_fw_stop(s);
    ep_load_cfg(PCI_DEVICE(s));
    memset(s->creg, 0, sizeof(s->creg));
    memset(s->pcie_cfg, 0, sizeof(s->pcie_cfg));
    /* Every core out of reset and clocked; the CA7 runs its ROM, which is
     * not the firmware: nothing answers until a host downloads an image. */
    for (int w = 0; w < BCM4366_NWRAP; w++) {
        s->ioctrl[w] = SICF_CLOCK_EN;
        s->resetctrl[w] = 0;
    }
    s->arm_halted = false;
    bcm4366_map_windows(s);
    bcm4366_update_irq(s);
}

static void bcm4366_realize(PCIDevice *d, Error **errp)
{
    BCM4366State *s = BCM4366(d);
    uint8_t *w = d->wmask;
    uint32_t erom[64];
    size_t n;

    d->exp.exp_cap = 0xac;
    d->config[PCI_INTERRUPT_PIN] = 1;
    pci_set_word(d->config + PCI_STATUS, PCI_STATUS_CAP_LIST);
    d->config[PCI_CAPABILITY_LIST] = 0x48;
    ep_load_cfg(d);

    /* Above the header only what is listed below is writable. */
    memset(w + PCI_CONFIG_HEADER_SIZE, 0, PCIE_CONFIG_SPACE_SIZE - PCI_CONFIG_HEADER_SIZE);
    pci_set_word(w + 0x4c, 0x0103);                     /* PMCSR */
    pci_set_word(w + 0x5a, 0x0071);                     /* MSI enable, MME */
    pci_set_long(w + 0x5c, 0xfffffffc);
    pci_set_long(w + 0x60, 0xffffffff);
    pci_set_word(w + 0x64, 0xffff);
    memset(w + 0x6c, 0xff, 0xac - 0x6c);                /* Broadcom registers */
    pci_set_word(w + 0xac + PCI_EXP_DEVCTL, 0xffff);
    pci_set_word(w + 0xac + PCI_EXP_LNKCTL, 0x0ffb);
    pci_set_word(w + 0xac + PCI_EXP_DEVCTL2, 0xffff);
    pci_set_word(w + 0xac + PCI_EXP_LNKCTL2, 0xffff);

    /* the backplane */
    memory_region_init(&s->bp, OBJECT(s), "bcm4366.backplane", 4 * GiB);
    memory_region_init_io(&s->bp_bg, OBJECT(s), &bg_ops, s, "bcm4366.bp-unmapped", 4 * GiB);
    memory_region_add_subregion_overlap(&s->bp, 0, &s->bp_bg, -1);
    memory_region_init_ram(&s->tcm, OBJECT(s), "bcm4366.tcm", BCM4366_TCM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(&s->bp, 0, &s->tcm);
    for (int i = 0; i < BCM4366_NCORES; i++) {
        memory_region_init_io(&s->core[i], OBJECT(s), &core_ops, &s->core[i],
                              cores[i].name, 0x1000);
        memory_region_add_subregion(&s->bp, CORE_BASE(i), &s->core[i]);
    }
    memory_region_init_io(&s->wrap, OBJECT(s), &wrap_ops, s, "bcm4366.wrappers",
                          BCM4366_NWRAP * 0x1000);
    memory_region_add_subregion(&s->bp, BP_WRAP_BASE, &s->wrap);
    memory_region_init_rom(&s->erom, OBJECT(s), "bcm4366.erom", 0x1000, &error_fatal);
    n = bcm4366_build_erom(erom);
    memcpy(memory_region_get_ram_ptr(&s->erom), erom, n * 4);
    memory_region_add_subregion(&s->bp, BP_EROM_BASE, &s->erom);

    /* BAR0: windows onto it */
    memory_region_init(&s->bar0, OBJECT(s), "bcm4366.bar0", BAR0_SIZE);
    memory_region_init_io(&s->bar0_bg, OBJECT(s), &bg_ops, s, "bcm4366.bar0-unmapped",
                          BAR0_SIZE);
    memory_region_add_subregion_overlap(&s->bar0, 0, &s->bar0_bg, -1);
    {
        static const hwaddr at[4] = { 0x0000, 0x1000, 0x4000, 0x5000 };

        for (int i = 0; i < 4; i++) {
            memory_region_init_alias(&s->bar0_win[i], OBJECT(s), "bcm4366.bar0-win",
                                     &s->bp, BP_ENUM_BASE, 0x1000);
            memory_region_add_subregion(&s->bar0, at[i], &s->bar0_win[i]);
        }
    }
    memory_region_init_alias(&s->bar0_pcie, OBJECT(s), "bcm4366.bar0-pcie2",
                             &s->core[C_PCIE], 0, 0x1000);
    memory_region_add_subregion(&s->bar0, 0x2000, &s->bar0_pcie);
    memory_region_init_alias(&s->bar0_cc, OBJECT(s), "bcm4366.bar0-cc",
                             &s->core[C_CC], 0, 0x1000);
    memory_region_add_subregion(&s->bar0, 0x3000, &s->bar0_cc);

    /* BAR2: the dongle's memory */
    memory_region_init_alias(&s->bar2, OBJECT(s), "bcm4366.bar2", &s->bp, 0,
                             BCM4366_TCM_SIZE);

    pci_register_bar(d, 0, PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64, &s->bar0);
    pci_register_bar(d, 2, PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64, &s->bar2);

    bcm4366_fw_realize(s);
}

static int bcm4366_post_load(void *opaque, int version_id)
{
    BCM4366State *s = opaque;

    bcm4366_map_windows(s);
    bcm4366_update_irq(s);
    return 0;
}

static const VMStateDescription bcm4366_vmstate = {
    .name = "bcm4366",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = bcm4366_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, BCM4366State),
        VMSTATE_UINT32_2DARRAY(creg, BCM4366State, BCM4366_NCORES, 1024),
        VMSTATE_UINT32_ARRAY(ioctrl, BCM4366State, BCM4366_NWRAP),
        VMSTATE_UINT32_ARRAY(resetctrl, BCM4366State, BCM4366_NWRAP),
        VMSTATE_UINT32_ARRAY(pcie_cfg, BCM4366State, 1024),
        VMSTATE_BOOL(arm_halted, BCM4366State),
        VMSTATE_STRUCT(fw, BCM4366State, 1, bcm4366_fw_vmstate, BCM4366Fw),
        VMSTATE_END_OF_LIST()
    },
};

static const Property bcm4366_props[] = {
    DEFINE_PROP_UINT32("radio", BCM4366State, radio, 0),
    DEFINE_PROP_UINT32("devid", BCM4366State, devid, 0x43c4),
    DEFINE_PROP_STRING("wl", BCM4366State, wl),
    DEFINE_PROP_BOOL("debug", BCM4366State, debug, false),
};

static void bcm4366_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = bcm4366_realize;
    k->config_write = bcm4366_config_write;
    k->vendor_id = 0x14e4;
    k->device_id = 0x4365;
    k->revision = 3;
    k->class_id = PCI_CLASS_NETWORK_OTHER;
    k->subsystem_vendor_id = 0x14e4;
    k->subsystem_id = 0x4365;
    dc->desc = "Broadcom BCM4366 802.11ac PCIe full-dongle radio";
    dc->vmsd = &bcm4366_vmstate;
    device_class_set_legacy_reset(dc, bcm4366_reset);
    device_class_set_props(dc, bcm4366_props);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo bcm4366_types[] = {
    {
        .name          = TYPE_BCM4366,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(BCM4366State),
        .class_init    = bcm4366_class_init,
        .interfaces    = (const InterfaceInfo[]) {
            { INTERFACE_PCIE_DEVICE },
            { }
        },
    },
};

DEFINE_TYPES(bcm4366_types)
