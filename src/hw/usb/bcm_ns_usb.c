/*
 * Broadcom Northstar (BCM4708/4709/47094) USB host controllers, built from
 * QEMU's generic EHCI, OHCI and xHCI models.
 *
 *   - USB 2.0 core (0x504): EHCI at 0x18021000 with an OHCI companion at
 *     0x18022000, sharing one interrupt. Full- and low-speed devices are
 *     handed to the OHCI, as on the unit (EHCI HCSPARAMS N_CC 1, N_PCC 2).
 *   - USB 3.0 core (0x505): xHCI at 0x18023000 with one SuperSpeed port.
 *
 * The vendor kernel finds them through its fake PCI bus (00:0b.0 OHCI,
 * 00:0b.1 EHCI, 00:0c.0 xHCI) and binds the stock ehci/ohci/xhci-pci drivers.
 * Capability words are the ones a BCM47094 unit reads, except where QEMU's
 * controller behaves differently and the driver would act on the difference
 * (marked below).
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/or-irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/usb/hcd-ehci.h"
#include "hw/usb/hcd-ohci.h"
#include "hw/usb/hcd-xhci-sysbus.h"
#include "hw/usb/bcm_ns_usb.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"

/* ---- EHCI ----------------------------------------------------------------
 * A unit reads CAPLENGTH 0x10, HCIVERSION 0x0100, HCSPARAMS 0x1212 (2 ports,
 * port power control, one companion with 2 ports) and HCCPARAMS 0xa010
 * (EECP 0xa0, isochronous threshold 1). QEMU's EHCI ignores PPC (port power
 * always on, as the unit's PORTSC reads) and the threshold.
 *
 * Synopsys INSNREG00-04 at 0x90-0xa0: the vendor kernel sets the packet
 * buffer OUT threshold (INSNREG01 bits 31:16 = 0x80) and "break memory
 * transfer" (INSNREG03 bit 0) before the driver binds. They are stored only;
 * the reset values are the unit's readings without those writes [assume].
 */
#define NS_EHCI_INSNREG     0x90
#define NS_EHCI_NINSNREG    5

OBJECT_DECLARE_SIMPLE_TYPE(BCMNSEHCIState, BCM_NS_EHCI)

struct BCMNSEHCIState {
    EHCISysBusState parent_obj;
    MemoryRegion insn;
    uint32_t insnreg[NS_EHCI_NINSNREG];
};

static const uint32_t ns_ehci_insnreg_reset[NS_EHCI_NINSNREG] = {
    0x00000000, 0x00000040, 0x00000100, 0x00000000, 0x00000000,
};

static DeviceRealize ns_ehci_parent_realize;
static ResettablePhases ns_ehci_parent_phases;

static uint64_t ns_ehci_insn_read(void *opaque, hwaddr addr, unsigned size)
{
    return BCM_NS_EHCI(opaque)->insnreg[addr >> 2];
}

static void ns_ehci_insn_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    BCM_NS_EHCI(opaque)->insnreg[addr >> 2] = val;
}

static const MemoryRegionOps ns_ehci_insn_ops = {
    .read = ns_ehci_insn_read,
    .write = ns_ehci_insn_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void ns_ehci_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    BCMNSEHCIState *s = BCM_NS_EHCI(dev);
    EHCIState *e = &s->parent_obj.ehci;

    ns_ehci_parent_realize(dev, errp);
    if (*errp) {
        return;
    }
    e->caps[0x04] = 0x10 | e->portnr;   /* PPC, N_PORTS */
    e->caps[0x08] = 0x10;               /* IST 1 */
    e->caps[0x09] = 0xa0;               /* EECP */
    memory_region_init_io(&s->insn, OBJECT(dev), &ns_ehci_insn_ops, s,
                          "bcm-ns.ehci-insnreg", NS_EHCI_NINSNREG * 4);
    memory_region_add_subregion(&e->mem, NS_EHCI_INSNREG, &s->insn);
}

static void ns_ehci_reset_hold(Object *obj, ResetType type)
{
    BCMNSEHCIState *s = BCM_NS_EHCI(obj);

    if (ns_ehci_parent_phases.hold) {
        ns_ehci_parent_phases.hold(obj, type);
    }
    memcpy(s->insnreg, ns_ehci_insnreg_reset, sizeof(s->insnreg));
}

static const VMStateDescription vmstate_ns_ehci = {
    .name = TYPE_BCM_NS_EHCI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(parent_obj.ehci, BCMNSEHCIState, 2, vmstate_ehci, EHCIState),
        VMSTATE_UINT32_ARRAY(insnreg, BCMNSEHCIState, NS_EHCI_NINSNREG),
        VMSTATE_END_OF_LIST()
    },
};

static void ns_ehci_class_init(ObjectClass *oc, const void *data)
{
    SysBusEHCIClass *sec = SYS_BUS_EHCI_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    sec->capsbase = 0x0;
    sec->opregbase = 0x10;
    sec->portscbase = 0x44;
    sec->portnr = 2;
    device_class_set_parent_realize(dc, ns_ehci_realize, &ns_ehci_parent_realize);
    resettable_class_set_parent_phases(RESETTABLE_CLASS(oc), NULL,
                                       ns_ehci_reset_hold, NULL,
                                       &ns_ehci_parent_phases);
    dc->vmsd = &vmstate_ns_ehci;
}

static const TypeInfo ns_ehci_info = {
    .name          = TYPE_BCM_NS_EHCI,
    .parent        = TYPE_SYS_BUS_EHCI,
    .instance_size = sizeof(BCMNSEHCIState),
    .class_init    = ns_ehci_class_init,
};

static void ns_usb_register_types(void)
{
    type_register_static(&ns_ehci_info);
}
type_init(ns_usb_register_types)

void bcm_ns_usb2_create(hwaddr ehci_base, hwaddr ohci_base, qemu_irq irq)
{
    DeviceState *ehci = qdev_new(TYPE_BCM_NS_EHCI);
    DeviceState *ohci = qdev_new(TYPE_SYSBUS_OHCI);
    DeviceState *or = qdev_new(TYPE_OR_IRQ);

    qdev_prop_set_uint16(or, "num-lines", 2);
    qdev_realize_and_unref(or, NULL, &error_fatal);
    qdev_connect_gpio_out(or, 0, irq);

    qdev_prop_set_bit(ehci, "companion-enable", true);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ehci), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(ehci), 0, ehci_base);
    sysbus_connect_irq(SYS_BUS_DEVICE(ehci), 0, qdev_get_gpio_in(or, 0));

    /* The unit's OHCI: HcRhDescriptorA 0x02000202 (2 ports, no power
     * switching). QEMU's reads 0x00000202: POTPGT is not modelled. */
    qdev_prop_set_string(ohci, "masterbus",
                         BUS(&SYS_BUS_EHCI(ehci)->ehci.bus)->name);
    qdev_prop_set_uint32(ohci, "num-ports", 2);
    qdev_prop_set_uint32(ohci, "firstport", 0);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ohci), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(ohci), 0, ohci_base);
    sysbus_connect_irq(SYS_BUS_DEVICE(ohci), 0, qdev_get_gpio_in(or, 1));
}

/* ---- xHCI ----------------------------------------------------------------
 * The unit's xHCI decodes 4 KiB: capabilities at 0, operational registers at
 * CAPLENGTH 0x20 (one port register set at 0x420), runtime registers at
 * RTSOFF 0x4a0, doorbells for 31 slots at DBOFF 0x8c0, extended capabilities
 * at xECP 0x940. QEMU's xHCI spreads the same blocks over 16 KiB (runtime at
 * 0x1000, doorbells at 0x2000), which would cover the GMACs from 0x18024000,
 * so its blocks are mapped here at the unit's offsets, and the capability
 * words point at them.
 */
#define NS_XHCI_SIZE        0x1000
#define NS_XHCI_OPER        0x020
#define NS_XHCI_PORTS       0x420
#define NS_XHCI_RUNTIME     0x4a0
#define NS_XHCI_DOORBELL    0x8c0
#define NS_XHCI_XECP        0x940
#define NS_XHCI_SLOTS       31
#define NS_XHCI_INTRS       4
/* QEMU's own capability block: its USB 3.0 Supported Protocol capability. */
#define QEMU_XHCI_USB3_CAP  0x30

typedef struct BCMNSXhciMap {
    MemoryRegion mem, cap, oper, port, runtime, doorbell, xcap;
} BCMNSXhciMap;

static uint64_t ns_xhci_cap_read(void *opaque, hwaddr addr, unsigned size)
{
    XHCIState *x = opaque;

    switch (addr) {
    case 0x00:                          /* HCIVERSION 1.0, CAPLENGTH */
        return 0x01000000 | NS_XHCI_OPER;
    case 0x04:                          /* HCSPARAMS1: unit 0x0100041f */
        return ((x->numports_2 + x->numports_3) << 24) | (x->numintrs << 8) |
               x->numslots;
    case 0x08:                          /* HCSPARAMS2: the unit's */
        return 0x14000052;
    case 0x0c:                          /* HCSPARAMS3: the unit's U1/U2 latencies */
        return 0x07ff0002;
    case 0x10:
        /* HCCPARAMS: the unit's 0x02501164 (xECP, MaxPSASize 1, PAE, NSS,
         * LTC) without CSZ (bit 2): QEMU's xHCI uses 32-byte contexts, and
         * the driver would lay out 64-byte ones. 32-bit addressing, as on
         * the unit. */
        return ((NS_XHCI_XECP / 4) << 16) | 0x00001160;
    case 0x14:
        return NS_XHCI_DOORBELL;
    case 0x18:
        return NS_XHCI_RUNTIME;
    default:
        return 0;
    }
}

static void ns_xhci_cap_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
}

static const MemoryRegionOps ns_xhci_cap_ops = {
    .read = ns_xhci_cap_read,
    .write = ns_xhci_cap_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

void bcm_ns_usb3_create(hwaddr base, qemu_irq irq)
{
    DeviceState *dev = qdev_new(TYPE_XHCI_SYSBUS);
    BCMNSXhciMap *m = g_new0(BCMNSXhciMap, 1);
    Object *owner = OBJECT(dev);
    XHCIState *x;

    qdev_prop_set_uint32(dev, "p2", 0);
    qdev_prop_set_uint32(dev, "p3", 1);
    qdev_prop_set_uint32(dev, "intrs", NS_XHCI_INTRS);
    qdev_prop_set_uint32(dev, "slots", NS_XHCI_SLOTS);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, irq);
    x = &XHCI_SYSBUS(dev)->xhci;
    assert(x->numports == 1 && x->numintrs == NS_XHCI_INTRS &&
           x->numslots == NS_XHCI_SLOTS);

    memory_region_init(&m->mem, owner, "bcm-ns.xhci", NS_XHCI_SIZE);
    memory_region_init_io(&m->cap, owner, &ns_xhci_cap_ops, x,
                          "bcm-ns.xhci-cap", NS_XHCI_OPER);
    memory_region_add_subregion(&m->mem, 0, &m->cap);
    memory_region_init_alias(&m->oper, owner, "bcm-ns.xhci-oper",
                             &x->mem_oper, 0, 0x400);
    memory_region_add_subregion(&m->mem, NS_XHCI_OPER, &m->oper);
    memory_region_init_alias(&m->port, owner, "bcm-ns.xhci-port",
                             &x->ports[0].mem, 0, 0x10);
    memory_region_add_subregion(&m->mem, NS_XHCI_PORTS, &m->port);
    memory_region_init_alias(&m->runtime, owner, "bcm-ns.xhci-runtime",
                             &x->mem_runtime, 0, 0x20 * (1 + NS_XHCI_INTRS));
    memory_region_add_subregion(&m->mem, NS_XHCI_RUNTIME, &m->runtime);
    memory_region_init_alias(&m->doorbell, owner, "bcm-ns.xhci-doorbell",
                             &x->mem_doorbell, 0, 4 * (1 + NS_XHCI_SLOTS));
    memory_region_add_subregion(&m->mem, NS_XHCI_DOORBELL, &m->doorbell);
    memory_region_init_alias(&m->xcap, owner, "bcm-ns.xhci-xcap",
                             &x->mem_cap, QEMU_XHCI_USB3_CAP, 0x10);
    memory_region_add_subregion(&m->mem, NS_XHCI_XECP, &m->xcap);
    memory_region_add_subregion(get_system_memory(), base, &m->mem);
}
