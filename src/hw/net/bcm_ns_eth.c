/*
 * Broadcom Northstar Ethernet: GMAC cores and the BCM53012 switch (SRAB).
 *
 * GMAC (core 0x82d rev 7), as et.ko drives it on BCM47094:
 *   - One RX channel (ch0, +0x220) and four TX channels (+0x200/240/280/2c0),
 *     dma64 layout (the IDM wrapper reports SISF_DMA64). Rings are 8 KiB
 *     aligned: addrlow bits 12:0 read as zero, which is how hnddma decides
 *     that ptr/CD are byte offsets inside the ring.
 *   - Descriptors are 16 bytes and never written back by hardware. Progress is
 *     reported only through status0.CD.
 *   - RX buffer = 28-byte receive header (le16 length incl. FCS, le16 flags),
 *     frame at the control.RO offset (30 for eth units, 18 for forwarders).
 *   - INT_STATUS is W1C; the line is INT_STATUS & INT_MASK.
 *   - TX and RX complete synchronously. MIB counters read as zero.
 *   - Reset values are an untouched core's on a unit (GMAC3, which the
 *     firmware never brings up); the driver read-modify-writes several of
 *     them. DEV_STATUS and UniMAC 0x844 are status words: [assume] they
 *     take the brought-up value once DEV_CONTROL bit 4 is set, as it is on
 *     the three cores et uses.
 *   - Offsets in 0x000-0x1fc and 0x800-0x87c that a BCM47094 unit does not
 *     decode (the same 116 on all four cores) abort the access, as on the
 *     unit, where a devmem read of one takes a bus error.
 *
 * BCM53012 behind the SRAB at 0x18007000:
 *   - SRAB commands complete synchronously; the grant follows the request.
 *   - Switch registers are a store indexed by (page, reg). The vendor robo
 *     driver always addresses a register at its first byte with its width,
 *     so registers do not overlap.
 *   - Reset values, the internal GPHYs' ID, BMSR and ANLPAR, and the
 *     link/speed/duplex summaries are matched to a unit's registers.
 *   - The VLAN table (5395-style access at 05:80/81/83) and per-port PVIDs
 *     (34:10+2p) drive forwarding. Ports 0-4 can carry a QEMU netdev, port 7
 *     is GMAC1, port 8 (IMP) is GMAC2. No MAC learning: frames flood to the
 *     VLAN's members.
 *   - Broadcom header (02:03 bit 0/1/2 = port 8/5/7): et's agg_attach turns it
 *     on for the IMP port through robo_bhdr_register(). Frames to such a port
 *     carry a 4-byte tag after the source MAC (opcode 0, source port in byte
 *     3); frames from it carry one too: opcode 0 = normal forwarding, opcode 1
 *     = send to the port bitmap in bytes 2-3 as is.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/net/bcm_ns_eth.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"

/* ---- GMAC ---------------------------------------------------------------- */

#define GMAC_DEV_STATUS     0x004
#define GMAC_INT_STATUS     0x020
#define GMAC_INT_MASK       0x024
#define GMAC_FIFO_DATA      0x128
#define GMAC_DMA_BASE       0x200
#define GMAC_DMA_END        0x300
#define GMAC_MIB_BASE       0x300
#define GMAC_MIB_END        0x430
#define GMAC_DEV_CONTROL    0x000
#define UMAC_CMD            0x808
#define UMAC_STATUS         0x844
#define DEVCTL_UP           (1u << 4)   /* set by et on every core it uses */

#define INT_RX              (1u << 16)
#define INT_TX(c)           (1u << (24 + (c)))

#define CMD_TX_EN           (1u << 0)
#define CMD_RX_EN           (1u << 1)
#define CMD_SW_RESET        (1u << 13)

/* dma64 per-direction registers */
#define D64_CONTROL         0x00
#define D64_PTR             0x04
#define D64_ADDRLOW         0x08
#define D64_ADDRHIGH        0x0c
#define D64_STATUS0         0x10
#define D64_STATUS1         0x14
#define D64_EN              (1u << 0)          /* XE / RE */
#define D64_XC_SE           (1u << 1)
#define D64_RC_RO(c)        (((c) >> 1) & 0x7f)
#define D64_RING_MASK       0x1fff
#define D64_STATE_DISABLED  0
#define D64_STATE_ACTIVE    1
#define D64_STATE_IDLE      2

#define D64_CTRL1_EOT       (1u << 28)
#define D64_CTRL1_IOC       (1u << 29)
#define D64_CTRL1_EOF       (1u << 30)
#define D64_CTRL1_SOF       (1u << 31)
#define D64_CTRL2_BC        0x7fff

#define RX_HDR_LEN          28
#define RX_HDR_MIN          4           /* len + flags */
#define ETH_FCS_LEN         4

static void gmac_update_irq(BCMNSGmacState *s)
{
    qemu_set_irq(s->irq, !!(s->regs[GMAC_INT_STATUS / 4] & s->regs[GMAC_INT_MASK / 4]));
}

/* channel-relative register block: TX ch c at 0x200 + 0x40c, RX ch0 at 0x220 */
static uint32_t *d64(BCMNSGmacState *s, bool rx, int c)
{
    return &s->regs[(GMAC_DMA_BASE + 0x40 * c + (rx ? 0x20 : 0)) / 4];
}

static void d64_desc(BCMNSGmacState *s, uint32_t ring, uint32_t cd, uint32_t d[4])
{
    address_space_read(&address_space_memory, ring + cd, MEMTXATTRS_UNSPECIFIED, d, 16);
    for (int i = 0; i < 4; i++) {
        d[i] = le32_to_cpu(d[i]);
    }
}

static uint32_t d64_next(uint32_t cd, uint32_t ctrl1)
{
    return (ctrl1 & D64_CTRL1_EOT) ? 0 : (cd + 16) & D64_RING_MASK;
}

static bool umac_up(BCMNSGmacState *s, uint32_t en)
{
    uint32_t cmd = s->regs[UMAC_CMD / 4];

    return (cmd & en) && !(cmd & CMD_SW_RESET);
}

/* Transmit every complete frame posted between CD and ptr. */
static void gmac_tx(BCMNSGmacState *s, int c)
{
    uint32_t *r = d64(s, false, c);
    uint32_t ring = r[D64_ADDRLOW / 4];
    uint32_t end = r[D64_PTR / 4] & D64_RING_MASK;
    uint32_t pos = s->tx_cd[c];
    size_t flen = 0;

    if (!(r[D64_CONTROL / 4] & D64_EN) || (r[D64_CONTROL / 4] & D64_XC_SE)) {
        return;
    }
    for (int n = 0; pos != end && n < 512; n++) {
        uint32_t d[4];
        uint32_t len;

        d64_desc(s, ring, pos, d);
        len = d[1] & D64_CTRL2_BC;
        if (d[0] & D64_CTRL1_SOF) {
            flen = 0;
        }
        len = MIN(len, sizeof(s->txbuf) - flen);
        address_space_read(&address_space_memory, d[2], MEMTXATTRS_UNSPECIFIED,
                           s->txbuf + flen, len);
        flen += len;
        pos = d64_next(pos, d[0]);
        if (d[0] & D64_CTRL1_EOF) {
            /* DMA drains into the MAC even while it is disabled; the frame is
             * then lost, as on the wire. */
            if (umac_up(s, CMD_TX_EN) && s->sw) {
                bcm53012_ingress(s->sw, s->sw_port, s->txbuf, flen);
            }
            if (d[0] & D64_CTRL1_IOC) {
                s->regs[GMAC_INT_STATUS / 4] |= INT_TX(c);
            }
            s->tx_cd[c] = pos;
            flen = 0;
        }
    }
    gmac_update_irq(s);
}

void bcm_ns_gmac_receive(BCMNSGmacState *s, const uint8_t *buf, size_t len)
{
    uint32_t *r = d64(s, true, 0);
    uint32_t ring = r[D64_ADDRLOW / 4];
    uint32_t ro = D64_RC_RO(r[D64_CONTROL / 4]);
    uint8_t hdr[RX_HDR_LEN] = { 0 };
    static const uint8_t fcs[ETH_FCS_LEN];
    uint32_t d[4];

    if (!umac_up(s, CMD_RX_EN) || !(r[D64_CONTROL / 4] & D64_EN) ||
        s->rx_cd == (r[D64_PTR / 4] & D64_RING_MASK)) {
        s->rx_dropped++;
        return;
    }
    d64_desc(s, ring, s->rx_cd, d);
    /* the receive offset (RO) leaves room for the status header; drivers use
     * 30 (header + 2 for IP alignment) or 14, so only the 4-byte length/flags
     * word has to fit, and the header is written up to RO */
    if (ro < RX_HDR_MIN || ro + len + ETH_FCS_LEN > (d[1] & D64_CTRL2_BC)) {
        /* would need a multi-buffer frame; et does not enable DMA_CTRL_RXMULTI */
        s->rx_dropped++;
        return;
    }
    /* frame first: the driver polls the header's length word */
    address_space_write(&address_space_memory, d[2] + ro, MEMTXATTRS_UNSPECIFIED, buf, len);
    address_space_write(&address_space_memory, d[2] + ro + len, MEMTXATTRS_UNSPECIFIED,
                        fcs, ETH_FCS_LEN);
    stw_le_p(hdr, len + ETH_FCS_LEN);
    address_space_write(&address_space_memory, d[2], MEMTXATTRS_UNSPECIFIED, hdr,
                        MIN(ro, sizeof(hdr)));
    s->rx_cd = d64_next(s->rx_cd, d[0]);
    s->regs[GMAC_INT_STATUS / 4] |= INT_RX;
    gmac_update_irq(s);
}

static uint32_t d64_status0(BCMNSGmacState *s, bool rx, int c)
{
    uint32_t *r = d64(s, rx, c);
    uint32_t cd = rx ? s->rx_cd : s->tx_cd[c];
    uint32_t state;

    if (!(r[D64_CONTROL / 4] & D64_EN)) {
        state = D64_STATE_DISABLED;
    } else if (!rx && (r[D64_CONTROL / 4] & D64_XC_SE)) {
        state = D64_STATE_IDLE;
    } else {
        state = cd == (r[D64_PTR / 4] & D64_RING_MASK) ? D64_STATE_IDLE : D64_STATE_ACTIVE;
    }
    return cd | (state << 28);
}

static uint64_t gmac_read(void *opaque, hwaddr addr, unsigned size)
{
    BCMNSGmacState *s = opaque;

    if (addr >= GMAC_DMA_BASE && addr < GMAC_DMA_END) {
        int c = (addr - GMAC_DMA_BASE) / 0x40;
        bool rx = addr & 0x20;

        switch (addr & 0x1f) {
        case D64_STATUS0:
            return d64_status0(s, rx, c);
        case D64_STATUS1:                       /* AD = CD, no errors */
            return rx ? s->rx_cd : s->tx_cd[c];
        }
    }
    if (addr >= GMAC_MIB_BASE && addr < GMAC_MIB_END) {
        return 0;
    }
    if (addr == GMAC_DEV_STATUS) {
        return (s->regs[GMAC_DEV_CONTROL / 4] & DEVCTL_UP) ? 0x04131100 : 0x04130000;
    }
    if (addr == UMAC_STATUS) {
        return (s->regs[GMAC_DEV_CONTROL / 4] & DEVCTL_UP) ? 0x0b : 0x1a;
    }
    return s->regs[addr / 4];
}

static void gmac_dma_write(BCMNSGmacState *s, hwaddr addr, uint32_t val)
{
    int c = (addr - GMAC_DMA_BASE) / 0x40;
    bool rx = addr & 0x20;
    uint32_t *r = d64(s, rx, c);
    uint32_t *cd = rx ? &s->rx_cd : &s->tx_cd[c];

    if (rx && c) {
        return;                                 /* only ch0 has a receiver */
    }
    switch (addr & 0x1f) {
    case D64_CONTROL:
        if (!(val & D64_EN)) {
            *cd = 0;
        }
        r[D64_CONTROL / 4] = val;
        break;
    case D64_PTR:
        r[D64_PTR / 4] = val;
        break;
    case D64_ADDRLOW:
        r[D64_ADDRLOW / 4] = val & ~D64_RING_MASK;
        *cd = 0;
        break;
    case D64_ADDRHIGH:
        r[D64_ADDRHIGH / 4] = val;
        break;
    default:                                    /* status: read-only */
        return;
    }
    if (!rx) {
        gmac_tx(s, c);
    }
}

static void gmac_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    BCMNSGmacState *s = opaque;

    if (addr >= GMAC_DMA_BASE && addr < GMAC_DMA_END) {
        gmac_dma_write(s, addr, val);
        return;
    }
    switch (addr) {
    case GMAC_INT_STATUS:
        s->regs[addr / 4] &= ~val;
        break;
    case GMAC_DEV_STATUS:
    case UMAC_STATUS:
        return;
    default:
        if (addr >= GMAC_MIB_BASE && addr < GMAC_MIB_END) {
            return;
        }
        s->regs[addr / 4] = val;
        break;
    }
    if (addr == UMAC_CMD) {
        for (int c = 0; c < BCM_NS_GMAC_NTX; c++) {
            gmac_tx(s, c);
        }
    }
    gmac_update_irq(s);
}

/* Words in 0x000-0x1fc and 0x800-0x87c with no register, from bus errors on a unit */
static const uint32_t gmac_hole_lo[4] = { 0xfffff8f4, 0xffffffff, 0xfffff8f0, 0xfafffc82 };
static const uint32_t gmac_hole_umac = 0x00417f81;

static bool gmac_hole(hwaddr addr)
{
    unsigned w;

    if (addr < GMAC_DMA_BASE) {
        w = addr / 4;
        return gmac_hole_lo[w / 32] & (1u << (w % 32));
    }
    if (addr >= 0x800 && addr < 0x880) {
        return gmac_hole_umac & (1u << ((addr - 0x800) / 4));
    }
    return false;
}

static MemTxResult gmac_read_attrs(void *opaque, hwaddr addr, uint64_t *data,
                                   unsigned size, MemTxAttrs attrs)
{
    if (gmac_hole(addr)) {
        return MEMTX_ERROR;             /* SLVERR: the unit reports DFSR.ExT = 1 */
    }
    *data = gmac_read(opaque, addr, size);
    return MEMTX_OK;
}

static MemTxResult gmac_write_attrs(void *opaque, hwaddr addr, uint64_t data,
                                    unsigned size, MemTxAttrs attrs)
{
    if (gmac_hole(addr)) {
        return MEMTX_ERROR;             /* SLVERR: the unit reports DFSR.ExT = 1 */
    }
    gmac_write(opaque, addr, data, size);
    return MEMTX_OK;
}

static const MemoryRegionOps gmac_ops = {
    .read_with_attrs = gmac_read_attrs,
    .write_with_attrs = gmac_write_attrs,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* Reset values as a unit's untouched GMAC3 reads them (offset, value) */
static const uint32_t gmac_reset_regs[][2] = {
    { 0x000, 0x00300003 }, { 0x104, 0x04cc0599 }, { 0x108, 0x0a0a0a0a },
    { 0x10c, 0x00000404 }, { 0x188, 0x00100000 }, { 0x18c, 0x00000271 },
    { 0x190, 0x06000001 }, { 0x804, 0x00000014 }, { 0x808, 0x010000db },
    { 0x814, 0x000005ee }, { 0x818, 0x0000ffff }, { 0x848, 0x00018100 },
    { 0x84c, 0x00018100 }, { 0x854, 0x00000007 }, { 0x860, 0x0000ffff },
    { 0x864, 0x00000040 }, { 0x868, 0x00000022 }, { 0x86c, 0x0000003c },
    { 0x870, 0x0000007d },
};

static void gmac_reset(DeviceState *dev)
{
    BCMNSGmacState *s = BCM_NS_GMAC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    for (size_t i = 0; i < ARRAY_SIZE(gmac_reset_regs); i++) {
        s->regs[gmac_reset_regs[i][0] / 4] = gmac_reset_regs[i][1];
    }
    s->regs[GMAC_FIFO_DATA / 4] = s->fifo_data;
    memset(s->tx_cd, 0, sizeof(s->tx_cd));
    s->rx_cd = 0;
}

static void gmac_init(Object *obj)
{
    BCMNSGmacState *s = BCM_NS_GMAC(obj);

    memory_region_init_io(&s->iomem, obj, &gmac_ops, s, TYPE_BCM_NS_GMAC, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription vmstate_gmac = {
    .name = TYPE_BCM_NS_GMAC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, BCMNSGmacState, 0x1000 / 4),
        VMSTATE_UINT32_ARRAY(tx_cd, BCMNSGmacState, BCM_NS_GMAC_NTX),
        VMSTATE_UINT32(rx_cd, BCMNSGmacState),
        VMSTATE_UINT32(rx_dropped, BCMNSGmacState),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * +0x128 differs per core on a unit and is not a reset value: the cores the
 * firmware leaves idle read a fixed word each (0x19e, 0x3c, 0x32b), and the
 * one carrying the LAN traffic (GMAC2) read 0x1597, then 0x17b8 seconds later.
 * [assume] it is the FIFO access data window after 0x120/0x124 (address and
 * byte select), so it shows FIFO RAM contents. et.ko never touches 0x120-0x128;
 * only a register dump sees it. The board sets each core's word; it does not
 * move with traffic here.
 */
static const Property gmac_properties[] = {
    DEFINE_PROP_UINT32("fifo-data", BCMNSGmacState, fifo_data, 0),
};

static void gmac_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, gmac_properties);
    dc->vmsd = &vmstate_gmac;
    device_class_set_legacy_reset(dc, gmac_reset);
}

/* ---- BCM53012 + SRAB ----------------------------------------------------- */

#define SRAB_CMDSTAT        0x2c
#define SRAB_WD_H           0x30
#define SRAB_WD_L           0x34
#define SRAB_RD_H           0x38
#define SRAB_RD_L           0x3c
#define SRAB_CTRLS          0x40
#define SRAB_INTR           0x44
#define CMDSTAT_GORDYN      (1u << 0)
#define CMDSTAT_WRITE       (1u << 1)
#define CMDSTAT_RST         (1u << 2)
#define CTRLS_RCAREQ        (1u << 3)
#define CTRLS_RCAGNT        (1u << 4)
#define CTRLS_SW_INIT_DONE  (1u << 6)

#define R(page, reg)        (((page) << 8) | (reg))
#define ROBO_DEVID          0x53012
#define PAGE_GPHY(p)        (0x10 + (p))
#define VLAN_CTRL0_EN       0x80
#define VTA_START           0x80
#define VTA_READ            0x01
#define BMCR_RESET          0x8000
#define BMCR_PDOWN          0x0800
#define BMCR_ANRESTART      0x0200
#define BMCR_DEFAULT        0x1140      /* AN on, full duplex, 1000 */
#define BMSR_BASE           0x79c9      /* abilities; | 0x24 when linked */
#define GPHY_ID1            0x600d      /* reg 2/3 of every internal GPHY on a unit */
#define GPHY_ID2            0x8720
#define GPHY_ANAR           0x0de1
#define GPHY_ANLPAR         0xdde1      /* a 1000FD peer with pause, as a unit read it */
#define NOLINK_1000FD_PORT  6           /* reads 1000FD with no link on a unit */
#define IMP_PORT            8
#define BRCM_TAG_LEN        4

/* 02:03 BRCM_HDR enable: bit 0 = port 8, bit 1 = port 5, bit 2 = port 7 */
static bool sw_bhdr(BCM53012State *sw, int p)
{
    uint64_t en = sw->regs[(0x02 << 8) | 0x03];

    return (p == IMP_PORT && (en & 1)) || (p == 5 && (en & 2)) || (p == 7 && (en & 4));
}

static bool sw_link(BCM53012State *sw, int p)
{
    if (p < BCM53012_NEXT) {
        NICState *nic = sw->ext[p].nic;

        return nic && !qemu_get_queue(nic)->link_down &&
               !(sw->regs[R(PAGE_GPHY(p), 0x00)] & BMCR_PDOWN);
    }
    return p == 5 || p == 7 || p == IMP_PORT;
}

static uint64_t robo_read(BCM53012State *sw, int page, int reg)
{
    uint64_t v = 0;
    int p;

    switch (R(page, reg)) {
    case R(0x02, 0x30):
        return ROBO_DEVID;
    case R(0x02, 0x40):
        return 0;                               /* rev 0 (real HW: "BCM53012, rev 0") */
    case R(0x01, 0x00):                         /* link status, bit per port */
        for (p = 0; p < BCM53012_NPORTS; p++) {
            v |= (uint64_t)sw_link(sw, p) << p;
        }
        return v;
    case R(0x01, 0x08):                         /* duplex: full on every linked port */
        for (p = 0; p < BCM53012_NPORTS; p++) {
            v |= (uint64_t)(sw_link(sw, p) || p == NOLINK_1000FD_PORT) << p;
        }
        return v;
    case R(0x01, 0x04):                         /* speed, 2 bits per port, 2 = 1000 */
        for (p = 0; p < BCM53012_NPORTS; p++) {
            bool g = sw_link(sw, p) || p == NOLINK_1000FD_PORT;

            v |= (uint64_t)(g ? 2 : 0) << (2 * p);
        }
        return v;
    }
    if (page >= PAGE_GPHY(0) && page < PAGE_GPHY(BCM53012_NEXT)) {
        bool link = sw_link(sw, page - PAGE_GPHY(0));

        switch (reg) {
        case 0x02:          /* BMSR: link + autoneg complete when a netdev is up */
            return BMSR_BASE | (link ? 0x0024 : 0);
        case 0x04:
            return GPHY_ID1;
        case 0x06:
            return GPHY_ID2;
        case 0x0a:          /* ANLPAR */
            return link ? GPHY_ANLPAR : 0;
        }
    }
    return sw->regs[R(page, reg)];
}

static void robo_write(BCM53012State *sw, int page, int reg, uint64_t v)
{
    switch (R(page, reg)) {
    case R(0x02, 0x30):
    case R(0x02, 0x40):
        return;
    case R(0x05, 0x80):                         /* VLAN table access */
        if (v & VTA_START) {
            uint32_t idx = sw->regs[R(0x05, 0x81)] & 0xfff;

            if (v & VTA_READ) {
                sw->regs[R(0x05, 0x83)] = sw->vtable[idx];
            } else {
                sw->vtable[idx] = sw->regs[R(0x05, 0x83)] & 0x3ffff;
            }
        }
        v &= ~(uint64_t)VTA_START;
        break;
    case R(0x05, 0x00):                         /* ARL access: no ARL; done at once */
        v &= ~(uint64_t)0x80;
        break;
    }
    if (page >= PAGE_GPHY(0) && page < PAGE_GPHY(BCM53012_NEXT) && reg == 0x00) {
        /* reset and restart-autoneg self-clear; reset restores the default */
        v = (v & BMCR_RESET) ? BMCR_DEFAULT : v & ~(uint64_t)BMCR_ANRESTART;
    }
    sw->regs[R(page, reg)] = v;
}

static void sw_egress(BCM53012State *sw, int p, int src, const uint8_t *buf, size_t len)
{
    uint8_t pad[60 + BRCM_TAG_LEN];
    g_autofree uint8_t *tagged = NULL;

    if (!sw_link(sw, p)) {
        return;
    }
    if (sw_bhdr(sw, p)) {
        tagged = g_malloc(MAX(len, 60) + BRCM_TAG_LEN);
        memcpy(tagged, buf, 12);
        tagged[12] = 0;                         /* opcode 0 */
        tagged[13] = 0;
        tagged[14] = 0;                         /* reason: normal forwarding */
        tagged[15] = src & 0x1f;
        memcpy(tagged + 16, buf + 12, len - 12);
        len += BRCM_TAG_LEN;
        if (len < 60 + BRCM_TAG_LEN) {
            memset(tagged + len, 0, 60 + BRCM_TAG_LEN - len);
            len = 60 + BRCM_TAG_LEN;
        }
        buf = tagged;
    } else if (len < 60) {                    /* the wire minimum, without FCS */
        memset(pad, 0, 60);
        memcpy(pad, buf, len);
        buf = pad;
        len = 60;
    }
    if (p < BCM53012_NEXT) {
        qemu_send_packet(qemu_get_queue(sw->ext[p].nic), buf, len);
    } else if (sw->gmac[p]) {
        bcm_ns_gmac_receive(sw->gmac[p], buf, len);
    } else if (sw->mac_fn[p]) {
        sw->mac_fn[p](sw->mac_opaque[p], buf, len);
    }
}

static unsigned arl_slot(uint16_t vid, const uint8_t *mac)
{
    uint32_t h = vid;

    for (int i = 0; i < 6; i++) {
        h = h * 31 + mac[i];
    }
    return h % BCM53012_ARL_SIZE;
}

static void arl_learn(BCM53012State *sw, uint16_t vid, const uint8_t *mac, int port)
{
    unsigned i = arl_slot(vid, mac);

    if (!(mac[0] & 1)) {
        memcpy(sw->arl[i].mac, mac, 6);
        sw->arl[i].vid = vid;
        sw->arl[i].port = port;
    }
}

/* Port a unicast address was learned on in this VLAN, or -1 to flood */
static int arl_lookup(BCM53012State *sw, uint16_t vid, const uint8_t *mac)
{
    unsigned i = arl_slot(vid, mac);

    if (mac[0] & 1 || sw->arl[i].vid != vid || memcmp(sw->arl[i].mac, mac, 6)) {
        return -1;
    }
    return sw->arl[i].port;
}

void bcm53012_ingress(BCM53012State *sw, int port, const uint8_t *buf, size_t len)
{
    int dport;
    g_autofree uint8_t *out = NULL;
    uint32_t members, untag, entry;
    uint16_t vid, tci = 0;
    bool tagged;
    size_t plen;

    g_autofree uint8_t *untagged = NULL;

    if (len < 14 || !sw_link(sw, port)) {
        return;
    }
    if (sw_bhdr(sw, port)) {
        uint8_t opcode = buf[12] >> 5;
        uint16_t dst = ((buf[14] & 1) << 8) | buf[15];

        if (len < 14 + BRCM_TAG_LEN) {
            return;
        }
        untagged = g_malloc(len - BRCM_TAG_LEN);
        memcpy(untagged, buf, 12);
        memcpy(untagged + 12, buf + 12 + BRCM_TAG_LEN, len - 12 - BRCM_TAG_LEN);
        buf = untagged;
        len -= BRCM_TAG_LEN;
        if (opcode == 1) {
            for (int p = 0; p < BCM53012_NPORTS; p++) {
                if (p != port && (dst & (1u << p))) {
                    sw_egress(sw, p, port, buf, len);
                }
            }
            return;
        }
    }
    if (!(sw->regs[R(0x34, 0x00)] & VLAN_CTRL0_EN)) {
        for (int p = 0; p < BCM53012_NPORTS; p++) {
            if (p != port) {
                sw_egress(sw, p, port, buf, len);
            }
        }
        return;
    }

    tagged = len >= 18 && lduw_be_p(buf + 12) == 0x8100;
    if (tagged) {
        tci = lduw_be_p(buf + 14);
    }
    vid = tci & 0xfff;
    if (!vid) {                                 /* untagged or priority-tagged */
        vid = sw->regs[R(0x34, 0x10 + 2 * port)] & 0xfff;
        tci = (tci & 0xf000) | vid;
    }
    entry = sw->vtable[vid];
    members = entry & 0x1ff;
    untag = (entry >> 9) & 0x1ff;
    if (!(members & (1u << port))) {
        return;
    }
    arl_learn(sw, vid, buf + 6, port);
    dport = arl_lookup(sw, vid, buf);
    if (dport >= 0 && (dport == port || !(members & (1u << dport)))) {
        return;                                 /* destination is behind the ingress port */
    }
    if (dport >= 0) {
        members = 1u << dport;
    }

    /* out = untagged frame followed by room to build the tagged one */
    plen = tagged ? len - 4 : len;
    out = g_malloc(2 * plen + 4);
    memcpy(out, buf, 12);
    memcpy(out + 12, buf + (tagged ? 16 : 12), plen - 12);
    {
        uint8_t *t = out + plen;

        memcpy(t, out, 12);
        stw_be_p(t + 12, 0x8100);
        stw_be_p(t + 14, tci);
        memcpy(t + 16, out + 12, plen - 12);
    }
    for (int p = 0; p < BCM53012_NPORTS; p++) {
        if (p == port || !(members & (1u << p))) {
            continue;
        }
        if (untag & (1u << p)) {
            sw_egress(sw, p, port, out, plen);
        } else {
            sw_egress(sw, p, port, out + plen, plen + 4);
        }
    }
}

void bcm53012_connect_gmac(BCM53012State *sw, int port, BCMNSGmacState *gmac)
{
    assert(port >= BCM53012_NEXT && port < BCM53012_NPORTS);
    sw->gmac[port] = gmac;
    gmac->sw = sw;
    gmac->sw_port = port;
}

void bcm53012_connect_mac(BCM53012State *sw, int port, BCM53012MacFn *fn, void *opaque)
{
    assert(port >= BCM53012_NEXT && port < BCM53012_NPORTS && !sw->gmac[port]);
    sw->mac_fn[port] = fn;
    sw->mac_opaque[port] = opaque;
}

static void srab_exec(BCM53012State *sw, uint32_t cmd)
{
    int page = (cmd >> 24) & 0xff, reg = (cmd >> 16) & 0xff;

    if (cmd & CMDSTAT_WRITE) {
        robo_write(sw, page, reg, ((uint64_t)sw->wd_h << 32) | sw->wd_l);
    } else {
        uint64_t v = robo_read(sw, page, reg);

        sw->rd_h = v >> 32;
        sw->rd_l = v;
    }
}

static uint64_t srab_read(void *opaque, hwaddr addr, unsigned size)
{
    BCM53012State *sw = opaque;

    switch (addr) {
    case SRAB_CMDSTAT:
        return sw->cmdstat;
    case SRAB_WD_H:
        return sw->wd_h;
    case SRAB_WD_L:
        return sw->wd_l;
    case SRAB_RD_H:
        return sw->rd_h;
    case SRAB_RD_L:
        return sw->rd_l;
    case SRAB_CTRLS:
        return (sw->ctrls & ~(CTRLS_RCAGNT | CTRLS_SW_INIT_DONE)) | CTRLS_SW_INIT_DONE |
               ((sw->ctrls & CTRLS_RCAREQ) ? CTRLS_RCAGNT : 0);
    case SRAB_INTR:
        return 0;
    }
    qemu_log_mask(LOG_UNIMP, "bcm53012: SRAB read 0x%" HWADDR_PRIx "\n", addr);
    return 0;
}

static void srab_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    BCM53012State *sw = opaque;

    switch (addr) {
    case SRAB_CMDSTAT:
        if (val & CMDSTAT_GORDYN) {
            srab_exec(sw, val);
        }
        sw->cmdstat = val & ~(CMDSTAT_GORDYN | CMDSTAT_RST);
        break;
    case SRAB_WD_H:
        sw->wd_h = val;
        break;
    case SRAB_WD_L:
        sw->wd_l = val;
        break;
    case SRAB_CTRLS:
        sw->ctrls = val;
        break;
    case SRAB_INTR:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "bcm53012: SRAB write 0x%" HWADDR_PRIx "\n", addr);
    }
}

static const MemoryRegionOps srab_ops = {
    .read = srab_read,
    .write = srab_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static bool sw_can_receive(NetClientState *nc)
{
    return true;                    /* frames with nowhere to go are dropped */
}

static ssize_t sw_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    BCM53012Port *pp = qemu_get_nic_opaque(nc);

    bcm53012_ingress(pp->sw, pp->port, buf, size);
    return size;
}

static NetClientInfo sw_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = sw_can_receive,
    .receive = sw_receive,
};

static void sw_realize(DeviceState *dev, Error **errp)
{
    BCM53012State *sw = BCM53012(dev);

    for (int p = 0; p < BCM53012_NEXT; p++) {
        BCM53012Port *pp = &sw->ext[p];
        g_autofree char *id = g_strdup_printf("%s.p%d", dev->id ?: TYPE_BCM53012, p);

        pp->sw = sw;
        pp->port = p;
        if (!pp->conf.peers.ncs[0]) {
            continue;
        }
        qemu_macaddr_default_if_unset(&pp->conf.macaddr);
        pp->nic = qemu_new_nic(&sw_net_info, &pp->conf, TYPE_BCM53012, id,
                               &dev->mem_reentrancy_guard, pp);
    }
}

static void sw_reset(DeviceState *dev)
{
    BCM53012State *sw = BCM53012(dev);

    sw->cmdstat = sw->wd_h = sw->wd_l = sw->rd_h = sw->rd_l = sw->ctrls = 0;
    memset(sw->regs, 0, sizeof(sw->regs));
    /*
     * Reset values the vendor robo driver read-modify-writes, worked back from
     * a unit's registers after boot: switch mode bit 2; IMP (0e) and port 5/7
     * (5d/5f) overrides "default(2 << 2) SPEED" and "default(1 << 1)
     * DUPLX_MODE" as bcmrobo.c says; 802.1Q control 0, 1 and 6.
     */
    sw->regs[R(0x00, 0x0b)] = 0x04;
    sw->regs[R(0x00, 0x0e)] = 0x0a;
    sw->regs[R(0x00, 0x5d)] = 0x0a;
    sw->regs[R(0x00, 0x5f)] = 0x0a;
    sw->regs[R(0x34, 0x00)] = 0x03;
    sw->regs[R(0x34, 0x01)] = 0x02;
    sw->regs[R(0x34, 0x06)] = 0x10;
    for (int p = 0; p < BCM53012_NEXT; p++) {
        sw->regs[R(PAGE_GPHY(p), 0x00)] = BMCR_DEFAULT;
        sw->regs[R(PAGE_GPHY(p), 0x08)] = GPHY_ANAR;
    }
    memset(sw->vtable, 0, sizeof(sw->vtable));
    memset(sw->arl, 0, sizeof(sw->arl));
}

static int sw_post_load(void *opaque, int version_id)
{
    BCM53012State *sw = opaque;

    memset(sw->arl, 0, sizeof(sw->arl));    /* not migrated: relearned from traffic */
    return 0;
}

static void sw_init(Object *obj)
{
    BCM53012State *sw = BCM53012(obj);

    memory_region_init_io(&sw->iomem, obj, &srab_ops, sw, "bcm53012-srab", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &sw->iomem);
}

static const Property sw_properties[] = {
    DEFINE_PROP_NETDEV("netdev-p0", BCM53012State, ext[0].conf.peers),
    DEFINE_PROP_NETDEV("netdev-p1", BCM53012State, ext[1].conf.peers),
    DEFINE_PROP_NETDEV("netdev-p2", BCM53012State, ext[2].conf.peers),
    DEFINE_PROP_NETDEV("netdev-p3", BCM53012State, ext[3].conf.peers),
    DEFINE_PROP_NETDEV("netdev-p4", BCM53012State, ext[4].conf.peers),
};

static const VMStateDescription vmstate_sw = {
    .name = TYPE_BCM53012,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = sw_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cmdstat, BCM53012State),
        VMSTATE_UINT32(wd_h, BCM53012State),
        VMSTATE_UINT32(wd_l, BCM53012State),
        VMSTATE_UINT32(rd_h, BCM53012State),
        VMSTATE_UINT32(rd_l, BCM53012State),
        VMSTATE_UINT32(ctrls, BCM53012State),
        VMSTATE_UINT64_ARRAY(regs, BCM53012State, 256 * 256),
        VMSTATE_UINT32_ARRAY(vtable, BCM53012State, 4096),
        VMSTATE_END_OF_LIST()
    },
};

static void sw_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = sw_realize;
    dc->vmsd = &vmstate_sw;
    device_class_set_legacy_reset(dc, sw_reset);
    device_class_set_props(dc, sw_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo bcm_ns_eth_types[] = {
    {
        .name          = TYPE_BCM_NS_GMAC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(BCMNSGmacState),
        .instance_init = gmac_init,
        .class_init    = gmac_class_init,
    }, {
        .name          = TYPE_BCM53012,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(BCM53012State),
        .instance_init = sw_init,
        .class_init    = sw_class_init,
    },
};

DEFINE_TYPES(bcm_ns_eth_types)
