/*
 * Broadcom Northstar Ethernet: GMAC cores (UniMAC + dma64) and the integrated
 * BCM53012 switch behind its SRAB register bridge.
 *
 * Register contracts: from the vendor et.ko / hnddma.c / bcmrobo.c of the
 * GPL release.
 */
#ifndef HW_NET_BCM_NS_ETH_H
#define HW_NET_BCM_NS_ETH_H

#include "hw/core/sysbus.h"
#include "net/net.h"
#include "qom/object.h"

#define TYPE_BCM_NS_GMAC "bcm-ns-gmac"
OBJECT_DECLARE_SIMPLE_TYPE(BCMNSGmacState, BCM_NS_GMAC)

#define TYPE_BCM53012 "bcm53012"
OBJECT_DECLARE_SIMPLE_TYPE(BCM53012State, BCM53012)

#define BCM_NS_GMAC_NTX    4
#define BCM_NS_GMAC_FRAME  0x4000      /* largest frame gathered from a TX chain */

struct BCMNSGmacState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    BCM53012State *sw;                 /* switch port this GMAC is wired to */
    int sw_port;

    uint32_t regs[0x1000 / 4];         /* plain RW store for everything not special */
    uint32_t tx_cd[BCM_NS_GMAC_NTX];   /* dma64 current descriptor (byte offset) */
    uint32_t rx_cd;
    uint32_t rx_dropped;
    uint8_t txbuf[BCM_NS_GMAC_FRAME];
};

void bcm_ns_gmac_receive(BCMNSGmacState *s, const uint8_t *buf, size_t len);

#define BCM53012_NPORTS    9           /* 0-4 GPHY, 5 RGMII, 7 GMAC1, 8 IMP (GMAC2) */
#define BCM53012_NEXT      5           /* ports 0-4 can carry a QEMU netdev */
#define BCM53012_ARL_SIZE  1024

/* A non-GMAC MAC on a switch port (port 5: the RTL8365MB's RGMII uplink) */
typedef void BCM53012MacFn(void *opaque, const uint8_t *buf, size_t len);

typedef struct BCM53012Port {
    BCM53012State *sw;
    int port;
    NICConf conf;
    NICState *nic;
} BCM53012Port;

struct BCM53012State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    BCM53012Port ext[BCM53012_NEXT];
    BCMNSGmacState *gmac[BCM53012_NPORTS];
    BCM53012MacFn *mac_fn[BCM53012_NPORTS];
    void *mac_opaque[BCM53012_NPORTS];

    /* SRAB */
    uint32_t cmdstat, wd_h, wd_l, rd_h, rd_l, ctrls;
    /* switch registers, one value per (page, reg) as the driver addresses them */
    uint64_t regs[256 * 256];
    uint32_t vtable[4096];             /* bits 8:0 members, 17:9 untagged */
    /* learned addresses: hash of (vid, mac) -> port; soft state, rebuilt by traffic */
    struct {
        uint8_t mac[6];
        uint16_t vid;                  /* 0 = empty */
        uint8_t port;
    } arl[BCM53012_ARL_SIZE];
};

void bcm53012_connect_gmac(BCM53012State *sw, int port, BCMNSGmacState *gmac);
void bcm53012_connect_mac(BCM53012State *sw, int port, BCM53012MacFn *fn, void *opaque);
void bcm53012_ingress(BCM53012State *sw, int port, const uint8_t *buf, size_t len);

#endif
