/*
 * igloo bridge wire protocol: TCP streams between host and guest carried over
 * one guest-polled hypercall, as a stand-in for vsock on kernels that lack it.
 * Shared verbatim by the guest module (portal/kl_bridge.c) and the host side
 * (QEMU hw/misc/igloo_bridge.c).
 *
 * The guest calls, in a loop,
 *     r7 = IGLOO_BRIDGE_HC, r0 = IGLOO_BRIDGE_MAGIC,
 *     r1 = tx physical address, r2 = tx length, r3 = rx physical address
 * The host consumes the tx records, writes at most IGLOO_BRIDGE_BUF bytes of
 * rx records and returns their length in r0. With no host attached the call
 * returns r0 unchanged (IGLOO_BRIDGE_MAGIC), and the guest backs off.
 *
 * Records are a header plus len payload bytes, padded to 4. All fields are
 * little-endian except the address in H_OPEN, which is network order.
 *
 * Flow control is per connection and per direction: each side may have at
 * most IGLOO_BRIDGE_WIN bytes in flight to the other, and the receiver
 * returns credit with ACK records as it writes the bytes to its socket.
 *
 * Close is a half close. H_CLOSE means the host client sent EOF (the guest
 * shuts down its socket for writing once its queue drains); G_CLOSED means
 * the guest socket hit EOF. A connection is freed on each side once both have
 * happened. Either record with IGLOO_BRIDGE_RST aborts the connection.
 */
#ifndef IGLOO_BRIDGE_PROTO_H
#define IGLOO_BRIDGE_PROTO_H

#define IGLOO_BRIDGE_HC     0x62726467u     /* "brdg" */
#define IGLOO_BRIDGE_MAGIC  0x42524447u     /* "BRDG" */
#define IGLOO_BRIDGE_BUF    65536           /* tx and rx buffer size */
#define IGLOO_BRIDGE_WIN    65536           /* per-connection window */
#define IGLOO_BRIDGE_CHUNK  16384           /* max payload of one DATA record */

/* guest -> host */
#define IGLOO_BRIDGE_G_OPENED  0x01         /* payload: s32 status (0 or -errno) */
#define IGLOO_BRIDGE_G_DATA    0x02
#define IGLOO_BRIDGE_G_CLOSED  0x03
#define IGLOO_BRIDGE_G_ACK     0x04         /* payload: u32 bytes written */

/* host -> guest */
#define IGLOO_BRIDGE_H_OPEN    0x81         /* payload: struct igloo_bridge_open */
#define IGLOO_BRIDGE_H_DATA    0x82
#define IGLOO_BRIDGE_H_CLOSE   0x83
#define IGLOO_BRIDGE_H_ACK     0x84         /* payload: u32 bytes written */

#define IGLOO_BRIDGE_RST       0x01         /* flags on CLOSE/CLOSED: abort */

struct igloo_bridge_rec {
    unsigned char type;
    unsigned char flags;
    unsigned short len;                     /* payload bytes */
    unsigned int conn;
};

struct igloo_bridge_open {
    unsigned int addr;                      /* IPv4, network order */
    unsigned short port;                    /* network order */
    unsigned char proto;                    /* 6 = TCP (only TCP for now) */
    unsigned char pad;
};

#define IGLOO_BRIDGE_ALIGN(n)  (((n) + 3u) & ~3u)

#endif
