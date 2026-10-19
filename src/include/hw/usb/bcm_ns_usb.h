/*
 * Broadcom Northstar (BCM4708/4709/47094) USB host controllers.
 */
#ifndef HW_USB_BCM_NS_USB_H
#define HW_USB_BCM_NS_USB_H

#include "hw/core/irq.h"
#include "exec/hwaddr.h"

#define TYPE_BCM_NS_EHCI "bcm-ns-ehci"

/* USB 2.0 core: EHCI at @ehci_base with an OHCI companion at @ohci_base,
 * both on one interrupt line. */
void bcm_ns_usb2_create(hwaddr ehci_base, hwaddr ohci_base, qemu_irq irq);

/* USB 3.0 core: xHCI at @base, one SuperSpeed root port. */
void bcm_ns_usb3_create(hwaddr base, qemu_irq irq);

#endif
