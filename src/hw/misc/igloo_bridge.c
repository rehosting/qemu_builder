/*
 * igloo-bridge: host end of the igloo.ko TCP bridge, a stand-in for vsock on
 * guest kernels without it. The guest module polls with one hypercall
 * (protocol: include/hw/misc/igloo_bridge_proto.h) and opens the streams with
 * in-kernel sockets, so nothing runs in guest userspace.
 *
 *   -device igloo-bridge,fwd=18080-127.0.0.1:80+18023-23,ctl=br.sock
 *
 * fwd: '+'-separated HOSTPORT-[GUESTIP:]GUESTPORT. Each listens on host
 *      127.0.0.1:HOSTPORT; every accepted connection becomes a guest
 *      connection to GUESTIP:GUESTPORT (default 127.0.0.1).
 * ctl: a unix socket speaking the vhost-user-vsock handshake: the client
 *      sends "CONNECT [GUESTIP ]PORT\n" and gets "OK PORT\n" (or "ERR n\n")
 *      once the guest has connected, then the raw stream.
 *
 * wl: a unix socket where a wlsim responder listens. The guest module's
 *      Wi-Fi stand-in (wlshim, include/hw/misc/wlshim_proto.h) makes one
 *      synchronous hypercall per request; each is forwarded unchanged as
 *      u32 length + message and the vCPU waits for the reply. The socket is
 *      connected lazily (and again after a failure, at most once a second);
 *      until it is, the guest sees "no host side".
 *
 * Standalone use: this takes over QEMU's single penguin hypercall callback,
 * so it is for runs without Penguin attached. Under Penguin the same protocol
 * would be served by a plugin instead.
 *
 * Connections are host state, not guest state, and are not migrated. A
 * loadvm drops every host connection (its clients see EOF), and any guest
 * record for a connection the host no longer has is answered with an RST, so
 * the guest's rolled-back connections are retired too. No vmstate section,
 * so a snapshot taken with the bridge also loads without it.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/thread.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "system/penguin.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "hw/misc/igloo_bridge_proto.h"
#include "hw/misc/wlshim_proto.h"
#include <glib-unix.h>
#include <sys/un.h>
#include <poll.h>

#define TYPE_IGLOO_BRIDGE "igloo-bridge"
OBJECT_DECLARE_SIMPLE_TYPE(IglooBridge, IGLOO_BRIDGE)

enum { BC_HANDSHAKE, BC_OPENING, BC_OPEN };

typedef struct BridgeListener {
    int fd;
    bool ctl;
    uint32_t addr;                  /* network order */
    uint16_t port;                  /* network order */
} BridgeListener;

typedef struct BridgeConn {
    uint32_t id;
    int fd;
    bool ctl;
    int state;
    uint32_t addr;
    uint16_t port;
    GByteArray *in;                 /* client -> guest, not yet sent */
    GByteArray *out;                /* guest -> client, not yet written */
    uint32_t out_noack;             /* leading bytes of out that are ours */
    uint32_t guest_win;             /* bytes we may still send to the guest */
    uint32_t ack_owed;              /* written to the client, not yet acked */
    bool open_sent, client_eof, close_sent, guest_eof, shut_wr;
    bool host_rst;                  /* client error: abort the guest side */
    bool guest_rst;                 /* guest aborted: drop everything */
    char line[64];
    int linelen;
} BridgeConn;

struct IglooBridge {
    DeviceState parent_obj;
    char *fwd, *ctl, *wl;

    QemuMutex lock;
    QemuThread thread;
    int wake[2];
    bool attached;
    uint32_t next_id;
    GArray *listeners;              /* BridgeListener */
    GPtrArray *conns;               /* BridgeConn * */
    GArray *rst;                    /* guest conn ids the host doesn't know */

    QemuMutex wl_lock;              /* one wlshim request at a time */
    int wl_fd;
    int64_t wl_retry;               /* earliest next connect, ms */
    bool wl_said;                   /* "responder connected" logged */
};

static void bridge_wake(IglooBridge *s)
{
    char c = 0;

    if (write(s->wake[1], &c, 1) < 0) {
        /* pipe full: the thread is already due to wake */
    }
}

static BridgeConn *bridge_find(IglooBridge *s, uint32_t id)
{
    for (guint i = 0; i < s->conns->len; i++) {
        BridgeConn *c = g_ptr_array_index(s->conns, i);
        if (c->id == id) {
            return c;
        }
    }
    return NULL;
}

static void bridge_ctl_reply(BridgeConn *c, const char *msg)
{
    g_byte_array_append(c->out, (const guint8 *)msg, strlen(msg));
    c->out_noack += strlen(msg);
}

/* ---- vCPU side: one exchange per guest poll ---- */

static void bridge_take_tx(IglooBridge *s, const uint8_t *tx, uint32_t len)
{
    uint32_t off = 0;

    while (off + sizeof(struct igloo_bridge_rec) <= len) {
        const struct igloo_bridge_rec *r = (const void *)(tx + off);
        const uint8_t *p = (const uint8_t *)(r + 1);
        BridgeConn *c;
        int32_t v;

        if (off + sizeof(*r) + r->len > len) {
            break;
        }
        off += sizeof(*r) + IGLOO_BRIDGE_ALIGN(r->len);
        c = bridge_find(s, r->conn);
        if (!c) {
            /* gone here (finished, or dropped by a loadvm): retire it there */
            if (!(r->type == IGLOO_BRIDGE_G_CLOSED &&
                  (r->flags & IGLOO_BRIDGE_RST))) {
                g_array_append_val(s->rst, r->conn);
            }
            continue;
        }
        switch (r->type) {
        case IGLOO_BRIDGE_G_OPENED:
            memcpy(&v, p, 4);
            if (c->state != BC_OPENING) {
                break;
            }
            if (v == 0) {
                c->state = BC_OPEN;
                if (c->ctl) {
                    g_autofree char *m = g_strdup_printf("OK %u\n",
                                                         ntohs(c->port));
                    bridge_ctl_reply(c, m);
                }
            } else {
                if (c->ctl) {
                    g_autofree char *m = g_strdup_printf("ERR %d\n", v);
                    bridge_ctl_reply(c, m);
                }
                c->guest_eof = c->close_sent = true;    /* guest freed it */
            }
            break;
        case IGLOO_BRIDGE_G_DATA:
            g_byte_array_append(c->out, p, r->len);
            break;
        case IGLOO_BRIDGE_G_ACK:
            memcpy(&v, p, 4);
            c->guest_win += (uint32_t)v;
            break;
        case IGLOO_BRIDGE_G_CLOSED:
            c->guest_eof = true;
            if (r->flags & IGLOO_BRIDGE_RST) {
                c->guest_rst = true;
            }
            break;
        }
    }
}

static bool bridge_put(uint8_t *rx, uint32_t *len, uint8_t type, uint8_t flags,
                       uint32_t conn, const void *payload, uint32_t plen)
{
    struct igloo_bridge_rec r = {
        .type = type, .flags = flags, .len = plen, .conn = conn,
    };
    uint32_t need = sizeof(r) + IGLOO_BRIDGE_ALIGN(plen);

    if (*len + need > IGLOO_BRIDGE_BUF) {
        return false;
    }
    memcpy(rx + *len, &r, sizeof(r));
    memcpy(rx + *len + sizeof(r), payload, plen);
    *len += need;
    return true;
}

static uint32_t bridge_fill_rx(IglooBridge *s, uint8_t *rx)
{
    uint32_t len = 0;

    while (s->rst->len &&
           bridge_put(rx, &len, IGLOO_BRIDGE_H_CLOSE, IGLOO_BRIDGE_RST,
                      g_array_index(s->rst, uint32_t, 0), NULL, 0)) {
        g_array_remove_index(s->rst, 0);
    }
    for (guint i = 0; i < s->conns->len; i++) {
        BridgeConn *c = g_ptr_array_index(s->conns, i);

        if (c->guest_rst) {
            continue;
        }
        if (c->host_rst) {
            if (c->open_sent && !c->close_sent &&
                bridge_put(rx, &len, IGLOO_BRIDGE_H_CLOSE, IGLOO_BRIDGE_RST,
                           c->id, NULL, 0)) {
                c->close_sent = true;
            }
            continue;
        }
        if (c->state == BC_OPENING && !c->open_sent) {
            struct igloo_bridge_open o = {
                .addr = c->addr, .port = c->port, .proto = 6,
            };
            if (bridge_put(rx, &len, IGLOO_BRIDGE_H_OPEN, 0, c->id,
                           &o, sizeof(o))) {
                c->open_sent = true;
            }
            continue;
        }
        if (c->state != BC_OPEN) {
            continue;
        }
        if (c->ack_owed && bridge_put(rx, &len, IGLOO_BRIDGE_H_ACK, 0, c->id,
                                      &c->ack_owed, 4)) {
            c->ack_owed = 0;
        }
        while (c->in->len && c->guest_win) {
            uint32_t n = MIN(c->in->len, c->guest_win);
            n = MIN(n, IGLOO_BRIDGE_CHUNK);
            if (!bridge_put(rx, &len, IGLOO_BRIDGE_H_DATA, 0, c->id,
                            c->in->data, n)) {
                break;
            }
            g_byte_array_remove_range(c->in, 0, n);
            c->guest_win -= n;
        }
        if (c->client_eof && !c->in->len && !c->close_sent &&
            bridge_put(rx, &len, IGLOO_BRIDGE_H_CLOSE, 0, c->id, NULL, 0)) {
            c->close_sent = true;
        }
    }
    return len;
}

static int bridge_hypercall(IglooBridge *s, uint64_t a0, uint64_t a1,
                            uint64_t a2, uint64_t a3, uint64_t *ret)
{
    g_autofree uint8_t *tx = NULL;
    g_autofree uint8_t *rx = NULL;
    uint32_t txlen = a2, rxlen;

    if (a0 != IGLOO_BRIDGE_MAGIC || txlen > IGLOO_BRIDGE_BUF) {
        return -1;
    }
    tx = g_malloc(txlen + 1);
    rx = g_malloc(IGLOO_BRIDGE_BUF);
    if (txlen && address_space_read(&address_space_memory, a1,
                                    MEMTXATTRS_UNSPECIFIED, tx, txlen)) {
        return -1;
    }

    qemu_mutex_lock(&s->lock);
    if (!s->attached) {
        s->attached = true;
        info_report("igloo-bridge: guest attached (tx %#" PRIx64
                    " rx %#" PRIx64 ")", a1, a3);
    }
    bridge_take_tx(s, tx, txlen);
    rxlen = bridge_fill_rx(s, rx);
    qemu_mutex_unlock(&s->lock);

    if (rxlen && address_space_write(&address_space_memory, a3,
                                     MEMTXATTRS_UNSPECIFIED, rx, rxlen)) {
        rxlen = 0;
    }
    if (txlen || rxlen) {
        bridge_wake(s);
    }
    *ret = rxlen;
    return 0;
}

/* ---- wlshim: forward each request to the responder ---- */

static bool wl_io(int fd, void *buf, size_t n, bool out)
{
    uint8_t *p = buf;

    while (n) {
        ssize_t r = out ? send(fd, p, n, MSG_NOSIGNAL) : recv(fd, p, n, 0);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return false;
        }
        p += r;
        n -= r;
    }
    return true;
}

static void wl_connect(IglooBridge *s)
{
    struct sockaddr_un sun = { .sun_family = AF_UNIX };
    struct timeval tv = { .tv_sec = 10 };   /* a hung responder fails the call */
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    int fd;

    if (now < s->wl_retry) {
        return;
    }
    s->wl_retry = now + 1000;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return;
    }
    pstrcpy(sun.sun_path, sizeof(sun.sun_path), s->wl);
    if (connect(fd, (struct sockaddr *)&sun, sizeof(sun))) {
        close(fd);
        return;
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    s->wl_fd = fd;
    if (!s->wl_said) {
        s->wl_said = true;
        info_report("igloo-bridge: wl responder connected (%s)", s->wl);
    }
}

static void wl_drop(IglooBridge *s, const char *why)
{
    warn_report("igloo-bridge: wl responder %s; reconnecting", why);
    close(s->wl_fd);
    s->wl_fd = -1;
    s->wl_said = false;
}

static int wl_hypercall(IglooBridge *s, uint64_t a0, uint64_t op,
                        uint64_t pa, uint64_t size, uint64_t *ret)
{
    g_autofree uint8_t *buf = NULL;
    struct wlshim_hdr h;
    uint32_t n, m;

    if (!s->wl || a0 != WLSHIM_MAGIC) {
        return -1;                      /* r0 comes back unchanged */
    }
    if (size > WLSHIM_BUF) {
        size = WLSHIM_BUF;
    }
    if (size < sizeof(h) ||
        address_space_read(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED,
                           &h, sizeof(h))) {
        *ret = WLSHIM_EHOST;
        return 0;
    }
    n = sizeof(h) + MIN(le32_to_cpu(h.len), size - sizeof(h));
    buf = g_malloc(size);
    if (address_space_read(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED,
                           buf, n)) {
        *ret = WLSHIM_EHOST;
        return 0;
    }
    ((struct wlshim_hdr *)buf)->len = cpu_to_le32(n - sizeof(h));

    qemu_mutex_lock(&s->wl_lock);
    if (s->wl_fd < 0) {
        wl_connect(s);
    }
    if (s->wl_fd < 0) {
        qemu_mutex_unlock(&s->wl_lock);
        return -1;                      /* no host side yet */
    }
    m = cpu_to_le32(n);
    if (!wl_io(s->wl_fd, &m, 4, true) || !wl_io(s->wl_fd, buf, n, true)) {
        wl_drop(s, "write failed");
        goto fail;
    }
    if (!wl_io(s->wl_fd, &m, 4, false)) {
        wl_drop(s, "closed or timed out");
        goto fail;
    }
    m = le32_to_cpu(m);
    if (m < sizeof(h) || m > size) {
        wl_drop(s, "sent a bad reply length");
        goto fail;
    }
    if (!wl_io(s->wl_fd, buf, m, false)) {
        wl_drop(s, "closed mid-reply");
        goto fail;
    }
    qemu_mutex_unlock(&s->wl_lock);

    /* the payload length must match what was sent */
    n = le32_to_cpu(((struct wlshim_hdr *)buf)->len);
    if (n > m - sizeof(h)) {
        ((struct wlshim_hdr *)buf)->len = cpu_to_le32(m - sizeof(h));
    }
    if (address_space_write(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED,
                            buf, m)) {
        *ret = WLSHIM_EHOST;
        return 0;
    }
    *ret = 0;
    return 0;

fail:
    qemu_mutex_unlock(&s->wl_lock);
    *ret = WLSHIM_EHOST;
    return 0;
}

static int igloo_hypercall(CPUState *cs, uint64_t nr, uint64_t a0,
                           uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t *ret,
                           void *opaque)
{
    IglooBridge *s = opaque;

    switch (nr) {
    case IGLOO_BRIDGE_HC:
        return s->listeners->len ? bridge_hypercall(s, a0, a1, a2, a3, ret)
                                 : -1;
    case WLSHIM_HC:
        return wl_hypercall(s, a0, a1, a2, a3, ret);
    }
    return -1;
}

/* ---- I/O thread: host sockets ---- */

static void bridge_accept(IglooBridge *s, BridgeListener *l)
{
    BridgeConn *c;
    int fd = accept(l->fd, NULL, NULL);

    if (fd < 0) {
        return;
    }
    qemu_set_blocking(fd, false, NULL);
    c = g_new0(BridgeConn, 1);
    c->id = ++s->next_id;
    c->fd = fd;
    c->ctl = l->ctl;
    c->state = l->ctl ? BC_HANDSHAKE : BC_OPENING;
    c->addr = l->addr;
    c->port = l->port;
    c->in = g_byte_array_new();
    c->out = g_byte_array_new();
    c->guest_win = IGLOO_BRIDGE_WIN;
    g_ptr_array_add(s->conns, c);
}

/* "CONNECT [ip ]port" */
static bool bridge_parse_connect(BridgeConn *c)
{
    g_auto(GStrv) w = g_strsplit_set(c->line, " \r", -1);
    int n = g_strv_length(w);
    struct in_addr a = { .s_addr = htonl(INADDR_LOOPBACK) };
    unsigned long port;

    while (n && !*w[n - 1]) {
        n--;
    }
    if (n < 2 || n > 3 || strcmp(w[0], "CONNECT") ||
        (n == 3 && !inet_aton(w[1], &a)) ||
        qemu_strtoul(w[n - 1], NULL, 10, &port) || !port || port > 65535) {
        return false;
    }
    c->addr = a.s_addr;
    c->port = htons(port);
    return true;
}

static void bridge_read(IglooBridge *s, BridgeConn *c)
{
    uint8_t buf[16384];
    ssize_t n;

    if (c->state == BC_HANDSHAKE) {
        /* byte at a time, so stream bytes after the line stay queued */
        while (c->linelen < (int)sizeof(c->line) - 1) {
            char ch;
            n = read(c->fd, &ch, 1);
            if (n <= 0) {
                if (n == 0 || errno != EAGAIN) {
                    c->host_rst = true;
                }
                return;
            }
            if (ch == '\n') {
                c->line[c->linelen] = 0;
                if (bridge_parse_connect(c)) {
                    c->state = BC_OPENING;
                } else {
                    bridge_ctl_reply(c, "ERR bad request\n");
                    c->guest_eof = c->close_sent = true;
                }
                return;
            }
            c->line[c->linelen++] = ch;
        }
        bridge_ctl_reply(c, "ERR bad request\n");
        c->guest_eof = c->close_sent = true;
        return;
    }
    n = MIN(sizeof(buf), IGLOO_BRIDGE_WIN - c->in->len);
    if (!n) {
        return;                             /* guest window full */
    }
    n = read(c->fd, buf, n);
    if (n > 0) {
        g_byte_array_append(c->in, buf, n);
    } else if (n == 0) {
        c->client_eof = true;
    } else if (errno != EAGAIN && errno != EINTR) {
        c->host_rst = true;
    }
}

static void bridge_write(BridgeConn *c)
{
    ssize_t n = send(c->fd, c->out->data, c->out->len, MSG_NOSIGNAL);

    if (n < 0) {
        if (errno != EAGAIN && errno != EINTR) {
            c->host_rst = true;
        }
        return;
    }
    if ((uint32_t)n <= c->out_noack) {
        c->out_noack -= n;
    } else {
        c->ack_owed += n - c->out_noack;
        c->out_noack = 0;
    }
    g_byte_array_remove_range(c->out, 0, n);
}

static bool bridge_done(BridgeConn *c)
{
    if (c->guest_rst) {
        return true;
    }
    if (c->host_rst) {
        return c->close_sent || !c->open_sent;
    }
    return c->guest_eof && c->close_sent && !c->out->len;
}

static void bridge_free(BridgeConn *c)
{
    close(c->fd);
    g_byte_array_free(c->in, true);
    g_byte_array_free(c->out, true);
    g_free(c);
}

static void *bridge_thread(void *opaque)
{
    IglooBridge *s = opaque;
    g_autofree struct pollfd *pfd = NULL;
    g_autofree BridgeConn **pc = NULL;
    guint cap = 0;

    for (;;) {
        guint nl, np = 0, i;

        qemu_mutex_lock(&s->lock);
        nl = s->listeners->len;
        if (cap < 1 + nl + s->conns->len) {
            cap = 2 * (1 + nl + s->conns->len);
            pfd = g_renew(struct pollfd, pfd, cap);
            pc = g_renew(BridgeConn *, pc, cap);
        }
        pfd[np++] = (struct pollfd){ .fd = s->wake[0], .events = POLLIN };
        for (i = 0; i < nl; i++) {
            BridgeListener *l = &g_array_index(s->listeners, BridgeListener, i);
            pfd[np++] = (struct pollfd){ .fd = l->fd, .events = POLLIN };
        }
        for (i = 0; i < s->conns->len; i++) {
            BridgeConn *c = g_ptr_array_index(s->conns, i);
            short ev = 0;

            if (!c->client_eof && !c->host_rst &&
                c->in->len < IGLOO_BRIDGE_WIN) {
                ev |= POLLIN;
            }
            if (c->out->len) {
                ev |= POLLOUT;
            }
            pc[np] = c;
            /* nothing wanted: a negative fd keeps POLLHUP from spinning us */
            pfd[np++] = (struct pollfd){ .fd = ev ? c->fd : -1, .events = ev };
        }
        qemu_mutex_unlock(&s->lock);

        if (poll(pfd, np, 500) < 0 && errno != EINTR) {
            error_report("igloo-bridge: poll: %s", strerror(errno));
            return NULL;
        }

        qemu_mutex_lock(&s->lock);
        if (pfd[0].revents & POLLIN) {
            char buf[64];
            while (read(s->wake[0], buf, sizeof(buf)) > 0) {
            }
        }
        for (i = 0; i < nl; i++) {
            if (pfd[1 + i].revents & POLLIN) {
                bridge_accept(s, &g_array_index(s->listeners,
                                                BridgeListener, i));
            }
        }
        for (i = 1 + nl; i < np; i++) {
            BridgeConn *c = pc[i];

            if ((pfd[i].events & POLLIN) &&
                (pfd[i].revents & (POLLIN | POLLHUP | POLLERR))) {
                bridge_read(s, c);
            }
            if (c->out->len && (pfd[i].revents & POLLOUT)) {
                bridge_write(c);
            }
        }
        for (i = 0; i < s->conns->len;) {
            BridgeConn *c = g_ptr_array_index(s->conns, i);

            if (c->guest_eof && !c->out->len && !c->shut_wr) {
                shutdown(c->fd, SHUT_WR);
                c->shut_wr = true;
            }
            if (bridge_done(c)) {
                g_ptr_array_remove_index_fast(s->conns, i);
                bridge_free(c);
            } else {
                i++;
            }
        }
        qemu_mutex_unlock(&s->lock);
    }
    return NULL;
}

/* loadvm rolls the guest back: its connections are not ours any more */
static void bridge_vm_state(void *opaque, bool running, RunState state)
{
    IglooBridge *s = opaque;

    if (running || state != RUN_STATE_RESTORE_VM) {
        return;
    }
    qemu_mutex_lock(&s->lock);
    for (guint i = 0; i < s->conns->len; i++) {
        BridgeConn *c = g_ptr_array_index(s->conns, i);
        c->guest_rst = true;            /* the I/O thread closes it */
    }
    g_array_set_size(s->rst, 0);
    qemu_mutex_unlock(&s->lock);
    bridge_wake(s);
}

/* ---- setup ---- */

static int bridge_listen_tcp(uint16_t port, Error **errp)
{
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    int one = 1, fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        error_setg_errno(errp, errno, "igloo-bridge: socket");
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) || listen(fd, 16)) {
        error_setg_errno(errp, errno, "igloo-bridge: listen 127.0.0.1:%u",
                         port);
        close(fd);
        return -1;
    }
    qemu_set_blocking(fd, false, NULL);
    return fd;
}

static int bridge_listen_unix(const char *path, Error **errp)
{
    struct sockaddr_un sun = { .sun_family = AF_UNIX };
    struct stat st;
    int fd;

    if (strlen(path) >= sizeof(sun.sun_path)) {
        error_setg(errp, "igloo-bridge: ctl path too long");
        return -1;
    }
    strcpy(sun.sun_path, path);
    if (!stat(path, &st) && S_ISSOCK(st.st_mode)) {
        unlink(path);                       /* a stale socket of ours */
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)&sun, sizeof(sun)) ||
        listen(fd, 16)) {
        error_setg_errno(errp, errno, "igloo-bridge: listen %s", path);
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    qemu_set_blocking(fd, false, NULL);
    return fd;
}

/* HOSTPORT-[GUESTIP:]GUESTPORT */
static bool bridge_add_fwd(IglooBridge *s, const char *spec, Error **errp)
{
    g_auto(GStrv) hp = g_strsplit(spec, "-", 2);
    struct in_addr a = { .s_addr = htonl(INADDR_LOOPBACK) };
    unsigned long hport, gport;
    const char *g, *colon;
    BridgeListener l = { 0 };

    if (g_strv_length(hp) != 2 || qemu_strtoul(hp[0], NULL, 10, &hport) ||
        !hport || hport > 65535) {
        goto bad;
    }
    g = hp[1];
    colon = strrchr(g, ':');
    if (colon) {
        g_autofree char *ip = g_strndup(g, colon - g);
        if (!inet_aton(ip, &a)) {
            goto bad;
        }
        g = colon + 1;
    }
    if (qemu_strtoul(g, NULL, 10, &gport) || !gport || gport > 65535) {
        goto bad;
    }
    l.fd = bridge_listen_tcp(hport, errp);
    if (l.fd < 0) {
        return false;
    }
    l.addr = a.s_addr;
    l.port = htons(gport);
    g_array_append_val(s->listeners, l);
    info_report("igloo-bridge: 127.0.0.1:%lu -> guest %s:%lu",
                hport, inet_ntoa(a), gport);
    return true;
bad:
    error_setg(errp, "igloo-bridge: bad fwd '%s' "
               "(want HOSTPORT-[GUESTIP:]GUESTPORT)", spec);
    return false;
}

static void bridge_realize(DeviceState *dev, Error **errp)
{
    IglooBridge *s = IGLOO_BRIDGE(dev);

    if (s->fwd && *s->fwd) {
        g_auto(GStrv) specs = g_strsplit(s->fwd, "+", -1);
        for (int i = 0; specs[i]; i++) {
            if (*specs[i] && !bridge_add_fwd(s, specs[i], errp)) {
                return;
            }
        }
    }
    if (s->ctl && *s->ctl) {
        BridgeListener l = { .ctl = true };
        l.fd = bridge_listen_unix(s->ctl, errp);
        if (l.fd < 0) {
            return;
        }
        g_array_append_val(s->listeners, l);
    }
    if (s->wl && strlen(s->wl) >= sizeof_field(struct sockaddr_un, sun_path)) {
        error_setg(errp, "igloo-bridge: wl path too long");
        return;
    }
    if (!s->listeners->len && !(s->wl && *s->wl)) {
        error_setg(errp, "igloo-bridge: needs fwd=, ctl= and/or wl=");
        return;
    }
    if (!g_unix_open_pipe(s->wake, FD_CLOEXEC, NULL)) {
        error_setg_errno(errp, errno, "igloo-bridge: pipe");
        return;
    }
    qemu_set_blocking(s->wake[0], false, NULL);
    qemu_set_blocking(s->wake[1], false, NULL);

    qemu_add_vm_change_state_handler(bridge_vm_state, s);
    penguin_register_guest_hypercall(IGLOO_BRIDGE_HC);
    if (s->wl && *s->wl) {
        penguin_register_guest_hypercall(WLSHIM_HC);
    }
    set_penguin_guest_hypercall_callback(igloo_hypercall, s);
    qemu_thread_create(&s->thread, "igloo-bridge", bridge_thread, s,
                       QEMU_THREAD_DETACHED);
}

static const Property igloo_bridge_props[] = {
    DEFINE_PROP_STRING("fwd", IglooBridge, fwd),
    DEFINE_PROP_STRING("ctl", IglooBridge, ctl),
    DEFINE_PROP_STRING("wl", IglooBridge, wl),
};

static void igloo_bridge_init(Object *obj)
{
    IglooBridge *s = IGLOO_BRIDGE(obj);

    qemu_mutex_init(&s->lock);
    qemu_mutex_init(&s->wl_lock);
    s->wl_fd = -1;
    s->wake[0] = s->wake[1] = -1;
    s->listeners = g_array_new(false, true, sizeof(BridgeListener));
    s->conns = g_ptr_array_new();
    s->rst = g_array_new(false, false, sizeof(uint32_t));
}

static void igloo_bridge_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = bridge_realize;
    dc->desc = "host end of the igloo.ko TCP bridge (vsock stand-in) "
               "and Wi-Fi stand-in";
    device_class_set_props(dc, igloo_bridge_props);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    /* host sockets and threads: never migrated, see the header comment */
    dc->hotpluggable = false;
}

static const TypeInfo igloo_bridge_info = {
    .name = TYPE_IGLOO_BRIDGE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(IglooBridge),
    .instance_init = igloo_bridge_init,
    .class_init = igloo_bridge_class_init,
};

static void igloo_bridge_register_types(void)
{
    type_register_static(&igloo_bridge_info);
}

type_init(igloo_bridge_register_types)
