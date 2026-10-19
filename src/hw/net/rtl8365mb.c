/*
 * Realtek RTL8365MB external switch, as the modelled board wires it:
 * - management through Realtek SMI bit-banged on ChipCommon GPIO 6 (SCK) and 7 (SDA),
 *   reset on GPIO 10 (active low);
 * - UTP ports 0-3 are LAN5-LAN8, EXT1 (RGMII) is the uplink into BCM53012 port 5.
 *
 * The bus side is edge driven: the driver paces SMI purely with MMIO round trips,
 * so the slave latches SDA on SCK rising and changes its own SDA only on SCK falling.
 * Frame formats and register behaviour: ETH-CONTRACT.md section 6.
 *
 * Forwarding is what the chip does with its tables at their reset values, because
 * the vendor driver never programs them: its ioctls and init (rtk_switch_init,
 * initialize_switch) call no VLAN, port-isolation or lookup-table setter, and rc on
 * this board has no path that does (its RTL VLAN commands are built for other models;
 * a VLAN for this board's guest networks is tagged on the host switch's port 5 and
 * crosses this one unchanged). So:
 * - source addresses are learned (one table, VLAN-unaware) and age out after 300 s;
 *   a known unicast goes to its port, anything else floods;
 * - each port may only send to the ports in its isolation mask (0x08a2 + port, 11
 *   ports), which resets to all ports;
 * - frames pass with their tags unchanged. Turning on VLAN filtering (0x07a8 bit 0)
 *   is logged as unimplemented; forwarding stays VLAN-unaware.
 * A write of 0x0a39 bit 0 flushes the learned table.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/net/rtl8365mb.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"

#define SMI_CMD_WRITE       0xb8
#define SMI_CMD_READ        0xb9

enum {
    SMI_IDLE,           /* no frame, or a frame we don't answer: wait for START */
    SMI_HOST_BYTE,      /* host shifts a byte in (cmd, addr lo/hi, data lo/hi) */
    SMI_SLAVE_ACK,      /* we hold SDA low for one clock */
    SMI_SLAVE_BYTE,     /* we shift a read-data byte out */
    SMI_HOST_ACK,       /* host drives its ack/nack bit after a data byte */
};

#define REG_CHIP_ID         0x1300
#define REG_EXT1_FORCE      0x1311  /* DIGITAL_INTERFACE1_FORCE */
#define REG_MIB_CTRL        0x1005
#define REG_IND_CTRL        0x1f00
#define REG_IND_STATUS      0x1f01
#define REG_IND_ADDR        0x1f02
#define REG_IND_WDATA       0x1f03
#define REG_IND_RDATA       0x1f04
#define REG_GPHY_OCP_MSB    0x1d15
#define REG_VLAN_CTRL       0x07a8  /* bit 0: VLAN filtering */
#define REG_PORT_ISO(p)     (0x08a2 + (p))  /* egress ports permitted from port p */
#define REG_LUT_FLUSH       0x0a39  /* bit 0: flush every learned address */

#define NPHYS               11      /* physical ports, as the isolation masks count */
#define ISO_ALL             0x07ff
#define LUT_AGE_NS          (300 * NANOSECONDS_PER_SECOND)

#define CHIP_ID_RTL8367C    0x6367
#define EXT_FORCE_LINK      0x1010  /* force + link */

#define OCP_MII(r)          (0xa400 + 2 * (r))
#define OCP_BMCR            OCP_MII(0)
#define OCP_BMSR            OCP_MII(1)
#define OCP_PHYSR           OCP_MII(26)
#define BMCR_RESET          0x8000
#define BMCR_PDOWN          0x0800

static bool utp_link(RTL8365MBState *s, int p)
{
    NICState *nic;

    if (s->in_reset || p >= RTL8365MB_NUTP) {
        return false;
    }
    nic = s->ext[p].nic;
    return nic && !qemu_get_queue(nic)->link_down &&
           !(s->ocp[p][OCP_BMCR >> 1] & BMCR_PDOWN);
}

static bool ext_link(RTL8365MBState *s)
{
    return !s->in_reset && (s->regs[REG_EXT1_FORCE] & EXT_FORCE_LINK) == EXT_FORCE_LINK;
}

static uint16_t ocp_read(RTL8365MBState *s, int phy, uint16_t ocp)
{
    bool link;

    if (phy >= RTL8365MB_NPHY) {
        qemu_log_mask(LOG_GUEST_ERROR, "rtl8365mb: OCP read of phy %d\n", phy);
        return 0xffff;
    }
    link = utp_link(s, phy);
    switch (ocp) {
    case OCP_BMSR:                      /* 1000FD capable, AN complete + link */
        return 0x7949 | (link ? 0x0024 : 0);
    case OCP_PHYSR:                     /* link b2, full duplex b3, speed [5:4] 2 = 1000 */
        return link ? 0x002c : 0;
    }
    return s->ocp[phy][ocp >> 1];
}

static void ocp_write(RTL8365MBState *s, int phy, uint16_t ocp, uint16_t v)
{
    if (phy >= RTL8365MB_NPHY) {
        qemu_log_mask(LOG_GUEST_ERROR, "rtl8365mb: OCP write of phy %d\n", phy);
        return;
    }
    if (ocp == OCP_BMCR) {
        v &= ~BMCR_RESET;
    }
    s->ocp[phy][ocp >> 1] = v;
}

/* Indirect PHY/OCP access: ETH-CONTRACT 6.4, checked against
 * rtl8367c_{set,get}AsicPHYOCPReg in the blob. */
static void ind_access(RTL8365MBState *s, uint16_t ctrl)
{
    uint16_t a = s->regs[REG_IND_ADDR];
    int phy = (a >> 5) & 0x7;
    uint16_t ocp = (((s->regs[REG_GPHY_OCP_MSB] >> 6) & 0x3f) << 10) |
                   (((a >> 8) & 0xf) << 6) | ((a & 0x1f) << 1);

    if (ctrl & 2) {
        ocp_write(s, phy, ocp, s->regs[REG_IND_WDATA]);
    } else {
        s->regs[REG_IND_RDATA] = ocp_read(s, phy, ocp);
    }
}

static void lut_flush(RTL8365MBState *s)
{
    memset(s->lut, 0, sizeof(s->lut));
}

static uint16_t reg_read(RTL8365MBState *s, uint16_t addr)
{
    switch (addr) {
    case REG_CHIP_ID:
        return CHIP_ID_RTL8367C;
    case REG_IND_STATUS:
        return 0;                       /* never busy */
    case REG_MIB_CTRL:
        return s->regs[addr] & ~1;      /* MIB counter read done at once */
    }
    return s->regs[addr];
}

static void reg_write(RTL8365MBState *s, uint16_t addr, uint16_t v)
{
    switch (addr) {
    case REG_CHIP_ID:
        return;
    case REG_IND_CTRL:
        if (v & 1) {
            ind_access(s, v);
        }
        v &= ~1;
        break;
    case REG_LUT_FLUSH:
        if (v & 1) {
            lut_flush(s);
        }
        v &= ~1;                        /* done at once */
        break;
    case REG_VLAN_CTRL:
        if ((v & 1) && !s->vlan_warned) {
            qemu_log_mask(LOG_UNIMP, "rtl8365mb: VLAN filtering enabled; "
                          "forwarding stays VLAN-unaware\n");
            s->vlan_warned = true;
        }
        break;
    }
    s->regs[addr] = v;
}

/* SMI slave */

static void smi_drive(RTL8365MBState *s, int level)
{
    if (s->drive != level) {
        s->drive = level;
        qemu_set_irq(s->sda_out, level);
    }
}

/* The host finished shifting in byte number s->byte (0 command, 1-2 address, 3-4 data). */
static void smi_host_byte_done(RTL8365MBState *s)
{
    uint8_t b = s->shift;

    switch (s->byte) {
    case 0:
        if (b != SMI_CMD_READ && b != SMI_CMD_WRITE) {
            qemu_log_mask(LOG_GUEST_ERROR, "rtl8365mb: SMI command 0x%02x\n", b);
            s->phase = SMI_IDLE;        /* no ACK: the host samples a 1 */
            return;
        }
        s->cmd = b;
        break;
    case 1: s->addr = b; break;
    case 2: s->addr |= b << 8; break;
    case 3: s->data = b; break;
    case 4: s->data |= b << 8; break;
    }
    s->phase = SMI_SLAVE_ACK;
    s->clocked = 0;
    smi_drive(s, 0);
}

static void smi_slave_bit(RTL8365MBState *s)
{
    uint8_t b = s->hi ? s->data >> 8 : s->data;

    smi_drive(s, (b >> (7 - s->nbits)) & 1);
}

static void smi_rising(RTL8365MBState *s)
{
    switch (s->phase) {
    case SMI_HOST_BYTE:
        s->shift = (s->shift << 1) | s->sda;
        s->nbits++;
        break;
    case SMI_SLAVE_BYTE:
        s->nbits++;
        break;
    case SMI_SLAVE_ACK:
    case SMI_HOST_ACK:
        s->clocked = 1;
        break;
    }
}

static void smi_falling(RTL8365MBState *s)
{
    switch (s->phase) {
    case SMI_HOST_BYTE:
        if (s->nbits == 8) {
            smi_host_byte_done(s);
        }
        break;
    case SMI_SLAVE_ACK:
        if (!s->clocked) {
            break;
        }
        smi_drive(s, 1);
        s->byte++;
        s->nbits = 0;
        s->shift = 0;
        if (s->cmd == SMI_CMD_READ && s->byte == 3) {
            s->data = reg_read(s, s->addr);
            s->phase = SMI_SLAVE_BYTE;
            s->hi = 0;
            smi_slave_bit(s);
        } else if (s->cmd == SMI_CMD_WRITE && s->byte == 5) {
            reg_write(s, s->addr, s->data);
            s->phase = SMI_IDLE;
        } else {
            s->phase = SMI_HOST_BYTE;
        }
        break;
    case SMI_SLAVE_BYTE:
        if (s->nbits == 8) {
            s->phase = SMI_HOST_ACK;
            s->clocked = 0;
            smi_drive(s, 1);
        } else {
            smi_slave_bit(s);
        }
        break;
    case SMI_HOST_ACK:
        if (!s->clocked) {
            break;
        }
        if (!s->hi) {
            s->hi = 1;
            s->nbits = 0;
            s->phase = SMI_SLAVE_BYTE;
            smi_slave_bit(s);
        } else {
            s->phase = SMI_IDLE;
        }
        break;
    }
}

static void smi_sck(void *opaque, int n, int level)
{
    RTL8365MBState *s = opaque;

    level = !!level;
    if (level == s->sck) {
        return;
    }
    s->sck = level;
    if (s->in_reset || s->phase == SMI_IDLE) {
        return;
    }
    if (level) {
        smi_rising(s);
    } else {
        smi_falling(s);
    }
}

static void smi_sda(void *opaque, int n, int level)
{
    RTL8365MBState *s = opaque;
    int old = s->sda;

    s->sda = level = !!level;
    if (!s->sck || old == level || s->in_reset) {
        return;
    }
    if (!level) {                       /* START: SDA falls while SCK is high */
        s->phase = SMI_HOST_BYTE;
        s->cmd = s->byte = s->nbits = s->shift = s->clocked = s->hi = 0;
        s->addr = s->data = 0;
    } else {                            /* STOP */
        s->phase = SMI_IDLE;
    }
    smi_drive(s, 1);
}

/* Forwarding tables at their reset values */
static void rtl_table_defaults(RTL8365MBState *s)
{
    for (int p = 0; p < NPHYS; p++) {
        s->regs[REG_PORT_ISO(p)] = ISO_ALL;
    }
    lut_flush(s);
    s->vlan_warned = false;
}

static void rtl_regs_reset(RTL8365MBState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->ocp, 0, sizeof(s->ocp));
    for (int p = 0; p < RTL8365MB_NPHY; p++) {
        s->ocp[p][OCP_BMCR >> 1] = 0x1140;      /* AN enable, 1000 FD */
    }
    rtl_table_defaults(s);
    s->phase = SMI_IDLE;
}

static void smi_reset_pin(void *opaque, int n, int level)
{
    RTL8365MBState *s = opaque;

    if (!level && !s->in_reset) {
        rtl_regs_reset(s);
    }
    s->in_reset = !level;
    smi_drive(s, 1);
}

/* Data path */

static void rtl_send(RTL8365MBState *s, int p, const uint8_t *buf, size_t len)
{
    uint8_t pad[60];

    if (len < 60) {
        memset(pad, 0, sizeof(pad));
        memcpy(pad, buf, len);
        buf = pad;
        len = sizeof(pad);
    }
    if (p < 0) {
        if (s->uplink) {
            s->uplink(s->uplink_opaque, buf, len);
        }
    } else {
        qemu_send_packet(qemu_get_queue(s->ext[p].nic), buf, len);
    }
}

static unsigned lut_slot(const uint8_t *mac)
{
    uint32_t h = 0;

    for (int i = 0; i < 6; i++) {
        h = h * 31 + mac[i];
    }
    return h % RTL8365MB_LUT_SIZE;
}

static void lut_learn(RTL8365MBState *s, const uint8_t *mac, int port, int64_t now)
{
    unsigned i = lut_slot(mac);

    if (!(mac[0] & 1)) {
        memcpy(s->lut[i].mac, mac, 6);
        s->lut[i].port = port;
        s->lut[i].valid = 1;
        s->lut[i].seen = now;
    }
}

/* Physical port a unicast address was learned on, or -1 to flood */
static int lut_lookup(RTL8365MBState *s, const uint8_t *mac, int64_t now)
{
    unsigned i = lut_slot(mac);

    if (mac[0] & 1 || !s->lut[i].valid || memcmp(s->lut[i].mac, mac, 6)) {
        return -1;
    }
    if (now - s->lut[i].seen > LUT_AGE_NS) {
        s->lut[i].valid = 0;
        return -1;
    }
    return s->lut[i].port;
}

static bool port_up(RTL8365MBState *s, int phys)
{
    return phys == RTL8365MB_EXT1 ? ext_link(s) : utp_link(s, phys);
}

static void rtl_egress(RTL8365MBState *s, int phys, const uint8_t *buf, size_t len)
{
    rtl_send(s, phys == RTL8365MB_EXT1 ? -1 : phys, buf, len);
}

/* src and every port are physical: UTP 0-3, or RTL8365MB_EXT1 */
static void rtl_forward(RTL8365MBState *s, int src, const uint8_t *buf, size_t len)
{
    static const int ports[] = { 0, 1, 2, 3, RTL8365MB_EXT1 };
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint16_t allowed = s->regs[REG_PORT_ISO(src)];
    int dst;

    lut_learn(s, buf + 6, src, now);
    dst = lut_lookup(s, buf, now);
    if (dst >= 0 && port_up(s, dst)) {
        if (dst != src && (allowed & (1 << dst))) {
            rtl_egress(s, dst, buf, len);
        }
        return;
    }
    for (int i = 0; i < ARRAY_SIZE(ports); i++) {
        int p = ports[i];

        if (p != src && (allowed & (1 << p)) && port_up(s, p)) {
            rtl_egress(s, p, buf, len);
        }
    }
}

void rtl8365mb_from_uplink(RTL8365MBState *s, const uint8_t *buf, size_t len)
{
    if (len >= 14 && ext_link(s)) {
        rtl_forward(s, RTL8365MB_EXT1, buf, len);
    }
}

void rtl8365mb_connect_uplink(RTL8365MBState *s, RTL8365MBUplinkFn *fn, void *opaque)
{
    s->uplink = fn;
    s->uplink_opaque = opaque;
}

static bool rtl_can_receive(NetClientState *nc)
{
    return true;
}

static ssize_t rtl_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    RTL8365MBPort *pp = qemu_get_nic_opaque(nc);

    if (size >= 14 && utp_link(pp->sw, pp->port)) {
        rtl_forward(pp->sw, pp->port, buf, size);
    }
    return size;
}

static NetClientInfo rtl_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = rtl_can_receive,
    .receive = rtl_receive,
};

static void rtl_realize(DeviceState *dev, Error **errp)
{
    RTL8365MBState *s = RTL8365MB(dev);

    for (int p = 0; p < RTL8365MB_NUTP; p++) {
        RTL8365MBPort *pp = &s->ext[p];
        g_autofree char *id = g_strdup_printf("%s.p%d", dev->id ?: TYPE_RTL8365MB, p);

        pp->sw = s;
        pp->port = p;
        if (!pp->conf.peers.ncs[0]) {
            continue;
        }
        qemu_macaddr_default_if_unset(&pp->conf.macaddr);
        pp->nic = qemu_new_nic(&rtl_net_info, &pp->conf, TYPE_RTL8365MB, id,
                               &dev->mem_reentrancy_guard, pp);
    }
    /* The board creates this device without a bus, so its reset handler
     * does not run at power-on; the forwarding tables must not start at 0. */
    rtl_table_defaults(s);
}

static void rtl_reset(DeviceState *dev)
{
    RTL8365MBState *s = RTL8365MB(dev);

    rtl_regs_reset(s);
    s->sck = s->sda = 1;
    s->in_reset = 0;
    s->drive = 0;
    smi_drive(s, 1);                    /* release SDA: the pad reads high */
}

static void rtl_init(Object *obj)
{
    RTL8365MBState *s = RTL8365MB(obj);

    qdev_init_gpio_in_named(DEVICE(obj), smi_sck, "sck", 1);
    qdev_init_gpio_in_named(DEVICE(obj), smi_sda, "sda", 1);
    qdev_init_gpio_in_named(DEVICE(obj), smi_reset_pin, "reset-n", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->sda_out, "sda-out", 1);
}

static const Property rtl_properties[] = {
    DEFINE_PROP_NETDEV("netdev-p0", RTL8365MBState, ext[0].conf.peers),
    DEFINE_PROP_NETDEV("netdev-p1", RTL8365MBState, ext[1].conf.peers),
    DEFINE_PROP_NETDEV("netdev-p2", RTL8365MBState, ext[2].conf.peers),
    DEFINE_PROP_NETDEV("netdev-p3", RTL8365MBState, ext[3].conf.peers),
};

static int rtl_post_load(void *opaque, int version_id)
{
    lut_flush(opaque);                  /* not migrated: relearned from traffic */
    return 0;
}

static const VMStateDescription vmstate_rtl = {
    .name = TYPE_RTL8365MB,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = rtl_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(sck, RTL8365MBState),
        VMSTATE_UINT8(sda, RTL8365MBState),
        VMSTATE_UINT8(in_reset, RTL8365MBState),
        VMSTATE_UINT8(phase, RTL8365MBState),
        VMSTATE_UINT8(nbits, RTL8365MBState),
        VMSTATE_UINT8(clocked, RTL8365MBState),
        VMSTATE_UINT8(byte, RTL8365MBState),
        VMSTATE_UINT8(hi, RTL8365MBState),
        VMSTATE_UINT8(cmd, RTL8365MBState),
        VMSTATE_UINT8(shift, RTL8365MBState),
        VMSTATE_UINT8(drive, RTL8365MBState),
        VMSTATE_UINT16(addr, RTL8365MBState),
        VMSTATE_UINT16(data, RTL8365MBState),
        VMSTATE_UINT16_ARRAY(regs, RTL8365MBState, 0x10000),
        VMSTATE_UINT16_2DARRAY(ocp, RTL8365MBState, RTL8365MB_NPHY, 0x10000 / 2),
        VMSTATE_END_OF_LIST()
    },
};

static void rtl_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rtl_realize;
    dc->vmsd = &vmstate_rtl;
    device_class_set_legacy_reset(dc, rtl_reset);
    device_class_set_props(dc, rtl_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo rtl8365mb_types[] = {
    {
        .name          = TYPE_RTL8365MB,
        .parent        = TYPE_DEVICE,
        .instance_size = sizeof(RTL8365MBState),
        .instance_init = rtl_init,
        .class_init    = rtl_class_init,
    },
};

DEFINE_TYPES(rtl8365mb_types)
