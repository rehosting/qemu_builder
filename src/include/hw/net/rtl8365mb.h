/*
 * Realtek RTL8365MB external switch on a bit-banged Realtek SMI bus.
 *
 * Register contract: from the vendor rtl8365mb.ko blob.
 */
#ifndef HW_NET_RTL8365MB_H
#define HW_NET_RTL8365MB_H

#include "hw/core/qdev.h"
#include "net/net.h"
#include "qom/object.h"

#define TYPE_RTL8365MB "rtl8365mb"
OBJECT_DECLARE_SIMPLE_TYPE(RTL8365MBState, RTL8365MB)

#define RTL8365MB_NUTP   4             /* UTP ports that can carry a QEMU netdev */
#define RTL8365MB_NPHY   5             /* PHYs the driver initialises */
#define RTL8365MB_EXT1   6             /* physical port of EXT1, the RGMII uplink */
#define RTL8365MB_LUT_SIZE 512         /* learned-address table (model size) */

typedef void RTL8365MBUplinkFn(void *opaque, const uint8_t *buf, size_t len);

typedef struct RTL8365MBPort {
    RTL8365MBState *sw;
    int port;
    NICConf conf;
    NICState *nic;
} RTL8365MBPort;

struct RTL8365MBState {
    DeviceState parent_obj;
    RTL8365MBPort ext[RTL8365MB_NUTP];
    qemu_irq sda_out;                  /* slave drive of MDIO, to the GPIO pad */
    RTL8365MBUplinkFn *uplink;         /* EXT1 (RGMII) toward the host switch */
    void *uplink_opaque;

    /* SMI bus */
    uint8_t sck, sda, in_reset;
    uint8_t phase, nbits, clocked;     /* clocked: the current ack slot saw SCK rise */
    uint8_t byte;                      /* host byte: 0 cmd, 1-2 addr, 3-4 write data */
    uint8_t hi;                        /* read data: sending the high byte */
    uint8_t cmd, shift, drive;
    uint16_t addr, data;

    uint16_t regs[0x10000];
    uint16_t ocp[RTL8365MB_NPHY][0x10000 / 2];

    /* learned addresses: mac -> physical port; soft state, rebuilt by traffic */
    struct {
        uint8_t mac[6];
        uint8_t port;
        uint8_t valid;
        int64_t seen;                  /* QEMU_CLOCK_VIRTUAL ns */
    } lut[RTL8365MB_LUT_SIZE];
    bool vlan_warned;
};

void rtl8365mb_connect_uplink(RTL8365MBState *s, RTL8365MBUplinkFn *fn, void *opaque);
void rtl8365mb_from_uplink(RTL8365MBState *s, const uint8_t *buf, size_t len);

#endif
