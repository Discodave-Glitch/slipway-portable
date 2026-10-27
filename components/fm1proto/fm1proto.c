/* SPDX-License-Identifier: GPL-3.0-only
 * See fm1proto.h for credits. */
#include "fm1proto.h"
#include <string.h>
#include <strings.h>

const uint8_t FM1_HS_QUERY[10] = {0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7};
const uint8_t FM1_UPGRADE[6]   = {0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7};

size_t fm1_pack7(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 0; uint32_t acc = 0; int nb = 0;
    for (size_t i = 0; i < n; i++) {
        acc |= (uint32_t)in[i] << nb;
        nb += 8;
        while (nb >= 7) { out[o++] = acc & 0x7F; acc >>= 7; nb -= 7; }
    }
    if (nb) out[o++] = acc & 0x7F;
    return o;
}

size_t fm1_unpack7(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 0; uint32_t acc = 0; int nb = 0;
    for (size_t i = 0; i < n; i++) {
        acc |= (uint32_t)(in[i] & 0x7F) << nb;
        nb += 7;
        while (nb >= 8) { out[o++] = acc & 0xFF; acc >>= 8; nb -= 8; }
    }
    return o;
}

bool fm1_parse_request(const uint8_t *pkt, size_t n, uint8_t *flags, uint32_t *addr, uint32_t *len)
{
    if (n < 2 || pkt[0] != 0xF0 || pkt[n - 1] != 0xF7) return false;
    uint8_t u[64];
    if (n - 2 > 64) return false;
    size_t m = fm1_unpack7(pkt + 1, n - 2, u);
    if (m != 15 || u[0] != 0x00 || u[1] != 0x59 || u[2] != 0x30) return false;
    uint8_t sum = 0;
    for (int i = 6; i < 14; i++) sum = (uint8_t)(sum + u[i]);
    if ((uint8_t)(~sum) != u[14]) return false;
    *flags = u[6];
    *addr = (uint32_t)u[7] | ((uint32_t)u[8] << 8) | ((uint32_t)u[9] << 16) | ((uint32_t)u[10] << 24);
    *len  = (uint32_t)u[11] | ((uint32_t)u[12] << 8) | ((uint32_t)u[13] << 16);
    return true;
}

size_t fm1_response_max(size_t len)
{
    size_t body = 7 + 4 + 3 + len + 1;          /* header(7) addr(4) len(3) data checksum(1) */
    return 2 + (body * 8 + 6) / 7;
}

size_t fm1_build_response(uint32_t addr, const uint8_t *data, size_t len, uint8_t flags, uint8_t *out, size_t out_cap)
{
    if (len > FM1_MAXDATA || out_cap < fm1_response_max(len)) return 0;
    uint8_t body[7 + 4 + 3 + FM1_MAXDATA + 1];
    size_t n = len + 8, k = 0;
    body[k++] = 0x00; body[k++] = 0x59; body[k++] = 0x30;
    body[k++] = (uint8_t)(n & 0xFF); body[k++] = (uint8_t)((n >> 8) & 0xFF);
    body[k++] = 0; body[k++] = flags;
    body[k++] = (uint8_t)addr; body[k++] = (uint8_t)(addr >> 8); body[k++] = (uint8_t)(addr >> 16); body[k++] = (uint8_t)(addr >> 24);
    body[k++] = (uint8_t)(len & 0xFF); body[k++] = (uint8_t)((len >> 8) & 0xFF); body[k++] = 0;
    memcpy(body + k, data, len); k += len;
    uint8_t sum = 0;
    for (size_t i = 6; i < k; i++) sum = (uint8_t)(sum + body[i]);
    body[k++] = (uint8_t)(~sum);
    out[0] = 0xF0;
    size_t p = fm1_pack7(body, k, out + 1);
    out[1 + p] = 0xF7;
    return p + 2;
}

bool fm1_parse_identity(const uint8_t *pkt, size_t n, fm1_identity_t *id)
{
    if (n < 2 || pkt[0] != 0xF0 || pkt[n - 1] != 0xF7 || n - 2 > 64) return false;
    uint8_t d[64];
    size_t m = fm1_unpack7(pkt + 1, n - 2, d);
    if (m != 34 || d[0] != 0x00 || d[1] != 0x59 || d[2] != 0x11) return false;
    /* d[6..32] text, NUL padded; keep printable ASCII only (anything else becomes '?') */
    size_t end = 33;
    while (end > 6 && d[end - 1] == 0) end--;
    char txt[28]; size_t t = 0;
    for (size_t i = 6; i < end && t < sizeof(txt) - 1; i++) txt[t++] = (d[i] >= 32 && d[i] < 127) ? (char)d[i] : '?';
    txt[t] = 0;
    /* ([^_]+)_(\d+) must match the whole text */
    const char *us = strchr(txt, '_');
    if (!us || us == txt) return false;
    const char *dg = us + 1;
    if (!*dg) return false;
    int ver = 0;
    for (const char *c = dg; *c; c++) {
        if (*c < '0' || *c > '9') return false;
        ver = ver * 10 + (*c - '0');
        if (ver > 1000000) return false;
    }
    if (strchr(us + 1, '_')) return false;   /* [^_]+ then _ then digits only */
    memset(id, 0, sizeof(*id));
    strncpy(id->text, txt, sizeof(id->text) - 1);
    size_t ml = (size_t)(us - txt);
    if (ml >= sizeof(id->model)) ml = sizeof(id->model) - 1;
    memcpy(id->model, txt, ml);
    id->version = ver;
    id->loader = (strncasecmp(id->model, "ota-", 4) == 0);
    return true;
}

bool fm1_product_of(const uint8_t *raw, size_t n, char *out, size_t cap)
{
    if (n < FM1_BLOCKS * FM1_BLK || cap < FM1_BLOCKS + 1) return false;
    size_t o = 0;
    for (unsigned i = 0; i < FM1_BLOCKS; i++) {
        uint8_t m = raw[i * FM1_BLK + FM1_KEEP];
        if (m != 0x7D) out[o++] = (char)(uint8_t)((m - i - 1) & 0xFF);
    }
    out[o] = 0;
    return true;
}

size_t fm1_logical_size(size_t raw_size)
{
    return raw_size >= FM1_BLOCKS * FM1_BLK ? raw_size - FM1_BLOCKS : 0;
}

bool fm1_read_logical(size_t raw_size, uint32_t addr, size_t len, uint8_t *out, fm1_read_raw_fn read_raw, void *ctx)
{
    size_t lsize = fm1_logical_size(raw_size);
    if ((size_t)addr + len > lsize) return false;
    const size_t head_logical = FM1_BLOCKS * FM1_KEEP;   /* 940 bytes of header, spread over 20 blocks */
    size_t pos = addr, left = len;
    while (left) {
        size_t raw_off, run;
        if (pos < head_logical) {
            size_t blk = pos / FM1_KEEP, off = pos % FM1_KEEP;
            raw_off = blk * FM1_BLK + off;
            run = FM1_KEEP - off;
        } else {
            raw_off = pos + FM1_BLOCKS;
            run = left;
        }
        if (run > left) run = left;
        if (!read_raw(ctx, raw_off, out, run)) return false;
        out += run; pos += run; left -= run;
    }
    return true;
}

size_t fm1_usbmidi_encode_sysex(const uint8_t *s, size_t n, uint8_t cable, uint8_t *out, size_t cap)
{
    size_t o = 0, i = 0;
    while (i < n) {
        size_t rem = n - i;
        if (o + 4 > cap) return 0;
        uint8_t hdr = (uint8_t)(cable << 4);
        if (rem >= 3) {
            bool last = (rem == 3);                      /* exactly 3 left: they end the message */
            out[o++] = hdr | (last ? 0x7 : 0x4);
            out[o++] = s[i]; out[o++] = s[i + 1]; out[o++] = s[i + 2];
            i += 3;
        } else if (rem == 2) {
            out[o++] = hdr | 0x6; out[o++] = s[i]; out[o++] = s[i + 1]; out[o++] = 0; i += 2;
        } else {
            out[o++] = hdr | 0x5; out[o++] = s[i]; out[o++] = 0; out[o++] = 0; i += 1;
        }
    }
    return o;
}

void fm1_sysex_rx_init(fm1_sysex_rx_t *rx)
{
    rx->len = 0; rx->active = false; rx->overflow = false;
}

static void rx_byte(fm1_sysex_rx_t *rx, uint8_t b, fm1_sysex_done_fn done, void *ctx)
{
    if (b == 0xF0) { rx->len = 0; rx->active = true; rx->overflow = false; }
    if (!rx->active) return;
    if (rx->len < sizeof(rx->buf)) rx->buf[rx->len++] = b; else rx->overflow = true;
    if (b == 0xF7) {
        rx->active = false;
        if (!rx->overflow && done) done(ctx, rx->buf, rx->len);
        rx->len = 0;
    }
}

void fm1_sysex_rx_feed(fm1_sysex_rx_t *rx, const uint8_t *d, size_t n, fm1_sysex_done_fn done, void *ctx)
{
    for (size_t i = 0; i + 4 <= n; i += 4) {
        uint8_t cin = d[i] & 0x0F;
        size_t cnt;
        switch (cin) {
        case 0x4: case 0x7: cnt = 3; break;      /* SysEx starts/continues, or ends with 3 bytes */
        case 0x6: cnt = 2; break;                /* ends with 2 bytes */
        case 0x5: cnt = 1; break;                /* ends with 1 byte */
        default: cnt = 0; break;                 /* anything else is not SysEx: ignore */
        }
        for (size_t k = 0; k < cnt; k++) rx_byte(rx, d[i + 1 + k], done, ctx);
    }
}
