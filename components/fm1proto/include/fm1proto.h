/* SPDX-License-Identifier: GPL-3.0-only
 * FM-1 USB-MIDI update protocol helpers for Slipway Portable.
 * A C port of the pure (no I/O) parts of tools/fm1_install.py and web/fm1ota.js from the Felucca project
 * (GPL-3.0, Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments).
 * Nothing in this file talks to hardware or writes anything. */
#ifndef FM1PROTO_H
#define FM1PROTO_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FM1_MAXDATA     512u          /* most bytes the FM-1 asks for in one read request */
#define FM1_BLOCKS      20u           /* package header: 20 blocks ... */
#define FM1_BLK         0x30u         /* ... of 0x30 bytes, the last byte of each is a marker */
#define FM1_KEEP        0x2Fu         /* bytes kept from each of those blocks */
#define FM1_FINISH_CHECK 0xE0000000u
#define FM1_FINISH_WRITE 0xF0000000u

extern const uint8_t FM1_HS_QUERY[10];   /* F0 00 32 45 00 00 00 40 7F F7: "who are you" */
extern const uint8_t FM1_UPGRADE[6];     /* F0 22 24 35 7F F7: start the update (NEVER sent by the link test) */

/* 8-bit bytes <-> 7-bit MIDI-safe bytes (little-endian bit packing, as in the Python tool) */
size_t fm1_pack7(const uint8_t *in, size_t n, uint8_t *out);      /* out needs (n*8+6)/7 bytes */
size_t fm1_unpack7(const uint8_t *in, size_t n, uint8_t *out);    /* out needs n*7/8 bytes     */

/* A read request from the FM-1: a full SysEx packet F0 .. F7 */
bool fm1_parse_request(const uint8_t *pkt, size_t n, uint8_t *flags, uint32_t *addr, uint32_t *len);

/* Builds the answer to a read request. Returns the packet length (F0 .. F7), or 0 if it does not fit.
 * out needs at least fm1_response_max(len) bytes. */
size_t fm1_response_max(size_t len);
size_t fm1_build_response(uint32_t addr, const uint8_t *data, size_t len, uint8_t flags, uint8_t *out, size_t out_cap);

/* The device identity, e.g. "FM-1_015" -> model "FM-1", version 15. loader = model starts with "ota-". */
typedef struct {
    char text[28];
    char model[28];
    int  version;
    bool loader;
} fm1_identity_t;
bool fm1_parse_identity(const uint8_t *pkt, size_t n, fm1_identity_t *id);

/* Package (.fwsc) helpers. raw = the file as stored. */
bool   fm1_product_of(const uint8_t *raw_head, size_t head_len, char *out, size_t out_cap);  /* needs the first 960 bytes */
size_t fm1_logical_size(size_t raw_size);                                  /* raw_size minus the 20 marker bytes */
/* Reads `len` bytes of the logical image starting at `addr`, by calling read_raw for the matching raw ranges.
 * read_raw(ctx, raw_offset, buf, n) must return true on success. Returns true if all bytes were produced. */
typedef bool (*fm1_read_raw_fn)(void *ctx, size_t raw_offset, uint8_t *buf, size_t n);
bool fm1_read_logical(size_t raw_size, uint32_t addr, size_t len, uint8_t *out, fm1_read_raw_fn read_raw, void *ctx);

/* USB-MIDI class packets (4 bytes each: cable/CIN + 3 MIDI bytes). SysEx only. */
size_t fm1_usbmidi_encode_sysex(const uint8_t *sysex, size_t n, uint8_t cable, uint8_t *out, size_t out_cap);

typedef struct {
    uint8_t  buf[1100];     /* longest packet we expect: 512 data bytes packed to 7 bits plus the header */
    size_t   len;
    bool     active;
    bool     overflow;
} fm1_sysex_rx_t;
void fm1_sysex_rx_init(fm1_sysex_rx_t *rx);
/* Feed raw bytes received from the USB-MIDI IN endpoint (any multiple of 4). Calls `done` once for every
 * complete F0..F7 packet. */
typedef void (*fm1_sysex_done_fn)(void *ctx, const uint8_t *pkt, size_t n);
void fm1_sysex_rx_feed(fm1_sysex_rx_t *rx, const uint8_t *data, size_t n, fm1_sysex_done_fn done, void *ctx);

#ifdef __cplusplus
}
#endif
#endif
