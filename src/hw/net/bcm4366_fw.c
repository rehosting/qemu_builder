/*
 * BCM4366 full-dongle firmware stand-in: the image's half of the PCIe msgbuf
 * protocol, without running the image.
 *
 * When the host releases the ARM after a download, this does what the
 * dongle image's start-up does as far as the host can see:
 *   - parse the nvram the host left at the top of RAM (its length token is
 *     the last word);
 *   - lay out pcie_shared (version 5; DMA'd 2-byte ring indices; D2H SEQNUM
 *     sync, as a unit's dhd reports), ring_info, the common rings' ring_mem,
 *     the TCM index arrays, the mailbox words and the console (hnd_cons_t,
 *     with the start-up lines a unit's firmware prints) below it;
 *   - write pcie_shared's address into the last word of RAM.
 * Then, on every H2D doorbell, it consumes the control submit ring, the RX
 * post ring and the TX flow rings, and answers on the three completion
 * rings:
 *   - IOCTLPTR_REQ: ack, relay the ioctl to the wlsim responder as one
 *     wlshim IOCTL (hw/misc/wlshim_proto.h), complete into a posted
 *     IOCTLRESP buffer;
 *   - FLOW_RING_CREATE/DELETE/FLUSH: keep the flow ring, complete;
 *   - TX_POST: the frame (header from the item, body by DMA) goes to the
 *     responder as a wlshim TX, then TX_STATUS;
 *   - the responder's queued frames (wlshim POLL, every 20 ms while up):
 *     Broadcom event frames go up as WL_EVENT into posted event buffers,
 *     data as RX_CMPLT into posted RX buffers.
 * Message layouts are the router DHD's (bcmmsgbuf.h, bcmpcie.h of the 7.14
 * SDK), with the shared-area and SEQNUM details checked against the
 * shipped host driver (DHD 1.363): SEQNUM is the last 32-bit word of every completion,
 * (254 + n) % 253 for the n-th item of a ring.
 *
 * Without a responder (no wl= socket, or it is not up), ioctls get the
 * dongle's BCME_UNSUPPORTED, except the version and magic queries and
 * cur_etheraddr (from the downloaded nvram's macaddr), so the host still
 * attaches.
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/sockets.h"
#include "qemu/cutils.h"
#include "migration/vmstate.h"
#include "hw/net/bcm4366.h"
#include "hw/misc/wlshim_proto.h"
#include <sys/socket.h>
#include <sys/un.h>

/* pcie_shared flags */
#define SH_VERSION          5
#define SH_DMA_INDEX        0x00010000
#define SH_D2H_SYNC_SEQNUM  0x00020000
#define SH_2BYTE_INDICES    0x00100000
/* pcie_shared offsets (80 bytes) */
#define SH_FLAGS            0x00
#define SH_CONSOLE          0x14
#define SH_FWID             0x1c
#define SH_LFRAG            0x20
#define SH_MAXRXBUFS        0x22
#define SH_RXOFFSET         0x24
#define SH_H2D_MB           0x28
#define SH_D2H_MB           0x2c
#define SH_RINGS_INFO       0x30
#define SH_SIZE             0x50
/* ring_info offsets (56 bytes) */
#define RI_RINGMEM          0x00
#define RI_H2D_W            0x04
#define RI_H2D_R            0x08
#define RI_D2H_W            0x0c
#define RI_D2H_R            0x10
#define RI_H2D_W_HOST       0x14    /* the 1.363 order, the host driver's; */
#define RI_H2D_R_HOST       0x1c    /* the 7.14 SDK header lists D2H first */
#define RI_D2H_W_HOST       0x24
#define RI_D2H_R_HOST       0x2c
#define RI_MAXSUBQ          0x34
#define RI_SIZE             0x38
/* ring_mem (16 bytes each) */
#define RM_MAXITEM          4
#define RM_LENITEM          6
#define RM_BASE             8
#define RM_SIZE             16

#define NCOMMON             5   /* ctrl submit, rx post, ctrl/tx/rx complete */
#define D2H_EPOCH_MODULO    253
#define D2H_EPOCH_INIT      (D2H_EPOCH_MODULO + 1)

enum {
    MSG_GEN_STATUS = 0x1, MSG_RING_STATUS = 0x2,
    MSG_FLOW_CREATE = 0x3, MSG_FLOW_CREATE_CMPLT = 0x4,
    MSG_FLOW_DELETE = 0x5, MSG_FLOW_DELETE_CMPLT = 0x6,
    MSG_FLOW_FLUSH = 0x7, MSG_FLOW_FLUSH_CMPLT = 0x8,
    MSG_IOCTLPTR_REQ = 0x9, MSG_IOCTLPTR_REQ_ACK = 0xa,
    MSG_IOCTLRESP_BUF_POST = 0xb, MSG_IOCTL_CMPLT = 0xc,
    MSG_EVENT_BUF_POST = 0xd, MSG_WL_EVENT = 0xe,
    MSG_TX_POST = 0xf, MSG_TX_STATUS = 0x10,
    MSG_RXBUF_POST = 0x11, MSG_RX_CMPLT = 0x12,
};

enum { D2H_CTRL, D2H_TX, D2H_RX };

#define BCME_ERROR          (-1)
#define BCME_UNSUPPORTED    (-23)
#define WLC_GET_MAGIC       0
#define WLC_GET_VERSION     1
#define WLC_GET_VAR         262
#define WLC_IOCTL_MAGIC     0x14e46c77
#define WLC_IOCTL_VERSION   2
#define ETHER_TYPE_BRCM     0x886c

#define FW_BOOT_NS          (40 * SCALE_MS)
#define FW_POLL_NS          (20 * SCALE_MS)
#define FW_POLL_SOON_NS     (2 * SCALE_MS)
#define CONS_BUF_SIZE       4096

/* ---- small helpers --------------------------------------------------------- */

static uint32_t tcm32(BCM4366State *s, uint32_t a)
{
    uint32_t v;

    bcm4366_tcm_read(s, a, &v, 4);
    return le32_to_cpu(v);
}

static uint16_t tcm16(BCM4366State *s, uint32_t a)
{
    uint16_t v;

    bcm4366_tcm_read(s, a, &v, 2);
    return le16_to_cpu(v);
}

static uint64_t tcm64(BCM4366State *s, uint32_t a)
{
    return tcm32(s, a) | ((uint64_t)tcm32(s, a + 4) << 32);
}

static void tcm_st32(BCM4366State *s, uint32_t a, uint32_t v)
{
    v = cpu_to_le32(v);
    bcm4366_tcm_write(s, a, &v, 4);
}

static void tcm_st16(BCM4366State *s, uint32_t a, uint16_t v)
{
    v = cpu_to_le16(v);
    bcm4366_tcm_write(s, a, &v, 2);
}

/*
 * Index arrays: in host memory once the host has given their addresses
 * (DMA'd indices). The TCM copies are kept current as well, as a unit's
 * firmware does: the host's "dhd dump" reads its ring counters from there.
 */
static uint16_t idx_get(BCM4366State *s, uint32_t hostoff, uint32_t tcmbase, int i)
{
    uint64_t h = tcm64(s, s->fw.ring_info + hostoff);
    uint16_t v = 0;

    if (h) {
        bcm4366_dma_read(s, h + 2 * i, &v, 2);
        v = le16_to_cpu(v);
        tcm_st16(s, tcmbase + 2 * i, v);
        return v;
    }
    return tcm16(s, tcmbase + 2 * i);
}

static void idx_put(BCM4366State *s, uint32_t hostoff, uint32_t tcmbase, int i, uint16_t v)
{
    uint64_t h = tcm64(s, s->fw.ring_info + hostoff);

    if (h) {
        uint16_t le = cpu_to_le16(v);

        bcm4366_dma_write(s, h + 2 * i, &le, 2);
    }
    tcm_st16(s, tcmbase + 2 * i, v);
}

/* ---- console ---------------------------------------------------------------- */

static void cons_puts(BCM4366State *s, const char *line)
{
    BCM4366Fw *f = &s->fw;
    int64_t ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    g_autofree char *t = g_strdup_printf("%06" PRId64 ".%03" PRId64 " %s\n",
                                         (ms / 1000) % 1000000, ms % 1000, line);

    for (char *p = t; *p; p++) {
        bcm4366_tcm_write(s, f->cons_buf + f->cons_idx, p, 1);
        f->cons_idx = (f->cons_idx + 1) % f->cons_size;
    }
    tcm_st32(s, f->cons + 16, f->cons_idx);             /* log.idx */
}

/* ---- the responder (wlsim) ------------------------------------------------ */

static void wl_drop(BCM4366State *s, const char *why)
{
    BCM4366Fw *f = &s->fw;

    if (f->wl_fd >= 0) {
        warn_report("bcm4366[%u]: wl responder %s", s->radio, why);
        close(f->wl_fd);
    }
    f->wl_fd = -1;
    f->wl_said = false;
}

static bool wl_io(int fd, void *buf, size_t n, bool out)
{
    uint8_t *p = buf;

    while (n) {
        ssize_t r = out ? send(fd, p, n, MSG_NOSIGNAL) : recv(fd, p, n, 0);

        if (r <= 0) {
            if (r < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        p += r;
        n -= r;
    }
    return true;
}

static bool wl_call(BCM4366State *s, struct wlshim_hdr *h, const void *pl,
                    void *reply, size_t room);

static void wl_connect(BCM4366State *s)
{
    BCM4366Fw *f = &s->fw;
    struct sockaddr_un sun = { .sun_family = AF_UNIX };
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    struct timeval tv = { .tv_sec = 5 };
    int fd;

    if (!s->wl || f->wl_fd >= 0 || now < f->wl_retry) {
        return;
    }
    f->wl_retry = now + 1000;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return;
    }
    pstrcpy(sun.sun_path, sizeof(sun.sun_path), s->wl);
    if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
        close(fd);
        return;
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    f->wl_fd = fd;
    {
        struct wlshim_hdr h = { .op = WLSHIM_HELLO, .cmd = WLSHIM_HELLO_QUERY, .len = 4 };
        uint32_t ver = cpu_to_le32(WLSHIM_VERSION);
        uint8_t rep[sizeof(struct wlshim_hello) + 8 * sizeof(struct wlshim_radio)];
        struct wlshim_hello *hello = (void *)rep;

        if (!wl_call(s, &h, &ver, rep, sizeof(rep)) || h.status < 0) {
            wl_drop(s, "refused HELLO");
            return;
        }
        f->nradio = le32_to_cpu(hello->nradio);
        if (s->radio < f->nradio && h.len >= sizeof(*hello) +
            (s->radio + 1) * sizeof(struct wlshim_radio)) {
            struct wlshim_radio *r = (void *)(rep + sizeof(*hello));

            memcpy(f->mac, r[s->radio].mac, 6);
        }
    }
    if (!f->wl_said) {
        f->wl_said = true;
        info_report("bcm4366[%u]: wl responder connected (%s)", s->radio, s->wl);
    }
}

/* One request: h (len = payload bytes) and pl out, h and up to room bytes
 * of reply payload back. */
static bool wl_call(BCM4366State *s, struct wlshim_hdr *h, const void *pl,
                    void *reply, size_t room)
{
    BCM4366Fw *f = &s->fw;
    uint32_t n = cpu_to_le32(sizeof(*h) + h->len), rn;
    g_autofree uint8_t *buf = NULL;

    if (f->wl_fd < 0) {
        return false;
    }
    pstrcpy(h->comm, sizeof(h->comm), "dongle");
    h->radio = s->radio;
    if (!wl_io(f->wl_fd, &n, 4, true) || !wl_io(f->wl_fd, h, sizeof(*h), true) ||
        (h->len && !wl_io(f->wl_fd, (void *)pl, h->len, true)) ||
        !wl_io(f->wl_fd, &rn, 4, false)) {
        wl_drop(s, "went away; reconnecting");
        return false;
    }
    rn = le32_to_cpu(rn);
    if (rn < sizeof(*h) || rn > WLSHIM_BUF + sizeof(*h)) {
        wl_drop(s, "sent a bad reply; reconnecting");
        return false;
    }
    buf = g_malloc(rn);
    if (!wl_io(f->wl_fd, buf, rn, false)) {
        wl_drop(s, "went away; reconnecting");
        return false;
    }
    memcpy(h, buf, sizeof(*h));
    h->len = MIN(h->len, MIN(rn - sizeof(*h), room));
    memcpy(reply, buf + sizeof(*h), h->len);
    return true;
}

/* ---- rings ------------------------------------------------------------------ */

static uint32_t ringmem(BCM4366State *s, int ring)
{
    return s->fw.ring_mem + ring * RM_SIZE;
}

/* Put one completion on D2H ring r; false if the ring is full. */
static bool d2h_put(BCM4366State *s, int r, uint8_t *item)
{
    BCM4366Fw *f = &s->fw;
    uint32_t rm = ringmem(s, 2 + r);
    uint64_t base = tcm64(s, rm + RM_BASE);
    uint16_t max = tcm16(s, rm + RM_MAXITEM), len = tcm16(s, rm + RM_LENITEM);
    uint16_t wr = f->d2h_wr[r], rd = idx_get(s, RI_D2H_R_HOST, f->d2h_r, r);
    uint32_t seq = cpu_to_le32(f->d2h_seq[r] % D2H_EPOCH_MODULO);

    if (!base || !max || len < 16 || len > 64 || (wr + 1) % max == rd) {
        BCM4366_LOG(s, "D2H ring %d: no room (base 0x%" PRIx64 " max %u len %u "
                    "wr %u rd %u)", r, base, max, len, wr, rd);
        return false;
    }
    memcpy(item + (len & ~3) - 4, &seq, 4);
    if (!bcm4366_dma_write(s, base + (uint64_t)wr * len, item, len)) {
        qemu_log_mask(LOG_GUEST_ERROR, "bcm4366[%u]: D2H ring %d DMA failed\n",
                      s->radio, r);
        return false;
    }
    f->d2h_seq[r]++;
    f->d2h_wr[r] = (wr + 1) % max;
    idx_put(s, RI_D2H_W_HOST, f->d2h_w, r, f->d2h_wr[r]);
    return true;
}

static void cmn_hdr(uint8_t *it, uint8_t type, uint8_t ifidx, uint32_t reqid)
{
    it[0] = type;
    it[1] = ifidx;
    stl_le_p(it + 4, reqid);
}

static bool hostbuf_pop(BCM4366HostBuf *v, uint32_t *n, BCM4366HostBuf *out)
{
    if (!*n) {
        return false;
    }
    *out = v[0];
    memmove(v, v + 1, (*n - 1) * sizeof(*v));
    (*n)--;
    return true;
}

static void hostbuf_push(BCM4366State *s, BCM4366HostBuf *v, uint32_t *n,
                         uint32_t max, const uint8_t *it, int lenoff, int addroff)
{
    if (*n >= max) {
        qemu_log_mask(LOG_GUEST_ERROR, "bcm4366[%u]: too many posted buffers "
                      "(type 0x%x)\n", s->radio, it[0]);
        return;
    }
    v[*n].pktid = ldl_le_p(it + 4);
    v[*n].len = lduw_le_p(it + lenoff);
    v[*n].addr = ldq_le_p(it + addroff);
    (*n)++;
}

/* ---- ioctls ------------------------------------------------------------------- */

/* What answers with no responder. */
static int fw_ioctl_local(BCM4366State *s, uint32_t cmd, const uint8_t *in, size_t inlen,
                          uint8_t *out, size_t *outlen)
{
    if (cmd == WLC_GET_MAGIC || cmd == WLC_GET_VERSION) {
        stl_le_p(out, cmd == WLC_GET_MAGIC ? WLC_IOCTL_MAGIC : WLC_IOCTL_VERSION);
        *outlen = 4;
        return 0;
    }
    if (cmd == WLC_GET_VAR && inlen >= 14 && !memcmp(in, "cur_etheraddr", 14)) {
        memcpy(out, s->fw.mac, 6);
        *outlen = 6;
        return 0;
    }
    *outlen = 0;
    return BCME_UNSUPPORTED;
}

static void fw_ioctl(BCM4366State *s, const uint8_t *it)
{
    BCM4366Fw *f = &s->fw;
    uint8_t ifidx = it[1];
    uint32_t reqid = ldl_le_p(it + 4), cmd = ldl_le_p(it + 8);
    uint16_t trans = lduw_le_p(it + 12), inlen = lduw_le_p(it + 14);
    uint16_t outlen = lduw_le_p(it + 16);
    uint64_t inaddr = ldq_le_p(it + 24);
    size_t room = MAX(inlen, outlen), rlen = 0;
    g_autofree uint8_t *in = g_malloc0(MAX(inlen, 1));
    g_autofree uint8_t *out = g_malloc0(MAX(room, 8) + WLSHIM_BUF);
    uint8_t ack[64] = { 0 }, cpl[64] = { 0 };
    BCM4366HostBuf rb;
    int status;

    if (inlen && !bcm4366_dma_read(s, inaddr, in, inlen)) {
        qemu_log_mask(LOG_GUEST_ERROR, "bcm4366[%u]: ioctl buffer DMA failed\n", s->radio);
    }
    cmn_hdr(ack, MSG_IOCTLPTR_REQ_ACK, ifidx, reqid);
    stl_le_p(ack + 12, cmd);
    d2h_put(s, D2H_CTRL, ack);

    wl_connect(s);
    if (f->wl_fd >= 0) {
        struct wlshim_hdr h = {
            .op = WLSHIM_IOCTL, .bss = ifidx, .cmd = cmd, .ulen = room, .len = inlen,
        };

        if (wl_call(s, &h, in, out, MIN(room, (size_t)WLSHIM_BUF))) {
            rlen = h.len;
            /* the BCME code rides in the reply's driver word; older
             * responders only give the errno */
            status = h.status >= 0 ? 0 : (h.driver ? (int32_t)h.driver : BCME_ERROR);
        } else {
            status = fw_ioctl_local(s, cmd, in, inlen, out, &rlen);
        }
    } else {
        status = fw_ioctl_local(s, cmd, in, inlen, out, &rlen);
    }

    BCM4366_LOG(s, "ioctl %u if %u in %u out %u -> %d, %zu bytes", cmd, ifidx, inlen,
                outlen, status, rlen);
    if (!hostbuf_pop(f->iocbuf, &f->niocbuf, &rb)) {
        qemu_log_mask(LOG_GUEST_ERROR, "bcm4366[%u]: no IOCTLRESP buffer for cmd %u\n",
                      s->radio, cmd);
        return;
    }
    rlen = MIN(rlen, MIN((size_t)rb.len, (size_t)outlen));
    if (rlen) {
        bcm4366_dma_write(s, rb.addr, out, rlen);
    }
    cmn_hdr(cpl, MSG_IOCTL_CMPLT, ifidx, rb.pktid);
    stw_le_p(cpl + 8, status);
    stw_le_p(cpl + 12, rlen);
    stw_le_p(cpl + 14, trans);
    stl_le_p(cpl + 16, cmd);
    d2h_put(s, D2H_CTRL, cpl);
}

/* ---- frames to the host -------------------------------------------------------- */

static void pend_add(BCM4366State *s, uint8_t ifidx, const uint8_t *frame, size_t len)
{
    BCM4366Fw *f = &s->fw;
    BCM4366Pending *p;

    if (f->npend >= BCM4366_FW_NPEND || len < 14 || len > 2048) {
        return;
    }
    BCM4366_LOG(s, "from responder: if %u, %zu bytes, type 0x%04x", ifidx, len,
                lduw_be_p(frame + 12));
    p = &f->pend[(f->pend_head + f->npend++) % BCM4366_FW_NPEND];
    p->ifidx = ifidx;
    p->event = lduw_be_p(frame + 12) == ETHER_TYPE_BRCM;
    p->len = len;
    p->data = g_memdup2(frame, len);
}

static bool pend_send(BCM4366State *s, BCM4366Pending *p)
{
    BCM4366Fw *f = &s->fw;
    uint8_t it[64] = { 0 };
    BCM4366HostBuf b;

    if (p->event) {
        /* The firmware's event packets run 2 bytes past the frame (the bcm
         * header's length counts 8 bytes of subtype header, 6 are there);
         * the host driver hands wl_host_event the length less 2 and
         * checks the event data fits in that. */
        static const uint8_t pad[2];

        if (!f->nevbuf || f->evbuf[0].len < p->len + 2) {
            BCM4366_LOG(s, "event waits: %u buffers, first %u bytes, need %u",
                        f->nevbuf, f->nevbuf ? f->evbuf[0].len : 0, p->len + 2);
            return false;
        }
        hostbuf_pop(f->evbuf, &f->nevbuf, &b);
        bcm4366_dma_write(s, b.addr, p->data, p->len);
        bcm4366_dma_write(s, b.addr + p->len, pad, 2);
        cmn_hdr(it, MSG_WL_EVENT, p->ifidx, b.pktid);
        stw_le_p(it + 12, p->len + 2);
        return d2h_put(s, D2H_CTRL, it);
    }
    if (!f->nrxbuf) {
        return false;
    }
    b = f->rxbuf[f->rxbuf_head];
    if (b.len < p->len) {
        return false;
    }
    bcm4366_dma_write(s, b.addr, p->data, p->len);
    cmn_hdr(it, MSG_RX_CMPLT, p->ifidx, b.pktid);
    stw_le_p(it + 10, 1);                       /* from the RX post ring */
    stw_le_p(it + 14, p->len);
    if (!d2h_put(s, D2H_RX, it)) {
        return false;
    }
    f->rxbuf_head = (f->rxbuf_head + 1) % BCM4366_FW_NRXBUF;
    f->nrxbuf--;
    return true;
}

static bool pend_flush(BCM4366State *s)
{
    BCM4366Fw *f = &s->fw;
    bool sent = false;

    while (f->npend) {
        BCM4366Pending *p = &f->pend[f->pend_head];

        if (!pend_send(s, p)) {
            break;
        }
        g_free(p->data);
        p->data = NULL;
        f->pend_head = (f->pend_head + 1) % BCM4366_FW_NPEND;
        f->npend--;
        sent = true;
    }
    return sent;
}

static void pend_clear(BCM4366Fw *f)
{
    for (int i = 0; i < BCM4366_FW_NPEND; i++) {
        g_free(f->pend[i].data);
        f->pend[i].data = NULL;
    }
    f->pend_head = f->npend = 0;
}

/* ---- H2D rings ----------------------------------------------------------------- */

static void fw_ctrl_item(BCM4366State *s, const uint8_t *it)
{
    BCM4366Fw *f = &s->fw;
    uint8_t cpl[64] = { 0 };
    uint16_t flowid;

    BCM4366_LOG(s, "ctrl msg 0x%x if %u req 0x%x", it[0], it[1], ldl_le_p(it + 4));
    switch (it[0]) {
    case MSG_IOCTLPTR_REQ:
        fw_ioctl(s, it);
        break;
    case MSG_IOCTLRESP_BUF_POST:
        hostbuf_push(s, f->iocbuf, &f->niocbuf, BCM4366_FW_NIOCBUF, it, 8, 16);
        break;
    case MSG_EVENT_BUF_POST:
        hostbuf_push(s, f->evbuf, &f->nevbuf, BCM4366_FW_NEVBUF, it, 8, 16);
        break;
    case MSG_FLOW_CREATE:
        flowid = lduw_le_p(it + 22);
        cmn_hdr(cpl, MSG_FLOW_CREATE_CMPLT, it[1], ldl_le_p(it + 4));
        stw_le_p(cpl + 10, flowid);
        if (flowid < 2 || flowid >= BCM4366_FW_NH2D) {
            stw_le_p(cpl + 8, 5);               /* BCMPCIE_RING_ID_INVALID */
        } else {
            f->flow[flowid] = (BCM4366FlowRing) {
                .base = ldq_le_p(it + 32), .max_items = lduw_le_p(it + 28),
                .len_item = lduw_le_p(it + 30), .ifidx = it[1], .active = 1,
            };
            f->h2d_rd[flowid] = 0;
        }
        d2h_put(s, D2H_CTRL, cpl);
        break;
    case MSG_FLOW_DELETE:
    case MSG_FLOW_FLUSH:
        flowid = lduw_le_p(it + 8);
        cmn_hdr(cpl, it[0] + 1, it[1], ldl_le_p(it + 4));
        stw_le_p(cpl + 10, flowid);
        if (flowid >= 2 && flowid < BCM4366_FW_NH2D && it[0] == MSG_FLOW_DELETE) {
            f->flow[flowid].active = 0;
        }
        d2h_put(s, D2H_CTRL, cpl);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "bcm4366[%u]: control message type 0x%x\n",
                      s->radio, it[0]);
        break;
    }
}

static void fw_tx_item(BCM4366State *s, int flowid, const uint8_t *it)
{
    BCM4366Fw *f = &s->fw;
    uint8_t ifidx = it[1], cpl[64] = { 0 };
    uint16_t dlen = lduw_le_p(it + 42);
    g_autofree uint8_t *frame = g_malloc(14 + dlen);

    if (it[0] != MSG_TX_POST) {
        qemu_log_mask(LOG_UNIMP, "bcm4366[%u]: flow ring message type 0x%x\n",
                      s->radio, it[0]);
        return;
    }
    memcpy(frame, it + 8, 14);
    if (dlen) {
        bcm4366_dma_read(s, ldq_le_p(it + 32), frame + 14, dlen);
    }
    wl_connect(s);
    if (f->wl_fd >= 0) {
        struct wlshim_hdr h = { .op = WLSHIM_TX, .bss = ifidx, .len = 14 + dlen };
        uint8_t none[4];

        if (wl_call(s, &h, frame, none, 0) && h.more) {
            timer_mod(f->poll_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      FW_POLL_SOON_NS);
        }
    }
    cmn_hdr(cpl, MSG_TX_STATUS, ifidx, ldl_le_p(it + 4));
    stw_le_p(cpl + 10, flowid);
    d2h_put(s, D2H_TX, cpl);
}

/* Consume H2D ring i from our read index to the host's write index. */
static void fw_h2d_ring(BCM4366State *s, int i)
{
    BCM4366Fw *f = &s->fw;
    uint64_t base;
    uint16_t max, len, wr;
    uint8_t it[64];
    int guard = 0;

    if (i < 2) {
        uint32_t rm = ringmem(s, i);

        base = tcm64(s, rm + RM_BASE);
        max = tcm16(s, rm + RM_MAXITEM);
        len = tcm16(s, rm + RM_LENITEM);
    } else {
        if (!f->flow[i].active) {
            return;
        }
        base = f->flow[i].base;
        max = f->flow[i].max_items;
        len = f->flow[i].len_item;
    }
    if (!base || !max || len < 8 || len > sizeof(it)) {
        return;
    }
    wr = idx_get(s, RI_H2D_W_HOST, f->h2d_w, i);
    if (wr != f->h2d_rd[i]) {
        BCM4366_LOG(s, "H2D ring %d: rd %u wr %u (max %u len %u)", i, f->h2d_rd[i], wr,
                    max, len);
    }
    if (wr >= max) {
        return;
    }
    while (f->h2d_rd[i] != wr && guard++ < max) {
        if (!bcm4366_dma_read(s, base + (uint64_t)f->h2d_rd[i] * len, it, len)) {
            qemu_log_mask(LOG_GUEST_ERROR, "bcm4366[%u]: H2D ring %d DMA failed\n",
                          s->radio, i);
            break;
        }
        f->h2d_rd[i] = (f->h2d_rd[i] + 1) % max;
        if (i == 0) {
            fw_ctrl_item(s, it);
        } else if (i == 1) {
            if (it[0] == MSG_RXBUF_POST && f->nrxbuf < BCM4366_FW_NRXBUF) {
                BCM4366HostBuf *b = &f->rxbuf[(f->rxbuf_head + f->nrxbuf) %
                                              BCM4366_FW_NRXBUF];

                b->pktid = ldl_le_p(it + 4);
                b->len = lduw_le_p(it + 10);
                b->addr = ldq_le_p(it + 24);
                f->nrxbuf++;
            }
        } else {
            fw_tx_item(s, i, it);
        }
    }
    idx_put(s, RI_H2D_R_HOST, f->h2d_r, i, f->h2d_rd[i]);
}

static void fw_poll_wl(BCM4366State *s)
{
    BCM4366Fw *f = &s->fw;
    g_autofree uint8_t *rep = g_malloc(WLSHIM_BUF);
    int rounds = 0;

    wl_connect(s);
    while (f->wl_fd >= 0 && rounds++ < 8 && f->npend < BCM4366_FW_NPEND - 16) {
        struct wlshim_hdr h = { .op = WLSHIM_POLL, .cmd = WLSHIM_POLL_RADIO };
        uint32_t off = 0;

        if (!wl_call(s, &h, NULL, rep, WLSHIM_BUF)) {
            break;
        }
        while (off + sizeof(struct wlshim_rec) <= h.len) {
            struct wlshim_rec r;

            memcpy(&r, rep + off, sizeof(r));
            r.len = le16_to_cpu(r.len);
            if (off + sizeof(r) + r.len > h.len) {
                break;
            }
            /* virtual interfaces and carrier follow the WLC_E_IF/LINK
             * events the responder also sends; dhd acts on those */
            if (le16_to_cpu(r.type) == WLSHIM_REC_RX) {
                pend_add(s, r.bss, rep + off + sizeof(r), r.len);
            }
            off += WLSHIM_ALIGN(sizeof(r) + r.len);
        }
        if (!h.more) {
            break;
        }
    }
}

static void fw_run(void *opaque)
{
    BCM4366State *s = opaque;
    BCM4366Fw *f = &s->fw;
    uint32_t w0[BCM4366_FW_ND2H];

    if (f->state != 2) {
        return;
    }
    memcpy(w0, f->d2h_wr, sizeof(w0));
    for (int i = 0; i < BCM4366_FW_NH2D; i++) {
        fw_h2d_ring(s, i);
    }
    pend_flush(s);
    if (memcmp(w0, f->d2h_wr, sizeof(w0))) {
        bcm4366_d2h_doorbell(s);
    }
}

static void fw_poll(void *opaque)
{
    BCM4366State *s = opaque;
    BCM4366Fw *f = &s->fw;

    if (f->state != 2) {
        return;
    }
    fw_poll_wl(s);
    fw_run(s);
    /* keep the TCM copies of the host's completion read indices current,
     * as the firmware does (dhd dump shows them) */
    for (int r = 0; r < BCM4366_FW_ND2H; r++) {
        idx_get(s, RI_D2H_R_HOST, f->d2h_r, r);
    }
    timer_mod(f->poll_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (f->npend ? FW_POLL_SOON_NS : FW_POLL_NS));
}

/* ---- start-up --------------------------------------------------------------------- */

static void fw_parse_nvram(BCM4366State *s, uint32_t vars, uint32_t size)
{
    g_autofree char *v = g_malloc0(size + 1);

    bcm4366_tcm_read(s, vars, v, size);
    for (char *p = v; p < v + size && *p; p += strlen(p) + 1) {
        unsigned m[6];

        if (sscanf(p, "macaddr=%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3],
                   &m[4], &m[5]) == 6) {
            for (int i = 0; i < 6; i++) {
                s->fw.mac[i] = m[i];
            }
        }
    }
}

/*
 * The downloaded image's version and FWID, from the trailer its build
 * appends ("... Version: <ver> CRC: ... FWID: 01-<hex>"), for the console's
 * start-up lines and pcie_shared's fwid. Taken from the image so that what
 * the host sees is whatever it downloaded.
 */
static void fw_image_id(BCM4366State *s, uint32_t len, char *ver, size_t vlen,
                        uint32_t *fwid)
{
    g_autofree char *img = g_malloc(len + 1);
    char *v, *e, *id;

    pstrcpy(ver, vlen, "unknown");
    *fwid = 0;
    bcm4366_tcm_read(s, BCM4366_RAM_BASE, img, len);
    img[len] = 0;
    id = memmem(img, len, "FWID: 01-", 9);
    if (!id) {
        return;
    }
    *fwid = strtoul(id + 9, NULL, 16);
    /* the image is binary: search bytes, not C strings */
    for (v = id - 9; v >= img && memcmp(v, "Version: ", 9); v--) {
    }
    if (v >= img) {
        v += 9;
        e = memmem(v, id - v, " CRC:", 5);
        if (e && e - v < (ptrdiff_t)vlen) {
            memcpy(ver, v, e - v);
            ver[e - v] = 0;
        }
    }
}

/*
 * hnd_debug_t, which the image reserves and its start-up fills: the image
 * points at it with "DBPP" and the address, near the start of RAM; the
 * host's dhd utility follows that to the console (consoledump) and checks
 * magic "DEBG" and version 1.
 */
#define DBPP_MAGIC          0x50504244
#define DEBG_MAGIC          0x47424544

static void fw_fill_debug(BCM4366State *s, const char *ver, uint32_t fwid)
{
    BCM4366Fw *f = &s->fw;
    uint8_t d[72] = { 0 };
    uint32_t at = 0;

    for (uint32_t o = 0; o < 0x200; o += 4) {
        if (tcm32(s, BCM4366_RAM_BASE + o) == DBPP_MAGIC) {
            at = tcm32(s, BCM4366_RAM_BASE + o + 4);
            break;
        }
    }
    if (at < BCM4366_RAM_BASE || at + sizeof(d) > BCM4366_RAM_BASE + BCM4366_RAM_SIZE) {
        return;
    }
    stl_le_p(d + 0, DEBG_MAGIC);
    stl_le_p(d + 4, 1);                                 /* version */
    stl_le_p(d + 8, fwid);
    pstrcpy((char *)d + 12, 32, ver);                   /* epivers */
    stl_le_p(d + 48, f->cons);                          /* console */
    stl_le_p(d + 52, BCM4366_RAM_BASE);
    stl_le_p(d + 56, BCM4366_RAM_SIZE);
    stl_le_p(d + 64, BCM4366_RAM_BASE);                 /* ROM: 0..2M [assume] */
    bcm4366_tcm_write(s, at, d, sizeof(d));
}

static void fw_boot(void *opaque)
{
    BCM4366State *s = opaque;
    BCM4366Fw *f = &s->fw;
    uint32_t end = BCM4366_RAM_BASE + BCM4366_RAM_SIZE;
    uint32_t tok = tcm32(s, end - 4), words = tok & 0xffff, vars = end - 4, a;
    int unit = s->radio;
    g_autofree char *line = NULL;
    char ver[64];
    uint32_t fwid;

    if (f->state != 1) {
        return;
    }
    if (((~tok >> 16) & 0xffff) == words && words * 4 < BCM4366_RAM_SIZE / 2) {
        vars -= words * 4;
        fw_parse_nvram(s, vars, words * 4);
    }
    if (!tcm32(s, BCM4366_RAM_BASE)) {
        qemu_log_mask(LOG_GUEST_ERROR, "bcm4366[%u]: ARM released with no image; "
                      "staying silent\n", s->radio);
        f->state = 0;
        return;
    }

    /* below the nvram: console, then the shared structures */
    a = (vars & ~0xffu) - CONS_BUF_SIZE;
    f->cons_buf = a;
    f->cons_size = CONS_BUF_SIZE;
    f->cons_idx = 0;
    a -= 0x100;
    f->cons = a;
    a -= 0x100;
    f->shared = a;
    f->ring_info = a + 0x80;
    a -= 0x100;
    f->ring_mem = a;
    a -= 0x100;
    f->h2d_w = a;
    f->h2d_r = a + 0x80;
    a -= 0x40;
    f->d2h_w = a;
    f->d2h_r = a + 0x10;
    f->h2d_mb = a + 0x20;
    f->d2h_mb = a + 0x24;
    {
        g_autofree uint8_t *z = g_malloc0(vars - a);

        bcm4366_tcm_write(s, a, z, vars - a);
    }

    fw_image_id(s, vars - BCM4366_RAM_BASE, ver, sizeof(ver), &fwid);
    tcm_st32(s, f->cons + 8, f->cons_buf);              /* log.buf */
    tcm_st32(s, f->cons + 12, f->cons_size);            /* log.buf_size */
    line = g_strdup_printf("RTE (PCIE-MSG_BUF) %s on BCM4366 r3 @ "
                           "40.0/200.0/800.0MHz", ver);
    cons_puts(s, line);
    g_free(line);
    cons_puts(s, "allocating a max of 511 rxcplid buffers");
    cons_puts(s, "pciemsgbuf0: Broadcom PCIE MSGBUF driver");
    line = g_strdup_printf("wlc_bmac_attach, deviceid 0x%x nbands 1", s->devid);
    cons_puts(s, line);
    g_free(line);
    line = g_strdup_printf("wl%d: wlc_bmac_attach: chiprev 3 corerev 64 cccap "
                           "0x58400009 maccap 0xf0018705 band %s, phy_type 11 "
                           "phy_rev 32", unit, unit ? "5G" : "2.4G");
    cons_puts(s, line);
    g_free(line);
    line = g_strdup_printf("wl%d: Broadcom BCM4366 802.11 Wireless Controller %s",
                           unit, ver);
    cons_puts(s, line);
    cons_puts(s, "ThreadX v5.6 initialized");

    fw_fill_debug(s, ver, fwid);
    for (int i = 0; i < NCOMMON; i++) {
        tcm_st16(s, ringmem(s, i), i);
    }
    tcm_st32(s, f->ring_info + RI_RINGMEM, f->ring_mem);
    tcm_st32(s, f->ring_info + RI_H2D_W, f->h2d_w);
    tcm_st32(s, f->ring_info + RI_H2D_R, f->h2d_r);
    tcm_st32(s, f->ring_info + RI_D2H_W, f->d2h_w);
    tcm_st32(s, f->ring_info + RI_D2H_R, f->d2h_r);
    tcm_st16(s, f->ring_info + RI_MAXSUBQ, BCM4366_FW_NH2D);

    tcm_st32(s, f->shared + SH_FLAGS, SH_VERSION | SH_DMA_INDEX | SH_2BYTE_INDICES |
             SH_D2H_SYNC_SEQNUM);
    tcm_st32(s, f->shared + SH_CONSOLE, f->cons);
    tcm_st32(s, f->shared + SH_FWID, fwid);
    tcm_st16(s, f->shared + SH_LFRAG, 2048);
    tcm_st16(s, f->shared + SH_MAXRXBUFS, 480);
    tcm_st32(s, f->shared + SH_RXOFFSET, 0);
    tcm_st32(s, f->shared + SH_H2D_MB, f->h2d_mb);
    tcm_st32(s, f->shared + SH_D2H_MB, f->d2h_mb);
    tcm_st32(s, f->shared + SH_RINGS_INFO, f->ring_info);

    memset(f->h2d_rd, 0, sizeof(f->h2d_rd));
    memset(f->d2h_wr, 0, sizeof(f->d2h_wr));
    memset(f->flow, 0, sizeof(f->flow));
    for (int r = 0; r < BCM4366_FW_ND2H; r++) {
        f->d2h_seq[r] = D2H_EPOCH_INIT;
    }
    f->niocbuf = f->nevbuf = f->nrxbuf = f->rxbuf_head = 0;
    pend_clear(f);

    /* the radio behind this dongle starts from power-on, as the chip does */
    wl_connect(s);
    if (f->wl_fd >= 0) {
        struct wlshim_hdr h = { .op = WLSHIM_RESET };
        uint8_t none[4];

        wl_call(s, &h, NULL, none, 0);
    }

    f->state = 2;
    BCM4366_LOG(s, "up: shared 0x%x, nvram %u bytes at 0x%x", f->shared, words * 4, vars);
    tcm_st32(s, end - 4, f->shared);                    /* "dongle ready" */
    timer_mod(f->poll_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + FW_POLL_NS);
}

void bcm4366_fw_start(BCM4366State *s)
{
    BCM4366Fw *f = &s->fw;

    f->state = 1;
    timer_mod(f->boot_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + FW_BOOT_NS);
}

void bcm4366_fw_stop(BCM4366State *s)
{
    BCM4366Fw *f = &s->fw;

    f->state = 0;
    if (f->boot_timer) {
        timer_del(f->boot_timer);
        timer_del(f->poll_timer);
    }
    pend_clear(f);
}

void bcm4366_fw_doorbell(BCM4366State *s)
{
    if (s->fw.state == 2) {
        qemu_bh_schedule(s->fw.bh);
    }
}

void bcm4366_fw_h2d_mailbox(BCM4366State *s)
{
    BCM4366Fw *f = &s->fw;
    uint32_t data;

    if (f->state != 2) {
        return;
    }
    /* D3 inform and the like: acknowledge whatever the host announces */
    data = tcm32(s, f->h2d_mb);
    qemu_log_mask(LOG_UNIMP, "bcm4366[%u]: H2D mailbox data 0x%x\n", s->radio, data);
    if (data & 1) {                                     /* H2D_HOST_D3_INFORM */
        tcm_st32(s, f->d2h_mb, 1);                      /* D2H_DEV_D3_ACK */
        bcm4366_d2h_mailbox(s);
    }
    tcm_st32(s, f->h2d_mb, 0);
}

void bcm4366_fw_realize(BCM4366State *s)
{
    BCM4366Fw *f = &s->fw;

    f->wl_fd = -1;
    f->boot_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, fw_boot, s);
    f->poll_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, fw_poll, s);
    f->bh = qemu_bh_new_guarded(fw_run, s, &DEVICE(s)->mem_reentrancy_guard);
}

/* ---- migration --------------------------------------------------------------------- */

static int fw_post_load(void *opaque, int version_id)
{
    BCM4366Fw *f = opaque;

    /* frames in flight from the responder are not migrated; the
     * responder's own state is, if it saves it */
    f->pend_head = f->npend = 0;
    if (f->state == 1) {
        timer_mod(f->boot_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + FW_BOOT_NS);
    } else if (f->state == 2) {
        timer_mod(f->poll_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + FW_POLL_NS);
    }
    return 0;
}

static const VMStateDescription vmstate_hostbuf = {
    .name = "bcm4366-hostbuf",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(pktid, BCM4366HostBuf),
        VMSTATE_UINT32(len, BCM4366HostBuf),
        VMSTATE_UINT64(addr, BCM4366HostBuf),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_flowring = {
    .name = "bcm4366-flowring",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(base, BCM4366FlowRing),
        VMSTATE_UINT16(max_items, BCM4366FlowRing),
        VMSTATE_UINT16(len_item, BCM4366FlowRing),
        VMSTATE_UINT8(ifidx, BCM4366FlowRing),
        VMSTATE_UINT8(active, BCM4366FlowRing),
        VMSTATE_END_OF_LIST()
    },
};

const VMStateDescription bcm4366_fw_vmstate = {
    .name = "bcm4366-fw",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = fw_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(state, BCM4366Fw),
        VMSTATE_UINT32(shared, BCM4366Fw),
        VMSTATE_UINT32(ring_info, BCM4366Fw),
        VMSTATE_UINT32(ring_mem, BCM4366Fw),
        VMSTATE_UINT32(h2d_w, BCM4366Fw),
        VMSTATE_UINT32(h2d_r, BCM4366Fw),
        VMSTATE_UINT32(d2h_w, BCM4366Fw),
        VMSTATE_UINT32(d2h_r, BCM4366Fw),
        VMSTATE_UINT32(h2d_mb, BCM4366Fw),
        VMSTATE_UINT32(d2h_mb, BCM4366Fw),
        VMSTATE_UINT32(cons, BCM4366Fw),
        VMSTATE_UINT32(cons_buf, BCM4366Fw),
        VMSTATE_UINT32(cons_size, BCM4366Fw),
        VMSTATE_UINT32(cons_idx, BCM4366Fw),
        VMSTATE_UINT16_ARRAY(h2d_rd, BCM4366Fw, BCM4366_FW_NH2D),
        VMSTATE_UINT16_ARRAY(d2h_wr, BCM4366Fw, BCM4366_FW_ND2H),
        VMSTATE_UINT32_ARRAY(d2h_seq, BCM4366Fw, BCM4366_FW_ND2H),
        VMSTATE_STRUCT_ARRAY(flow, BCM4366Fw, BCM4366_FW_NH2D, 1,
                             vmstate_flowring, BCM4366FlowRing),
        VMSTATE_STRUCT_ARRAY(iocbuf, BCM4366Fw, BCM4366_FW_NIOCBUF, 1,
                             vmstate_hostbuf, BCM4366HostBuf),
        VMSTATE_UINT32(niocbuf, BCM4366Fw),
        VMSTATE_STRUCT_ARRAY(evbuf, BCM4366Fw, BCM4366_FW_NEVBUF, 1,
                             vmstate_hostbuf, BCM4366HostBuf),
        VMSTATE_UINT32(nevbuf, BCM4366Fw),
        VMSTATE_STRUCT_ARRAY(rxbuf, BCM4366Fw, BCM4366_FW_NRXBUF, 1,
                             vmstate_hostbuf, BCM4366HostBuf),
        VMSTATE_UINT32(rxbuf_head, BCM4366Fw),
        VMSTATE_UINT32(nrxbuf, BCM4366Fw),
        VMSTATE_UINT8_ARRAY(mac, BCM4366Fw, 6),
        VMSTATE_END_OF_LIST()
    },
};
