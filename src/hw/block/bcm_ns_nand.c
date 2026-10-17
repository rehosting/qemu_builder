/*
 * Broadcom iProc / Northstar NAND controller ("nandcore", brcmnand v6.x)
 * with its IDM wrapper page and the 16 MiB direct-read window, backed by a
 * raw NAND image (per page: data followed by OOB, as nanddump -o writes it).
 *
 * Register contract: include/nand_core.h and shared/nandcore.c from the
 * vendor GPL release (the vendor kernel drives this block through nflash ->
 * hndnand -> nandcore, and through brcmnand -> nand_base).
 *
 *   - Commands complete synchronously: intfc_status reads CTRL_READY |
 *     FLASH_READY after every command.
 *   - The controller moves one 512-byte "cache" sector per command plus its
 *     spare bytes (acc_control spare-per-sector, 16 on this board).
 *   - Data registers (flash_cache, spare areas, device id) present bytes
 *     big-endian unless the wrapper's ioctrl NAND_APB_LITTLE_ENDIAN (bit 24) is
 *     set; nandcore flips it around every copy.
 *   - Program follows NAND semantics (bits only go 1 -> 0); erase sets 0xff.
 *   - ECC: no bit errors are ever injected, and ECC bytes are not generated
 *     into the OOB (caveat: OOB ECC positions read back as written).
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/block/block.h"
#include "system/block-backend.h"
#include "migration/vmstate.h"
#include "migration/blocker.h"
#include "qemu/error-report.h"
#include "system/runstate.h"
#include "qom/object.h"
#include "hw/misc/bcm_ns.h"

#define TYPE_BCM_NS_NAND "bcm-ns-nand"
OBJECT_DECLARE_SIMPLE_TYPE(BCMNSNandState, BCM_NS_NAND)

#define SECTOR          512
#define SPARE_REGS      16          /* spare_area_{read,write}_ofs[16] */
#define WINDOW_SIZE     (16 * MiB)

enum {
    R_REVISION        = 0x000,
    R_CMD_START       = 0x004,
    R_CMD_EXT_ADDRESS = 0x008,
    R_CMD_ADDRESS     = 0x00c,
    R_CMD_END_ADDRESS = 0x010,
    R_INTFC_STATUS    = 0x014,
    R_CS_NAND_SELECT  = 0x018,
    R_ACC_CONTROL_CS0 = 0x050,
    R_CONFIG_CS0      = 0x054,
    R_TIMING_1_CS0    = 0x058,
    R_TIMING_2_CS0    = 0x05c,
    R_ACC_CONTROL_CS1 = 0x060,
    R_CONFIG_CS1      = 0x064,
    R_TIMING_1_CS1    = 0x068,
    R_TIMING_2_CS1    = 0x06c,
    R_UNCORR_ERRCNT   = 0x0fc,
    R_CORR_ERRCNT     = 0x100,
    R_READ_ERRCNT     = 0x104,
    R_DEVICE_ID       = 0x194,
    R_DEVICE_ID_EXT   = 0x198,
    R_SPARE_RD        = 0x200,
    R_SPARE_WR        = 0x280,
    R_FLASH_CACHE     = 0x400,
};

enum {
    CMD_PAGE_RD     = 0x01,
    CMD_SPARE_RD    = 0x02,
    CMD_STATUS_RD   = 0x03,
    CMD_PAGE_PROG   = 0x04,
    CMD_SPARE_PROG  = 0x05,
    CMD_ID_RD       = 0x07,
    CMD_BLOCK_ERASE = 0x08,
    CMD_FLASH_RESET = 0x09,
};

#define IST_CTRL_READY   0x80000000u
#define IST_FLASH_READY  0x40000000u
#define IST_CACHE_VALID  0x20000000u
#define IST_SPARE_VALID  0x10000000u
#define IST_ERASED       0x08000000u
#define CSEL_NAND_WP     0x20000000u
#define APB_LITTLE_END   0x01000000u   /* wrapper ioctrl */
#define RO_CTRL_READY    0x00000001u   /* wrapper iostatus */

/* NAND status byte */
#define NST_FAIL   0x01
#define NST_ARDY   0x20
#define NST_RDY    0x40
#define NST_NWP    0x80

struct BCMNSNandState {
    SysBusDevice parent_obj;
    MemoryRegion regs, wrap, window;
    BlockBackend *blk;
    Error *migration_blocker;
    VMChangeStateEntry *reload;     /* pending post-loadvm re-read */
    uint8_t *storage;               /* pages * (page + oob) */

    /* properties: geometry and what CFE left programmed */
    uint32_t page_size, oob_size, pages_per_block, num_blocks;
    uint32_t revision, config_cs0, acc_control_cs0;
    uint64_t device_id;             /* up to 8 ID bytes, byte 0 in bits 7:0 */

    /* registers */
    uint32_t cmd_ext_address, cmd_address, cmd_end_address;
    uint32_t intfc_status, cs_nand_select;
    uint32_t acc_control[2], config[2], timing1[2], timing2[2];
    uint32_t id_latched, id_ext_latched;
    uint8_t  cache[SECTOR];
    uint8_t  spare_rd[SPARE_REGS * 4];
    uint8_t  spare_wr[SPARE_REGS * 4];
    uint32_t wrap_ioctrl;
};

static uint32_t nand_raw_page(BCMNSNandState *s)
{
    return s->page_size + s->oob_size;
}

static uint64_t nand_size(BCMNSNandState *s)
{
    return (uint64_t)s->page_size * s->pages_per_block * s->num_blocks;
}

static uint32_t spare_per_sector(BCMNSNandState *s)
{
    return MIN(s->acc_control[0] & 0x7f, s->oob_size / (s->page_size / SECTOR));
}

static uint8_t *page_data(BCMNSNandState *s, uint64_t page)
{
    return s->storage + page * nand_raw_page(s);
}

bool bcm_ns_nand_read(DeviceState *dev, uint64_t off, void *buf, size_t len)
{
    BCMNSNandState *s = BCM_NS_NAND(dev);
    uint8_t *out = buf;

    if (off > nand_size(s) || len > nand_size(s) - off) {
        return false;
    }
    while (len) {
        uint64_t page = off / s->page_size;
        uint32_t in = off % s->page_size;
        size_t n = MIN(len, s->page_size - in);

        memcpy(out, page_data(s, page) + in, n);
        out += n;
        off += n;
        len -= n;
    }
    return true;
}

static void nand_flush_page(BCMNSNandState *s, uint64_t page)
{
    if (s->blk && blk_is_writable(s->blk)) {
        uint32_t raw = nand_raw_page(s);
        blk_pwrite(s->blk, page * raw, raw, page_data(s, page), 0);
    }
}

static bool all_ff(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (b[i] != 0xff) {
            return false;
        }
    }
    return true;
}

/* Byte <-> register order according to the wrapper's APB endianness bit. */
static uint32_t pack(BCMNSNandState *s, const uint8_t *b)
{
    return (s->wrap_ioctrl & APB_LITTLE_END) ? ldl_le_p(b) : ldl_be_p(b);
}

static void unpack(BCMNSNandState *s, uint8_t *b, uint32_t v)
{
    if (s->wrap_ioctrl & APB_LITTLE_END) {
        stl_le_p(b, v);
    } else {
        stl_be_p(b, v);
    }
}

static bool nand_locate(BCMNSNandState *s, uint64_t *page, uint32_t *sector)
{
    uint64_t addr = ((uint64_t)(s->cmd_ext_address & 0xffff) << 32) | s->cmd_address;

    if (addr >= nand_size(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "bcm-ns-nand: address 0x%" PRIx64
                      " beyond device\n", addr);
        return false;
    }
    *page = addr / s->page_size;
    *sector = (addr % s->page_size) / SECTOR;
    return true;
}

static void nand_cmd(BCMNSNandState *s, uint32_t op)
{
    uint64_t page;
    uint32_t sector, sps = spare_per_sector(s);
    bool wp = s->cs_nand_select & CSEL_NAND_WP;
    uint8_t status = NST_RDY | NST_ARDY | (wp ? 0 : NST_NWP);
    uint8_t *p;

    s->intfc_status &= ~(IST_CACHE_VALID | IST_SPARE_VALID | IST_ERASED | 0xff);

    switch (op) {
    case CMD_PAGE_RD:
    case CMD_SPARE_RD:
        if (!nand_locate(s, &page, &sector)) {
            status |= NST_FAIL;
            break;
        }
        p = page_data(s, page);
        memset(s->spare_rd, 0xff, sizeof(s->spare_rd));
        memcpy(s->spare_rd, p + s->page_size + sector * sps, sps);
        if (op == CMD_PAGE_RD) {
            memcpy(s->cache, p + sector * SECTOR, SECTOR);
            s->intfc_status |= IST_CACHE_VALID;
            if (all_ff(s->cache, SECTOR)) {
                s->intfc_status |= IST_ERASED;
            }
        }
        s->intfc_status |= IST_SPARE_VALID;
        break;

    case CMD_PAGE_PROG:
    case CMD_SPARE_PROG:
        if (wp || !nand_locate(s, &page, &sector)) {
            status |= NST_FAIL;
            break;
        }
        p = page_data(s, page);
        if (op == CMD_PAGE_PROG) {
            for (int i = 0; i < SECTOR; i++) {
                p[sector * SECTOR + i] &= s->cache[i];
            }
        }
        for (uint32_t i = 0; i < sps; i++) {
            p[s->page_size + sector * sps + i] &= s->spare_wr[i];
        }
        nand_flush_page(s, page);
        break;

    case CMD_BLOCK_ERASE: {
        uint64_t first;
        if (wp || !nand_locate(s, &page, &sector)) {
            status |= NST_FAIL;
            break;
        }
        first = page - page % s->pages_per_block;
        memset(page_data(s, first), 0xff,
               (size_t)nand_raw_page(s) * s->pages_per_block);
        for (uint32_t i = 0; i < s->pages_per_block; i++) {
            nand_flush_page(s, first + i);
        }
        break;
    }

    case CMD_ID_RD:
    case CMD_FLASH_RESET: {
        uint8_t id[8];
        stq_le_p(id, s->device_id);
        s->id_latched = ldl_be_p(id);           /* byte 0 in bits 31:24 */
        s->id_ext_latched = ldl_be_p(id + 4);
        break;
    }

    case CMD_STATUS_RD:
        break;

    default:
        qemu_log_mask(LOG_UNIMP, "bcm-ns-nand: unimplemented command 0x%02x\n", op);
        break;
    }

    s->intfc_status |= IST_CTRL_READY | IST_FLASH_READY | status;
}

static uint32_t id_reg(BCMNSNandState *s, uint32_t latched)
{
    uint8_t b[4];

    stl_be_p(b, latched);
    return pack(s, b);
}

static uint64_t nand_read(void *opaque, hwaddr addr, unsigned size)
{
    BCMNSNandState *s = opaque;

    if (addr >= R_FLASH_CACHE && addr < R_FLASH_CACHE + SECTOR) {
        return pack(s, s->cache + (addr - R_FLASH_CACHE));
    }
    if (addr >= R_SPARE_RD && addr < R_SPARE_RD + SPARE_REGS * 4) {
        return pack(s, s->spare_rd + (addr - R_SPARE_RD));
    }
    if (addr >= R_SPARE_WR && addr < R_SPARE_WR + SPARE_REGS * 4) {
        return pack(s, s->spare_wr + (addr - R_SPARE_WR));
    }
    switch (addr) {
    case R_REVISION:        return s->revision;
    case R_CMD_EXT_ADDRESS: return s->cmd_ext_address;
    case R_CMD_ADDRESS:     return s->cmd_address;
    case R_CMD_END_ADDRESS: return s->cmd_end_address;
    case R_INTFC_STATUS:    return s->intfc_status;
    case R_CS_NAND_SELECT:  return s->cs_nand_select;
    case R_ACC_CONTROL_CS0: return s->acc_control[0];
    case R_CONFIG_CS0:      return s->config[0];
    case R_TIMING_1_CS0:    return s->timing1[0];
    case R_TIMING_2_CS0:    return s->timing2[0];
    case R_ACC_CONTROL_CS1: return s->acc_control[1];
    case R_CONFIG_CS1:      return s->config[1];
    case R_TIMING_1_CS1:    return s->timing1[1];
    case R_TIMING_2_CS1:    return s->timing2[1];
    case R_UNCORR_ERRCNT:
    case R_CORR_ERRCNT:
    case R_READ_ERRCNT:     return 0;
    case R_DEVICE_ID:       return id_reg(s, s->id_latched);
    case R_DEVICE_ID_EXT:   return id_reg(s, s->id_ext_latched);
    default:
        qemu_log_mask(LOG_UNIMP, "bcm-ns-nand: read 0x%03" HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void nand_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    BCMNSNandState *s = opaque;

    if (addr >= R_FLASH_CACHE && addr < R_FLASH_CACHE + SECTOR) {
        unpack(s, s->cache + (addr - R_FLASH_CACHE), val);
        return;
    }
    if (addr >= R_SPARE_WR && addr < R_SPARE_WR + SPARE_REGS * 4) {
        unpack(s, s->spare_wr + (addr - R_SPARE_WR), val);
        return;
    }
    switch (addr) {
    case R_CMD_START:       nand_cmd(s, (val >> 24) & 0x1f); break;
    case R_CMD_EXT_ADDRESS: s->cmd_ext_address = val; break;
    case R_CMD_ADDRESS:     s->cmd_address = val; break;
    case R_CMD_END_ADDRESS: s->cmd_end_address = val; break;
    case R_CS_NAND_SELECT:  s->cs_nand_select = val; break;
    case R_ACC_CONTROL_CS0: s->acc_control[0] = val; break;
    case R_CONFIG_CS0:      s->config[0] = val; break;
    case R_TIMING_1_CS0:    s->timing1[0] = val; break;
    case R_TIMING_2_CS0:    s->timing2[0] = val; break;
    case R_ACC_CONTROL_CS1: s->acc_control[1] = val; break;
    case R_CONFIG_CS1:      s->config[1] = val; break;
    case R_TIMING_1_CS1:    s->timing1[1] = val; break;
    case R_TIMING_2_CS1:    s->timing2[1] = val; break;
    default:
        qemu_log_mask(LOG_UNIMP, "bcm-ns-nand: write 0x%03" HWADDR_PRIx
                      " = 0x%08" PRIx64 "\n", addr, val);
        break;
    }
}

static const MemoryRegionOps nand_ops = {
    .read = nand_read,
    .write = nand_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* IDM wrapper page of the NAND core (0x1811a000). */
static uint64_t wrap_read(void *opaque, hwaddr addr, unsigned size)
{
    BCMNSNandState *s = opaque;

    switch (addr) {
    case 0x408: return s->wrap_ioctrl;
    case 0x500: return RO_CTRL_READY;
    default:    return 0;
    }
}

static void wrap_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    BCMNSNandState *s = opaque;

    switch (addr) {
    case 0x400: s->wrap_ioctrl |= val; break;
    case 0x404: s->wrap_ioctrl &= ~val; break;
    case 0x408: s->wrap_ioctrl = val; break;
    default:    break;
    }
}

static const MemoryRegionOps wrap_ops = {
    .read = wrap_read,
    .write = wrap_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* Direct-read window: linear main-area bytes, no OOB. */
static uint64_t window_read(void *opaque, hwaddr addr, unsigned size)
{
    BCMNSNandState *s = opaque;
    uint64_t val = 0;

    for (unsigned i = 0; i < size; i++) {
        uint64_t a = addr + i;
        uint8_t b = 0xff;
        if (a < nand_size(s)) {
            b = page_data(s, a / s->page_size)[a % s->page_size];
        }
        val |= (uint64_t)b << (8 * i);
    }
    return val;
}

static void window_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR, "bcm-ns-nand: write to read-only window\n");
}

static const MemoryRegionOps window_ops = {
    .read = window_read,
    .write = window_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void nand_reset(DeviceState *dev)
{
    BCMNSNandState *s = BCM_NS_NAND(dev);
    uint8_t id[8];

    /* As CFE leaves it: configured, ID latched, WP asserted. */
    s->cmd_ext_address = s->cmd_address = s->cmd_end_address = 0;
    s->intfc_status = IST_CTRL_READY | IST_FLASH_READY | NST_RDY | NST_ARDY;
    s->cs_nand_select = CSEL_NAND_WP;
    s->acc_control[0] = s->acc_control_cs0;
    s->config[0] = s->config_cs0;
    s->acc_control[1] = s->config[1] = 0;
    s->timing1[0] = s->timing2[0] = s->timing1[1] = s->timing2[1] = 0;
    stq_le_p(id, s->device_id);
    s->id_latched = ldl_be_p(id);
    s->id_ext_latched = ldl_be_p(id + 4);
    memset(s->cache, 0xff, sizeof(s->cache));
    memset(s->spare_rd, 0xff, sizeof(s->spare_rd));
    memset(s->spare_wr, 0xff, sizeof(s->spare_wr));
    s->wrap_ioctrl = 0;
}

static void nand_realize(DeviceState *dev, Error **errp)
{
    BCMNSNandState *s = BCM_NS_NAND(dev);
    uint64_t raw = (uint64_t)nand_raw_page(s) * s->pages_per_block * s->num_blocks;

    if (s->page_size % SECTOR || !s->pages_per_block || !s->num_blocks) {
        error_setg(errp, "bad NAND geometry");
        return;
    }
    if (s->blk) {
        uint64_t perm = BLK_PERM_CONSISTENT_READ |
                        (blk_supports_write_perm(s->blk) ? BLK_PERM_WRITE : 0);
        if (blk_set_perm(s->blk, perm, BLK_PERM_ALL, errp) < 0) {
            return;
        }
        s->storage = blk_blockalign(s->blk, raw);
        if (!blk_check_size_and_read_all(s->blk, dev, s->storage, raw, errp)) {
            return;
        }
    } else {
        /* Flash with no backing drive lives only in this buffer, which is not
         * migrated: refuse savevm/migration rather than lose it silently. */
        s->storage = blk_blockalign(NULL, raw);
        memset(s->storage, 0xff, raw);
        error_setg(&s->migration_blocker,
                   "bcm-ns-nand: flash has no backing drive (use -drive if=mtd)");
        if (migrate_add_blocker(&s->migration_blocker, errp) < 0) {
            return;
        }
    }

    memory_region_init_io(&s->regs, OBJECT(s), &nand_ops, s, "bcm-ns.nand", 0x1000);
    memory_region_init_io(&s->wrap, OBJECT(s), &wrap_ops, s, "bcm-ns.nand-idm", 0x1000);
    memory_region_init_io(&s->window, OBJECT(s), &window_ops, s, "bcm-ns.nand-window",
                          WINDOW_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->regs);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->wrap);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->window);
}

static const Property nand_properties[] = {
    DEFINE_PROP_DRIVE("drive", BCMNSNandState, blk),
    DEFINE_PROP_UINT32("page-size", BCMNSNandState, page_size, 2048),
    DEFINE_PROP_UINT32("oob-size", BCMNSNandState, oob_size, 64),
    DEFINE_PROP_UINT32("pages-per-block", BCMNSNandState, pages_per_block, 64),
    DEFINE_PROP_UINT32("num-blocks", BCMNSNandState, num_blocks, 1024),
    DEFINE_PROP_UINT64("device-id", BCMNSNandState, device_id, 0),
    DEFINE_PROP_UINT32("revision", BCMNSNandState, revision, 0),
    DEFINE_PROP_UINT32("config-cs0", BCMNSNandState, config_cs0, 0),
    DEFINE_PROP_UINT32("acc-control-cs0", BCMNSNandState, acc_control_cs0, 0),
};

/*
 * Flash contents are not in the migration stream: the drive is. Every program
 * and erase is written through to it, so once loadvm has reverted the (qcow2)
 * drive to the snapshot, or an incoming migration has the same drive state,
 * re-reading the drive restores the array exactly. The read cannot happen in
 * post_load: loadvm holds all block devices drained there, and a request would
 * wait on that drain forever. Defer it to the run-state change that follows,
 * as pflash does (the VM is stopped until then, so nothing reads the array).
 */
static void nand_reload_cb(void *opaque, bool running, RunState state)
{
    BCMNSNandState *s = opaque;
    uint64_t raw = (uint64_t)nand_raw_page(s) * s->pages_per_block * s->num_blocks;

    if (!running) {
        return;
    }
    if (blk_pread(s->blk, 0, raw, s->storage, 0) < 0) {
        error_report("bcm-ns-nand: re-reading flash after loadvm failed");
    }
    qemu_del_vm_change_state_handler(s->reload);
    s->reload = NULL;
}

static int nand_post_load(void *opaque, int version_id)
{
    BCMNSNandState *s = opaque;

    if (s->blk && !s->reload) {
        s->reload = qemu_add_vm_change_state_handler(nand_reload_cb, s);
    }
    return 0;
}

static const VMStateDescription vmstate_nand = {
    .name = TYPE_BCM_NS_NAND,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = nand_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cmd_ext_address, BCMNSNandState),
        VMSTATE_UINT32(cmd_address, BCMNSNandState),
        VMSTATE_UINT32(cmd_end_address, BCMNSNandState),
        VMSTATE_UINT32(intfc_status, BCMNSNandState),
        VMSTATE_UINT32(cs_nand_select, BCMNSNandState),
        VMSTATE_UINT32_ARRAY(acc_control, BCMNSNandState, 2),
        VMSTATE_UINT32_ARRAY(config, BCMNSNandState, 2),
        VMSTATE_UINT32_ARRAY(timing1, BCMNSNandState, 2),
        VMSTATE_UINT32_ARRAY(timing2, BCMNSNandState, 2),
        VMSTATE_UINT32(id_latched, BCMNSNandState),
        VMSTATE_UINT32(id_ext_latched, BCMNSNandState),
        VMSTATE_UINT8_ARRAY(cache, BCMNSNandState, SECTOR),
        VMSTATE_UINT8_ARRAY(spare_rd, BCMNSNandState, SPARE_REGS * 4),
        VMSTATE_UINT8_ARRAY(spare_wr, BCMNSNandState, SPARE_REGS * 4),
        VMSTATE_UINT32(wrap_ioctrl, BCMNSNandState),
        VMSTATE_END_OF_LIST()
    },
};

static void nand_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = nand_realize;
    dc->vmsd = &vmstate_nand;
    device_class_set_legacy_reset(dc, nand_reset);
    device_class_set_props(dc, nand_properties);
}

static const TypeInfo nand_info = {
    .name          = TYPE_BCM_NS_NAND,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCMNSNandState),
    .class_init    = nand_class_init,
};

static void nand_register_types(void)
{
    type_register_static(&nand_info);
}

type_init(nand_register_types)
