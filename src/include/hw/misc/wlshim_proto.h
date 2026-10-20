/*
 * wlshim wire protocol: a host model standing in for a Wi-Fi driver at the
 * netdev ioctl layer. Shared verbatim by the guest module (portal/kl_wlshim.c)
 * and QEMU (hw/misc/igloo_bridge.c, which forwards it); the host responder
 * (wlsim, Python) mirrors it in wlsim/proto.py.
 *
 * The guest calls
 *     r7 = WLSHIM_HC, r0 = WLSHIM_MAGIC, r1 = op,
 *     r2 = buffer physical address, r3 = buffer size (at most WLSHIM_BUF)
 * The buffer holds a struct wlshim_hdr and hdr.len payload bytes. The host
 * overwrites the buffer with its reply, a header and hdr.len payload bytes,
 * never more than r3 bytes in all, and returns 0 in r0. With no host side the
 * call returns r0 unchanged (WLSHIM_MAGIC). A transport failure returns
 * WLSHIM_EHOST; the guest treats it as "no host" for that call.
 *
 * Every call is synchronous: the vCPU waits for the reply. The guest may
 * issue calls from several CPUs at once, each with its own buffer; the host
 * serialises them.
 *
 * All fields are little-endian. The guest fills hdr.comm with the calling
 * task's name on every op, for the host's logs.
 *
 * Standalone QEMU forwards each call over a unix stream socket unchanged:
 * u32 n, then n bytes (header + payload); the responder answers the same way.
 *
 * Ops:
 *
 * HELLO   guest -> payload: u32 version (WLSHIM_VERSION); cmd 0, or
 *                  WLSHIM_HELLO_QUERY to only list the radios.
 *         host  -> status 0, payload: struct wlshim_hello then
 *                  hello.nradio x struct wlshim_radio.
 *                  status < 0: no radios; the guest registers nothing.
 *         A plain HELLO after the first means the driver started again (a
 *         reboot): every radio returns to power-on. A query changes nothing.
 *
 * IOCTL   One SIOCDEVPRIVATE request on a radio interface.
 *         guest -> radio, bss (0 = the radio's primary interface, n = the
 *                  virtual interface the host announced with IF_ADD as bss n),
 *                  cmd, set, driver (0 for a wl_ioctl_t, else the dhd_ioctl_t
 *                  driver word, DHD_IOCTL_MAGIC for dhd), ulen = the caller's
 *                  buffer length, len = bytes of it carried (min(ulen,
 *                  8192): WLC_IOCTL_MAXLEN, the most dhd copies), payload =
 *                  those bytes.
 *         host  -> status = the ioctl's return value (0 or -errno), used,
 *                  needed, len = bytes to copy back to the caller's buffer
 *                  (at most ulen), payload = those bytes. On failure, driver
 *                  = the BCME code (negative) the error comes from, which a
 *                  dongle model passes on as the firmware would.
 *
 * TX      A frame the guest sent on a radio interface.
 *         guest -> radio, bss, len, payload = the Ethernet frame.
 *         host  -> status 0, len 0, more = 1 if a station (a host peer)
 *                  took the frame, so answers may follow: the guest then
 *                  polls promptly for a while.
 *
 * POLL    Collect queued host -> guest work.
 *         guest -> len 0; cmd 0 for every radio's records, or
 *                  WLSHIM_POLL_RADIO for only those of hdr.radio.
 *         host  -> payload = records (struct wlshim_rec + rec.len bytes,
 *                  each padded to 4), more = 1 if the host has more queued.
 *
 * RESET   One radio back to power-on (a dongle's firmware starting
 *         again); the others keep their state and its queued records go.
 *         guest -> radio. host -> status 0, or < 0 for no such radio.
 *
 * Record types (POLL reply):
 *   RX       radio, bss, payload = an Ethernet frame to deliver as received
 *            on that interface (data, or a Broadcom event frame, 0x886c).
 *   IF_ADD   radio, bss, payload = struct wlshim_if: create the virtual
 *            interface (as dhd does on WLC_E_IF).
 *   IF_DEL   radio, bss: remove it.
 *   CARRIER  radio, bss, payload = u32 0 or 1: netif_carrier_off/on.
 */
#ifndef WLSHIM_PROTO_H
#define WLSHIM_PROTO_H

#define WLSHIM_HC       0x776c7368u     /* "wlsh" */
#define WLSHIM_MAGIC    0x574c5348u     /* "WLSH" */
#define WLSHIM_EHOST    0xffffffffu
#define WLSHIM_VERSION  1
#define WLSHIM_BUF      16384           /* > WLC_IOCTL_MAXLEN (8192) + header */
#define WLSHIM_IFNAMSIZ 16

#define WLSHIM_HELLO    1
#define WLSHIM_IOCTL    2
#define WLSHIM_TX       3
#define WLSHIM_POLL     4
#define WLSHIM_RESET    5

#define WLSHIM_HELLO_QUERY  1           /* HELLO cmd: list the radios only */
#define WLSHIM_POLL_RADIO   1           /* POLL cmd: this radio's records only */

#define WLSHIM_REC_RX       1
#define WLSHIM_REC_IF_ADD   2
#define WLSHIM_REC_IF_DEL   3
#define WLSHIM_REC_CARRIER  4

struct wlshim_hdr {                     /* 64 bytes */
    unsigned int op;
    int status;
    unsigned int radio;
    unsigned int bss;
    unsigned int cmd;
    unsigned int set;
    unsigned int driver;
    unsigned int ulen;
    unsigned int len;                   /* payload bytes after the header */
    unsigned int used;
    unsigned int needed;
    unsigned int more;
    char comm[16];                      /* guest: the calling task's comm */
};

struct wlshim_hello {
    unsigned int version;
    unsigned int nradio;
};

struct wlshim_radio {                   /* 24 bytes */
    char ifname[WLSHIM_IFNAMSIZ];       /* "eth1" */
    unsigned char mac[6];
    unsigned char band;                 /* 2 = 2.4 GHz, 5 = 5 GHz */
    unsigned char unit;                 /* wl unit number: wl0, wl1 ... */
};

struct wlshim_if {                      /* 24 bytes */
    char ifname[WLSHIM_IFNAMSIZ];       /* "wl0.1" */
    unsigned char mac[6];
    unsigned char pad[2];
};

struct wlshim_rec {                     /* 8 bytes */
    unsigned short type;
    unsigned short len;                 /* payload bytes */
    unsigned char radio;
    unsigned char bss;
    unsigned short pad;
};

#define WLSHIM_ALIGN(n)  (((n) + 3u) & ~3u)

#endif
