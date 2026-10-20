/*
 * Broadcom Northstar (BCM4708/4709/47094) PCIe Gen2 root complex ("PAXB").
 *
 * One controller per port: a 4 KiB register block on the SoC bus, a 128 MiB
 * outbound window into PCI memory space and one INTx interrupt. Its root
 * port is bus 0 device 0 of the port's own PCI domain; whatever sits on the
 * link is on the root port's secondary bus.
 */
#ifndef HW_PCI_HOST_BCM_NS_PCIE_H
#define HW_PCI_HOST_BCM_NS_PCIE_H

#include "hw/pci/pci_bridge.h"
#include "hw/pci/pci_host.h"
#include "qom/object.h"

#define TYPE_BCM_NS_PCIE_HOST "bcm-ns-pcie-host"
OBJECT_DECLARE_SIMPLE_TYPE(BCMNSPCIeHost, BCM_NS_PCIE_HOST)

#define TYPE_BCM_NS_PCIE_RC "bcm-ns-pcie-rc"
OBJECT_DECLARE_SIMPLE_TYPE(BCMNSPCIeRC, BCM_NS_PCIE_RC)

#define BCM_NS_PCIE_REGS_SIZE   0x1000
#define BCM_NS_PCIE_OWIN_SIZE   (128 * MiB)
#define BCM_NS_PCIE_NOWIN       3       /* OARR/OMAP pairs, 64 MiB each */
#define BCM_NS_PCIE_NIWIN       2       /* IARR1/IMAP1, IARR2/IMAP2 */

struct BCMNSPCIeRC {
    PCIBridge parent_obj;
};

struct BCMNSPCIeHost {
    PCIHostState parent_obj;

    BCMNSPCIeRC rc;
    MemoryRegion regs;
    MemoryRegion owin;                  /* CPU side, mapped by the board */
    MemoryRegion owin_alias[BCM_NS_PCIE_NOWIN];
    MemoryRegion pci_mem;               /* PCI memory space */
    MemoryRegion pci_mem_bg;            /* nothing decoded: master abort */
    MemoryRegion pci_io;                /* NS has no I/O space */
    MemoryRegion dma_root;              /* inbound: PCI address -> SoC */
    MemoryRegion dma_alias[BCM_NS_PCIE_NIWIN];
    AddressSpace dma_as;
    qemu_irq irq;

    uint32_t reg[BCM_NS_PCIE_REGS_SIZE / 4];
    uint32_t intx;                      /* INTA..INTD levels from the link */

    /* properties */
    uint32_t port;                      /* 1-based, as the kernel's PCIEn */
    uint64_t owin_base;                 /* CPU address the board maps owin at */
};

#endif
