/*
 * Broadcom Northstar (BCM4708/4709/47094) SoC backplane devices.
 *
 * Register contracts follow the Broadcom HND sources shipped in the vendor
 * GPL release (shared/siutils.c, aiutils.c, include/sbchipc.h,
 * include/aidmp.h, arch/arm/plat-brcm). See bcm_ns.c for per-register notes.
 */
#ifndef HW_MISC_BCM_NS_H
#define HW_MISC_BCM_NS_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

/* ---- ChipCommon-A ------------------------------------------------------ */
#define TYPE_BCM_NS_CHIPCOMMON "bcm-ns-chipcommon"
OBJECT_DECLARE_SIMPLE_TYPE(BCMNSChipCommonState, BCM_NS_CHIPCOMMON)

#define BCM_NS_CC_NGPIO 24

struct BCMNSChipCommonState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    QEMUTimer *wdt_timer;
    qemu_irq irq;                      /* -> GIC SPI 85 (IRQ_CCA) */
    qemu_irq gpio_out[BCM_NS_CC_NGPIO];

    /* properties */
    uint32_t chipid;
    uint32_t capabilities;
    uint32_t capabilities_ext;
    uint32_t chipstatus;
    uint32_t eromptr;
    uint32_t gpio_pullup_in;           /* level seen on undriven input pins */
    uint32_t wdt_hz;                   /* watchdog count rate (ALP clock) */

    /* state */
    uint32_t corecontrol;
    uint32_t intstatus;
    uint32_t intmask;
    uint32_t chipcontrol;
    uint32_t gpiopullup, gpiopulldown;
    uint32_t gpioout, gpioouten, gpiocontrol;
    uint32_t gpiointpolarity, gpiointmask;
    uint32_t gpioevent, gpioeventintmask, gpioeventintpolarity;
    uint32_t gpiotimerval, gpiotimeroutmask;
    uint32_t gpio_ext_level;           /* externally driven input levels */
    uint32_t gpio_ext_driven;          /* which inputs are externally driven */
    uint32_t watchdog;
    uint32_t clkdiv;
    uint32_t uart_irq_level;           /* bit n = UART n interrupt line */
};

/* ---- IDM / DMP wrappers (one 4 KiB page per core) ----------------------- */
#define TYPE_BCM_NS_IDM "bcm-ns-idm"
OBJECT_DECLARE_SIMPLE_TYPE(BCMNSIDMState, BCM_NS_IDM)

#define BCM_NS_IDM_BASE   0x18100000
#define BCM_NS_IDM_PAGES  0x30          /* 0x18100000 - 0x1812ffff */

struct BCMNSIDMState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t oobselouta30[BCM_NS_IDM_PAGES];
    uint32_t ioctrl[BCM_NS_IDM_PAGES];
    uint32_t iostatus[BCM_NS_IDM_PAGES];   /* set by the board (straps) */
    uint32_t resetctrl[BCM_NS_IDM_PAGES];
};

void bcm_ns_idm_set_iostatus(BCMNSIDMState *s, hwaddr wrapper_pa, uint32_t val);
void bcm_ns_idm_set_oobsel(BCMNSIDMState *s, hwaddr wrapper_pa, uint32_t val);

/* ---- Plain register file with reset values (DMU/CRU, iHost CRU) --------- */
#define TYPE_BCM_NS_REGFILE "bcm-ns-regfile"
OBJECT_DECLARE_SIMPLE_TYPE(BCMNSRegFileState, BCM_NS_REGFILE)

#define BCM_NS_REGFILE_WORDS 1024       /* 4 KiB */

typedef struct BCMNSRegInit {
    uint32_t offset;
    uint32_t value;
    uint32_t ro_mask;                   /* bits that ignore writes */
} BCMNSRegInit;

struct BCMNSRegFileState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    char *name;
    const BCMNSRegInit *init;           /* terminated by offset == UINT32_MAX */
    uint32_t regs[BCM_NS_REGFILE_WORDS];
    uint32_t ro[BCM_NS_REGFILE_WORDS];
};

BCMNSRegFileState *bcm_ns_regfile_create(const char *name, hwaddr base,
                                         const BCMNSRegInit *init);

/* ---- NAND controller (hw/block/bcm_ns_nand.c) ---------------------------- */
/*
 * Copy len bytes of the flash's data area (OOB skipped), starting at data
 * offset off, from the device's current contents (guest writes included).
 * False if the range is outside the flash. For the board's CFE-style boot.
 */
bool bcm_ns_nand_read(DeviceState *dev, uint64_t off, void *buf, size_t len);

#endif
