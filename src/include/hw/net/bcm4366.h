/*
 * Broadcom BCM4366 802.11ac radio as a PCIe full-dongle endpoint.
 *
 * Two halves. The chip (hw/net/bcm4366.c) is what the host driver can see
 * and program: config space, the BAR0 register windows onto the chip's
 * backplane, ChipCommon, the enumeration ROM, core wrappers, the PCIe2
 * core's mailboxes, the SYSMEM core and its RAM (BAR2), and the ARM CA7's
 * halt and reset. The firmware (hw/net/bcm4366_fw.c) stands in for the
 * dongle image the host downloads: when the host releases the ARM it plays
 * the image's part of the msgbuf protocol, answering through a wlsim
 * responder. The two meet only through the functions below, so another
 * firmware (the real image on a modelled CA7) could replace the second half.
 */
#ifndef HW_NET_BCM4366_H
#define HW_NET_BCM4366_H

#include "hw/pci/pci_device.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_BCM4366 "bcm4366"
OBJECT_DECLARE_SIMPLE_TYPE(BCM4366State, BCM4366)

#define BCM4366_TCM_SIZE        (4 * MiB)       /* BAR2: dongle addresses 0..4M */
#define BCM4366_RAM_BASE        0x200000        /* SYSMEM RAM (CA7_4365_RAM_BASE) */
#define BCM4366_RAM_SIZE        (2 * MiB)
#define BCM4366_NCORES          6
#define BCM4366_NWRAP           16

/* Firmware stand-in limits */
#define BCM4366_FW_MAXFLOW      40              /* TX flow rings */
#define BCM4366_FW_NH2D         (2 + BCM4366_FW_MAXFLOW)
#define BCM4366_FW_ND2H         3
#define BCM4366_FW_NIOCBUF      16
#define BCM4366_FW_NEVBUF       64
#define BCM4366_FW_NRXBUF       1024
#define BCM4366_FW_NPEND        256             /* host-bound frames waiting for buffers */

typedef struct BCM4366HostBuf {
    uint32_t pktid;
    uint32_t len;
    uint64_t addr;
} BCM4366HostBuf;

typedef struct BCM4366FlowRing {
    uint64_t base;
    uint16_t max_items;
    uint16_t len_item;
    uint8_t ifidx;
    uint8_t active;
} BCM4366FlowRing;

typedef struct BCM4366Pending {                 /* a frame for the host */
    uint8_t ifidx;
    uint8_t event;
    uint16_t len;
    uint8_t *data;                              /* not migrated */
} BCM4366Pending;

/* The firmware stand-in's state; vmstate in bcm4366_fw.c. */
typedef struct BCM4366Fw {
    uint32_t state;                             /* 0 off, 1 booting, 2 up */
    uint32_t shared;                            /* pcie_shared address */
    uint32_t ring_info, ring_mem, h2d_w, h2d_r, d2h_w, d2h_r;
    uint32_t h2d_mb, d2h_mb, cons, cons_buf, cons_size, cons_idx;
    uint16_t h2d_rd[BCM4366_FW_NH2D];
    uint16_t d2h_wr[BCM4366_FW_ND2H];
    uint32_t d2h_seq[BCM4366_FW_ND2H];
    BCM4366FlowRing flow[BCM4366_FW_NH2D];
    BCM4366HostBuf iocbuf[BCM4366_FW_NIOCBUF];
    uint32_t niocbuf;
    BCM4366HostBuf evbuf[BCM4366_FW_NEVBUF];
    uint32_t nevbuf;
    BCM4366HostBuf rxbuf[BCM4366_FW_NRXBUF];
    uint32_t rxbuf_head, nrxbuf;
    uint8_t mac[6];
    /* not migrated */
    BCM4366Pending pend[BCM4366_FW_NPEND];
    uint32_t pend_head, npend;
    QEMUTimer *boot_timer, *poll_timer;
    QEMUBH *bh;
    int wl_fd;
    int64_t wl_retry;
    bool wl_said;
    uint32_t nradio;                            /* from the responder's HELLO */
} BCM4366Fw;

struct BCM4366State {
    PCIDevice parent_obj;

    MemoryRegion bar0;
    MemoryRegion bar0_win[4];                   /* cfg 0x80, 0x70, 0x74, 0x78 */
    MemoryRegion bar0_pcie, bar0_cc;            /* fixed: +8K PCIe2 core, +12K CC */
    MemoryRegion bar0_bg;
    MemoryRegion bar2;
    MemoryRegion bp;                            /* the chip's backplane */
    MemoryRegion bp_bg;
    MemoryRegion tcm;
    MemoryRegion core[BCM4366_NCORES];
    MemoryRegion wrap;
    MemoryRegion erom;

    uint32_t creg[BCM4366_NCORES][1024];        /* core register files */
    uint32_t ioctrl[BCM4366_NWRAP], resetctrl[BCM4366_NWRAP];
    uint32_t pcie_cfg[1024];                    /* PCIe2 configaddr/configdata */
    bool arm_halted;                            /* the host stopped the CA7 */

    BCM4366Fw fw;

    /* properties */
    uint32_t radio;                             /* wlsim radio: 0 2.4 GHz, 1 5 GHz */
    uint32_t devid;                             /* the radio's 802.11 device id */
    char *wl;                                   /* wlsim socket */
    bool debug;                                 /* log the firmware's work */
};

#define BCM4366_LOG(s, fmt, ...) do {                                       \
        if ((s)->debug) {                                                   \
            qemu_log("bcm4366[%u]: " fmt "\n", (s)->radio, ## __VA_ARGS__);  \
        }                                                                   \
    } while (0)

/* chip, for the firmware */
void bcm4366_tcm_read(BCM4366State *s, uint32_t addr, void *buf, size_t len);
void bcm4366_tcm_write(BCM4366State *s, uint32_t addr, const void *buf, size_t len);
bool bcm4366_dma_read(BCM4366State *s, uint64_t addr, void *buf, size_t len);
bool bcm4366_dma_write(BCM4366State *s, uint64_t addr, const void *buf, size_t len);
void bcm4366_d2h_doorbell(BCM4366State *s);
void bcm4366_d2h_mailbox(BCM4366State *s);

/* firmware, for the chip */
void bcm4366_fw_realize(BCM4366State *s);
void bcm4366_fw_start(BCM4366State *s);         /* the host released the ARM */
void bcm4366_fw_stop(BCM4366State *s);          /* halted, reset or powered off */
void bcm4366_fw_doorbell(BCM4366State *s);      /* H2D doorbell 0 */
void bcm4366_fw_h2d_mailbox(BCM4366State *s);   /* H2D mailbox data */
extern const VMStateDescription bcm4366_fw_vmstate;

#endif
