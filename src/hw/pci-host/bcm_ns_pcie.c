/*
 * Broadcom Northstar PCIe Gen2 root complex (iProc PAXB, core 0x501 rev 7).
 *
 * Register contract from the vendor kernel's host driver
 * (arch/arm/plat-brcm/bcm5301x_pcie.c), which is the only software that
 * programs it:
 *   - 0x000 CONTROL: bit 0 releases PERST# to the link partner;
 *   - 0x120/0x124 EXT_CFG_ADDR/DATA: the root port's own 4 KiB config space;
 *   - 0x1f8/0x1fc CFG_ADDR/DATA: config cycles on the link, addressed
 *     bus[27:20] dev[19:15] fn[14:12] reg[11:2] type[1:0] (type 0 goes to the
 *     device on the link whatever the bus number);
 *   - 0x330 RC_INTX_EN, 0x334 RC_INTX_CSR: INTA-D from the link into the
 *     port's interrupt;
 *   - 0x400-0x4ff: the root port's config header again (the kernel enables
 *     memory and bus mastering through 0x404);
 *   - 0xc00-0xd5c: inbound (IMAP/IARR) and outbound (OMAP/OARR) address
 *     translation.
 * Reset values and holes are a unit's: 0x128, 0x12c, 0x1e0, 0xd18 and 0xd1c
 * raise a bus error there, as they do here.
 *
 * The root port's config space is a unit's (Broadcom 14e4:d612, PM, PCIe v2
 * root port, AER and a vendor capability, and the vendor registers above
 * 0x400), so the kernel's PCI messages and /proc/bus/pci match a unit. Link
 * status is computed: up while PERST# is released and a device is on the
 * secondary bus, at the speed LNKCTL2 asks for (the endpoint's link is
 * Gen2 x1).
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pcie.h"
#include "hw/pci-host/bcm_ns_pcie.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"

enum {
    PCIE_CONTROL        = 0x000,
    PCIE_EXT_CFG_ADDR   = 0x120,
    PCIE_EXT_CFG_DATA   = 0x124,
    PCIE_CFG_ADDR       = 0x1f8,
    PCIE_CFG_DATA       = 0x1fc,
    PCIE_RC_INTX_EN     = 0x330,
    PCIE_RC_INTX_CSR    = 0x334,
    PCIE_HDR_OFF        = 0x400,
    PCIE_IMAP0          = 0xc00,
    PCIE_IMAP1          = 0xc80,
    PCIE_IMAP2          = 0xcc0,
    PCIE_IARR           = 0xd00,        /* + 8n, n = 0..2 */
    PCIE_OARR           = 0xd20,
    PCIE_OMAP           = 0xd40,
};

#define CONTROL_PERST_OFF   (1u << 0)
#define RP_EXP_CAP          0xac        /* root port PCIe capability */
#define RP_LNKSTA           (RP_EXP_CAP + PCI_EXP_LNKSTA)
#define RP_SLTSTA           (RP_EXP_CAP + PCI_EXP_SLTSTA)
#define RP_LNKCTL2          (RP_EXP_CAP + PCI_EXP_LNKCTL2)
#define RP_MPS_SHADOW       0x4d4       /* hw_init's MPS; devcap mirrors it */
#define OWIN_SLICE          (64 * MiB)

/* Holes: a unit's PAXB raises a bus error for these. */
static bool pcie_reg_hole(hwaddr off)
{
    return off == 0x128 || off == 0x12c || off == 0x1e0 ||
           off == 0xd18 || off == 0xd1c;
}

/*
 * A unit's register words at boot end; [assume]d to be the reset values for
 * the ones the kernel does not write. IMAP0 window 2 points at the port's
 * own core, filled in at reset.
 */
static const struct { uint16_t off; uint32_t val; } pcie_reg_reset[] = {
    { 0xc00, 0x18000001 }, { 0xc04, 0x18001001 }, { 0xc0c, 0xffff1001 },
};
/* Port 1 only, as the boot loader leaves it on a unit: IMAP1 enabled at 0 and
 * IARR1 128 MiB at PCI 0x4000000000000000, above what the model maps. */
static const struct { uint16_t off; uint32_t val; } pcie_reg_reset_port1[] = {
    { 0xc80, 0x00000001 }, { 0xd08, 0x00000080 }, { 0xd0c, 0x40000000 },
};

/* ---- root port config space, a unit's (port 1; port 2 differs only in
 * two PHY status words at 0x988/0x994) ---- */
static const struct { uint16_t off; uint32_t val; } rp_cfg[] = {
    { 0x048, 0xc803ac01 }, { 0x04c, 0x00002008 },       /* PM: D0/D3hot/D3cold PME */
    { 0x0ac, 0x00420010 }, { 0x0b0, 0x00008002 },       /* PCIe v2 root port */
    { 0x0b4, 0x00102810 }, { 0x0b8, 0x00655c12 },       /* Gen2 x1 */
    { 0x0bc, 0x00000000 }, { 0x0c4, 0x00000000 },
    { 0x0c8, 0x00010000 }, { 0x0d0, 0x0008001f }, { 0x0dc, 0x00000002 },
    { 0x100, 0x18010001 }, { 0x10c, 0x00062030 },       /* AER, next 0x180 */
    { 0x114, 0x00002000 }, { 0x118, 0x000000a0 },
    { 0x180, 0x0001000b }, { 0x184, 0x02800000 },       /* vendor-specific */
    { 0x408, 0x00010010 }, { 0x40c, 0xc0000000 }, { 0x418, 0x00004000 },
    { 0x434, 0x14e4d612 }, { 0x438, 0xd61214e4 }, { 0x43c, 0x00020000 },
    { 0x440, 0x00003048 }, { 0x444, 0x000002e4 }, { 0x4d0, 0x00000040 },
    { 0x4d4, 0x00008002 }, { 0x4dc, 0x00315e12 }, { 0x4e4, 0x0008001f },
    { 0x4f0, 0x00000002 }, { 0x4f8, 0x00000001 }, { 0x500, 0x40010003 },
    { 0x50c, 0x00000003 }, { 0x554, 0x02800000 }, { 0x558, 0x0000000f },
    { 0x560, 0x0000000f }, { 0x800, 0x1c600300 }, { 0x804, 0x0ffff7ff },
    { 0x808, 0x00c00062 }, { 0x80c, 0x000800c0 }, { 0x814, 0x02000000 },
    { 0x860, 0x00451451 }, { 0x870, 0x00000a0a }, { 0x878, 0x01000202 },
    { 0x87c, 0x00000004 }, { 0x904, 0x00008000 }, { 0x93c, 0x00004000 },
    { 0x944, 0x00008000 }, { 0x97c, 0x00004000 }, { 0x980, 0xffff0201 },
    { 0x984, 0xffff0fff }, { 0x988, 0x0000c1e8 }, { 0x990, 0x00108008 },
    { 0x994, 0xa448003e },
};

static BCMNSPCIeHost *rc_host(PCIDevice *d)
{
    return BCM_NS_PCIE_HOST(OBJECT(d)->parent);
}

static PCIDevice *rc_link_partner(BCMNSPCIeHost *s, uint8_t devfn)
{
    PCIBus *sec = pci_bridge_get_sec_bus(PCI_BRIDGE(&s->rc));

    if (!(s->reg[PCIE_CONTROL / 4] & CONTROL_PERST_OFF)) {
        return NULL;
    }
    return sec->devices[devfn];
}

static bool rc_link_up(BCMNSPCIeHost *s)
{
    return rc_link_partner(s, 0) != NULL;
}

/* LNKSTA: Gen1 or Gen2 (LNKCTL2's target, the endpoint can do both) x1
 * with slot clock, as a unit reads (0x1012); down: no width. Slot status
 * presence follows the device. */
static void rc_sync_link(BCMNSPCIeHost *s)
{
    uint8_t *c = PCI_DEVICE(&s->rc)->config;
    bool up = rc_link_up(s);
    uint16_t speed = MIN(MAX(pci_get_word(c + RP_LNKCTL2) & 0xf, 1), 2);

    pci_set_word(c + RP_LNKSTA, PCI_EXP_LNKSTA_SLC |
                 (up ? (1 << 4) | speed : 1));
    pci_set_word(c + RP_SLTSTA, up ? PCI_EXP_SLTSTA_PDS : 0);
}

static uint32_t rc_config_read(PCIDevice *d, uint32_t addr, int len)
{
    uint32_t v = 0;

    /* Not pci_default_read_config: QEMU's own link sync reports a link
     * even with nothing attached. */
    rc_sync_link(rc_host(d));
    memcpy(&v, d->config + addr, len);
    return le32_to_cpu(v);
}

static void rc_config_write(PCIDevice *d, uint32_t addr, uint32_t val, int len)
{
    uint8_t *c = d->config;

    pci_bridge_write_config(d, addr, val, len);
    /* Retrain Link reads as 0; training is instant. */
    pci_word_test_and_clear_mask(c + RP_EXP_CAP + PCI_EXP_LNKCTL, PCI_EXP_LNKCTL_RL);
    if (ranges_overlap(addr, len, RP_MPS_SHADOW, 1)) {
        c[RP_EXP_CAP + PCI_EXP_DEVCAP] = (c[RP_EXP_CAP + PCI_EXP_DEVCAP] & ~7) |
                                         (c[RP_MPS_SHADOW] & 7);
    }
}

static void rc_load_cfg(PCIDevice *d)
{
    for (size_t i = 0; i < ARRAY_SIZE(rp_cfg); i++) {
        pci_set_long(d->config + rp_cfg[i].off, rp_cfg[i].val);
    }
}

static void rc_realize(PCIDevice *d, Error **errp)
{
    uint8_t *w = d->wmask, *w1c = d->w1cmask;

    pci_bridge_initfn(d, TYPE_PCIE_BUS);
    d->exp.exp_cap = RP_EXP_CAP;
    d->config[PCI_INTERRUPT_PIN] = 1;
    pci_set_word(d->config + PCI_STATUS, PCI_STATUS_CAP_LIST);
    d->config[PCI_CAPABILITY_LIST] = 0x48;
    rc_load_cfg(d);

    /* Above the header only what is listed below is writable. */
    memset(w + PCI_CONFIG_HEADER_SIZE, 0, PCIE_CONFIG_SPACE_SIZE - PCI_CONFIG_HEADER_SIZE);
    /* NS has no I/O space: base/limit read 0 (the unit's 0x1c is 0). */
    memset(w + PCI_IO_BASE, 0, 2);
    memset(w + PCI_IO_BASE_UPPER16, 0, 4);

    /* PM: power state, PME enable; PME status write-1-to-clear. */
    pci_set_word(w + 0x4c, 0x0103);
    pci_set_word(w1c + 0x4c, 0x8000);
    /* PCIe: DEVCTL, LNKCTL, SLTCTL, RTCTL, DEVCTL2, LNKCTL2. */
    pci_set_word(w + RP_EXP_CAP + PCI_EXP_DEVCTL, 0xffff);
    pci_set_word(w1c + RP_EXP_CAP + PCI_EXP_DEVSTA, 0x000f);
    pci_set_word(w + RP_EXP_CAP + PCI_EXP_LNKCTL, 0x0ffb | PCI_EXP_LNKCTL_RL);
    pci_set_word(w + RP_EXP_CAP + PCI_EXP_SLTCTL, 0x1fff);
    pci_set_word(w + RP_EXP_CAP + PCI_EXP_RTCTL, 0x001f);
    pci_set_word(w + RP_EXP_CAP + PCI_EXP_DEVCTL2, 0xffff);
    pci_set_word(w + RP_LNKCTL2, 0xffff);
    /* AER masks, severity, control; status write-1-to-clear. */
    pci_set_long(w + 0x108, 0xffffffff);
    pci_set_long(w + 0x10c, 0xffffffff);
    pci_set_long(w + 0x114, 0xffffffff);
    pci_set_long(w + 0x118, 0x000001c0);
    pci_set_long(w1c + 0x104, 0xffffffff);
    pci_set_long(w1c + 0x110, 0xffffffff);
    /* Vendor registers: storage. */
    memset(w + 0x400, 0xff, 0x1000 - 0x400);
}

static void rc_reset(DeviceState *dev)
{
    PCIDevice *d = PCI_DEVICE(dev);

    pci_bridge_reset(dev);
    rc_load_cfg(d);
}

/* ---- the controller ------------------------------------------------------ */

static void pcie_update_irq(BCMNSPCIeHost *s)
{
    s->reg[PCIE_RC_INTX_CSR / 4] = s->intx;
    qemu_set_irq(s->irq, !!(s->intx & s->reg[PCIE_RC_INTX_EN / 4] & 0xf));
}

static void pcie_set_irq(void *opaque, int pin, int level)
{
    BCMNSPCIeHost *s = opaque;

    s->intx = deposit32(s->intx, pin, 1, !!level);
    pcie_update_irq(s);
}

static uint64_t pcie_reg64(BCMNSPCIeHost *s, hwaddr off)
{
    return deposit64(s->reg[off / 4], 32, 32, s->reg[off / 4 + 1]);
}

/* Outbound: OARR n (64 MiB at CPU address OARR & ~1) -> PCI address OMAP n. */
static void pcie_map_outbound(BCMNSPCIeHost *s)
{
    for (int i = 0; i < BCM_NS_PCIE_NOWIN; i++) {
        MemoryRegion *a = &s->owin_alias[i];
        uint64_t oarr = pcie_reg64(s, PCIE_OARR + 8 * i);
        uint64_t omap = pcie_reg64(s, PCIE_OMAP + 8 * i) & ~(uint64_t)(OWIN_SLICE - 1);
        uint64_t cpu = oarr & ~(uint64_t)(OWIN_SLICE - 1);
        bool on = (oarr & 1) && cpu >= s->owin_base &&
                  cpu + OWIN_SLICE <= s->owin_base + BCM_NS_PCIE_OWIN_SIZE &&
                  omap + OWIN_SLICE <= 4 * GiB;

        memory_region_transaction_begin();
        memory_region_set_enabled(a, on);
        if (on) {
            memory_region_set_alias_offset(a, omap);
            memory_region_set_address(a, cpu - s->owin_base);
        } else if (oarr & 1) {
            qemu_log_mask(LOG_UNIMP, "bcm-ns-pcie%u: OARR%d 0x%" PRIx64
                          " is outside the port's window\n", s->port, i, oarr);
        }
        memory_region_transaction_commit();
    }
}

/*
 * Inbound: PCI addresses in IARR n go to the SoC at IMAP n. IARR1's size
 * field is in MiB; IARR2's, on this core revision, in GiB (the kernel's
 * "1GB for NS-B0 IARR_2"). Function 0's IMAP1/IMAP2 only: the kernel uses
 * nothing else, and IMAP0's 4 KiB pages into the CCB are not modelled.
 */
static void pcie_map_inbound(BCMNSPCIeHost *s)
{
    static const hwaddr imap[BCM_NS_PCIE_NIWIN] = { PCIE_IMAP1, PCIE_IMAP2 };
    static const uint64_t unit[BCM_NS_PCIE_NIWIN] = { MiB, GiB };

    for (int i = 0; i < BCM_NS_PCIE_NIWIN; i++) {
        MemoryRegion *a = &s->dma_alias[i];
        uint64_t iarr = pcie_reg64(s, PCIE_IARR + 8 * (i + 1));
        uint64_t map = pcie_reg64(s, imap[i]);
        uint64_t size = (iarr & 0xff) * unit[i];
        uint64_t base = iarr & ~(uint64_t)0xfffff;
        uint64_t to = map & ~(uint64_t)0xfff;
        bool on = (map & 1) && size && base + size <= 4 * GiB &&
                  to + size <= 4 * GiB;

        memory_region_transaction_begin();
        memory_region_set_enabled(a, on);
        if (on) {
            memory_region_set_size(a, size);
            memory_region_set_alias_offset(a, to);
            memory_region_set_address(a, base);
        }
        memory_region_transaction_commit();
    }
}

static PCIDevice *pcie_cfg_target(BCMNSPCIeHost *s, uint32_t a)
{
    PCIHostState *h = PCI_HOST_BRIDGE(s);
    uint8_t devfn = (a >> 12) & 0xff;
    int bus = (a >> 20) & 0xff;

    if (!rc_link_up(s)) {
        return NULL;
    }
    if ((a & 3) == 0) {
        return rc_link_partner(s, devfn);
    }
    return bus ? pci_find_device(h->bus, bus, devfn) : NULL;
}

static MemTxResult pcie_read(void *opaque, hwaddr off, uint64_t *data,
                             unsigned size, MemTxAttrs attrs)
{
    BCMNSPCIeHost *s = opaque;
    PCIDevice *rc = PCI_DEVICE(&s->rc);
    PCIDevice *d;

    if (pcie_reg_hole(off)) {
        return MEMTX_DECODE_ERROR;
    }
    switch (off) {
    case PCIE_EXT_CFG_DATA:
        *data = pci_host_config_read_common(rc, s->reg[PCIE_EXT_CFG_ADDR / 4] & 0x1ffc,
                                            PCIE_CONFIG_SPACE_SIZE, 4);
        return MEMTX_OK;
    case PCIE_CFG_DATA:
        d = pcie_cfg_target(s, s->reg[PCIE_CFG_ADDR / 4]);
        *data = d ? pci_host_config_read_common(d, s->reg[PCIE_CFG_ADDR / 4] & 0xffc,
                                                pci_config_size(d), 4)
                  : 0xffffffff;
        return MEMTX_OK;
    }
    if (off >= PCIE_HDR_OFF && off < PCIE_HDR_OFF + PCI_CONFIG_SPACE_SIZE) {
        *data = pci_host_config_read_common(rc, off - PCIE_HDR_OFF,
                                            PCI_CONFIG_SPACE_SIZE, 4);
        return MEMTX_OK;
    }
    *data = s->reg[off / 4];
    return MEMTX_OK;
}

static MemTxResult pcie_write(void *opaque, hwaddr off, uint64_t val,
                              unsigned size, MemTxAttrs attrs)
{
    BCMNSPCIeHost *s = opaque;
    PCIDevice *rc = PCI_DEVICE(&s->rc);
    PCIDevice *d;
    uint32_t old = s->reg[off / 4];

    if (pcie_reg_hole(off)) {
        return MEMTX_DECODE_ERROR;
    }
    switch (off) {
    case PCIE_EXT_CFG_DATA:
        pci_host_config_write_common(rc, s->reg[PCIE_EXT_CFG_ADDR / 4] & 0x1ffc,
                                     PCIE_CONFIG_SPACE_SIZE, val, 4);
        return MEMTX_OK;
    case PCIE_CFG_DATA:
        d = pcie_cfg_target(s, s->reg[PCIE_CFG_ADDR / 4]);
        if (d) {
            pci_host_config_write_common(d, s->reg[PCIE_CFG_ADDR / 4] & 0xffc,
                                         pci_config_size(d), val, 4);
        }
        return MEMTX_OK;
    case PCIE_RC_INTX_CSR:
        return MEMTX_OK;                /* status of the INTx wires */
    }
    if (off >= PCIE_HDR_OFF && off < PCIE_HDR_OFF + PCI_CONFIG_SPACE_SIZE) {
        pci_host_config_write_common(rc, off - PCIE_HDR_OFF,
                                     PCI_CONFIG_SPACE_SIZE, val, 4);
        return MEMTX_OK;
    }
    s->reg[off / 4] = val;

    switch (off) {
    case PCIE_CONTROL:
        /* PERST# released: the link partner comes out of reset. */
        if ((val & ~old) & CONTROL_PERST_OFF) {
            PCIDevice *ep = rc_link_partner(s, 0);

            if (ep) {
                device_cold_reset(DEVICE(ep));
            }
        }
        break;
    case PCIE_RC_INTX_EN:
        pcie_update_irq(s);
        break;
    default:
        if (off >= PCIE_IMAP1 && off < PCIE_OARR) {
            pcie_map_inbound(s);
        } else if (off >= PCIE_OARR && off < PCIE_OMAP + 8 * BCM_NS_PCIE_NOWIN) {
            pcie_map_outbound(s);
        }
        break;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps pcie_ops = {
    .read_with_attrs = pcie_read,
    .write_with_attrs = pcie_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* PCI memory nothing decodes: a master abort, which reads all ones. */
static uint64_t pcie_bg_read(void *opaque, hwaddr off, unsigned size)
{
    BCMNSPCIeHost *s = opaque;

    qemu_log_mask(LOG_GUEST_ERROR, "bcm-ns-pcie%u: read of PCI address 0x%"
                  HWADDR_PRIx " nothing decodes\n", s->port, off);
    return ~0ull;
}

static void pcie_bg_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    BCMNSPCIeHost *s = opaque;

    qemu_log_mask(LOG_GUEST_ERROR, "bcm-ns-pcie%u: write to PCI address 0x%"
                  HWADDR_PRIx " nothing decodes\n", s->port, off);
}

static const MemoryRegionOps pcie_bg_ops = {
    .read = pcie_bg_read,
    .write = pcie_bg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static AddressSpace *pcie_dma_as(PCIBus *bus, void *opaque, int devfn)
{
    return &((BCMNSPCIeHost *)opaque)->dma_as;
}

static const PCIIOMMUOps pcie_iommu_ops = {
    .get_address_space = pcie_dma_as,
};

static void pcie_reset_hold(Object *obj, ResetType type)
{
    BCMNSPCIeHost *s = BCM_NS_PCIE_HOST(obj);

    memset(s->reg, 0, sizeof(s->reg));
    for (size_t i = 0; i < ARRAY_SIZE(pcie_reg_reset); i++) {
        s->reg[pcie_reg_reset[i].off / 4] = pcie_reg_reset[i].val;
    }
    if (s->port == 1) {
        for (size_t i = 0; i < ARRAY_SIZE(pcie_reg_reset_port1); i++) {
            s->reg[pcie_reg_reset_port1[i].off / 4] = pcie_reg_reset_port1[i].val;
        }
    }
    /* IMAP0 window 2: this port's own core */
    s->reg[(PCIE_IMAP0 + 8) / 4] = (0x18012000 + (s->port - 1) * 0x1000) | 1;
    pcie_update_irq(s);
    pcie_map_inbound(s);
    pcie_map_outbound(s);
}

static void pcie_host_realize(DeviceState *dev, Error **errp)
{
    BCMNSPCIeHost *s = BCM_NS_PCIE_HOST(dev);
    PCIHostState *h = PCI_HOST_BRIDGE(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    g_autofree char *name = g_strdup_printf("pcie%u.root", s->port);
    g_autofree char *secname = g_strdup_printf("pcie%u", s->port);

    if (s->port < 1 || s->port > 3) {
        error_setg(errp, "bcm-ns-pcie: port must be 1..3");
        return;
    }
    memory_region_init_io(&s->regs, OBJECT(s), &pcie_ops, s, "bcm-ns-pcie.regs",
                          BCM_NS_PCIE_REGS_SIZE);
    sysbus_init_mmio(sbd, &s->regs);

    memory_region_init(&s->pci_mem, OBJECT(s), "bcm-ns-pcie.mem", 4 * GiB);
    memory_region_init_io(&s->pci_mem_bg, OBJECT(s), &pcie_bg_ops, s,
                          "bcm-ns-pcie.mem-abort", 4 * GiB);
    memory_region_add_subregion_overlap(&s->pci_mem, 0, &s->pci_mem_bg, -1);
    memory_region_init(&s->pci_io, OBJECT(s), "bcm-ns-pcie.io", 16);

    memory_region_init(&s->owin, OBJECT(s), "bcm-ns-pcie.owin", BCM_NS_PCIE_OWIN_SIZE);
    for (int i = 0; i < BCM_NS_PCIE_NOWIN; i++) {
        memory_region_init_alias(&s->owin_alias[i], OBJECT(s), "bcm-ns-pcie.oarr",
                                 &s->pci_mem, 0, OWIN_SLICE);
        memory_region_set_enabled(&s->owin_alias[i], false);
        memory_region_add_subregion(&s->owin, 0, &s->owin_alias[i]);
    }
    sysbus_init_mmio(sbd, &s->owin);

    memory_region_init(&s->dma_root, OBJECT(s), "bcm-ns-pcie.inbound", 4 * GiB);
    for (int i = 0; i < BCM_NS_PCIE_NIWIN; i++) {
        memory_region_init_alias(&s->dma_alias[i], OBJECT(s), "bcm-ns-pcie.iarr",
                                 get_system_memory(), 0, MiB);
        memory_region_set_enabled(&s->dma_alias[i], false);
        memory_region_add_subregion(&s->dma_root, 0, &s->dma_alias[i]);
    }
    address_space_init(&s->dma_as, &s->dma_root, "bcm-ns-pcie.dma");

    sysbus_init_irq(sbd, &s->irq);

    h->bus = pci_register_root_bus(dev, name, pcie_set_irq, pci_swizzle_map_irq_fn,
                                   s, &s->pci_mem, &s->pci_io, 0, 4, TYPE_PCIE_BUS);
    /* Set before the root port realizes: its secondary bus takes the flag
     * from here, and the root bus itself is realized only after us. Without
     * it the endpoints' config space reads as 256 bytes. */
    h->bus->flags |= PCI_BUS_EXTENDED_CONFIG_SPACE;
    pci_setup_iommu(h->bus, &pcie_iommu_ops, s);

    PCI_BRIDGE(&s->rc)->bus_name = g_strdup(secname);
    qdev_realize(DEVICE(&s->rc), BUS(h->bus), errp);
}

static void pcie_host_init(Object *obj)
{
    BCMNSPCIeHost *s = BCM_NS_PCIE_HOST(obj);

    object_initialize_child(obj, "rc", &s->rc, TYPE_BCM_NS_PCIE_RC);
    qdev_prop_set_int32(DEVICE(&s->rc), "addr", PCI_DEVFN(0, 0));
    qdev_prop_set_bit(DEVICE(&s->rc), "multifunction", false);
}

static const char *pcie_root_bus_path(PCIHostState *h, PCIBus *rootbus)
{
    BCMNSPCIeHost *s = BCM_NS_PCIE_HOST(h);
    static char path[3][8];

    snprintf(path[s->port - 1], sizeof(path[0]), "%04x:00", s->port);
    return path[s->port - 1];
}

static int pcie_post_load(void *opaque, int version_id)
{
    BCMNSPCIeHost *s = opaque;

    pcie_update_irq(s);
    pcie_map_inbound(s);
    pcie_map_outbound(s);
    return 0;
}

static const VMStateDescription pcie_host_vmstate = {
    .name = "bcm-ns-pcie-host",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = pcie_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, BCMNSPCIeHost, BCM_NS_PCIE_REGS_SIZE / 4),
        VMSTATE_UINT32(intx, BCMNSPCIeHost),
        VMSTATE_END_OF_LIST()
    },
};

static const Property pcie_host_props[] = {
    DEFINE_PROP_UINT32("port", BCMNSPCIeHost, port, 1),
    DEFINE_PROP_UINT64("owin-base", BCMNSPCIeHost, owin_base, 0),
};

static void pcie_host_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIHostBridgeClass *hc = PCI_HOST_BRIDGE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    hc->root_bus_path = pcie_root_bus_path;
    dc->realize = pcie_host_realize;
    dc->vmsd = &pcie_host_vmstate;
    dc->fw_name = "pci";
    dc->user_creatable = false;
    rc->phases.hold = pcie_reset_hold;
    device_class_set_props(dc, pcie_host_props);
}

static const VMStateDescription rc_vmstate = {
    .name = "bcm-ns-pcie-rc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, PCIBridge),
        VMSTATE_END_OF_LIST()
    },
};

static void rc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = rc_realize;
    k->exit = pci_bridge_exitfn;
    k->config_read = rc_config_read;
    k->config_write = rc_config_write;
    k->vendor_id = 0x14e4;
    k->device_id = 0xd612;
    k->revision = 0;
    k->class_id = PCI_CLASS_BRIDGE_PCI;
    dc->desc = "Broadcom Northstar PCIe root port";
    dc->vmsd = &rc_vmstate;
    dc->user_creatable = false;
    device_class_set_legacy_reset(dc, rc_reset);
}

static const TypeInfo bcm_ns_pcie_types[] = {
    {
        .name          = TYPE_BCM_NS_PCIE_HOST,
        .parent        = TYPE_PCI_HOST_BRIDGE,
        .instance_size = sizeof(BCMNSPCIeHost),
        .instance_init = pcie_host_init,
        .class_init    = pcie_host_class_init,
    }, {
        .name          = TYPE_BCM_NS_PCIE_RC,
        .parent        = TYPE_PCI_BRIDGE,
        .instance_size = sizeof(BCMNSPCIeRC),
        .class_init    = rc_class_init,
        .interfaces    = (const InterfaceInfo[]) {
            { INTERFACE_PCIE_DEVICE },
            { }
        },
    },
};

DEFINE_TYPES(bcm_ns_pcie_types)
