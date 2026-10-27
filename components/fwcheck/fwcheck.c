/* SPDX-License-Identifier: GPL-3.0-only
 * Slipway Portable - firmware file checker. See fwcheck.h. */
#include <string.h>
#include <stdio.h>
#include "fwcheck.h"
#include "fm1proto.h"

const uint8_t FW_STOCK_SHA256[32] = {
    0xdb, 0x16, 0x42, 0xb2, 0xb6, 0xfa, 0x5c, 0x2c, 0xcc, 0xb1, 0x1f, 0xfd, 0x13, 0x87, 0x80, 0x68,
    0xbb, 0x28, 0x60, 0x16, 0x78, 0xd3, 0x64, 0x40, 0x49, 0xf9, 0x9d, 0xc4, 0x0e, 0x7e, 0xdb, 0x8a,
};

/* ------------------------------------------------------------------------------------ SHA-256 --- */
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(fw_sha256_t *c, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void fw_sha256_init(fw_sha256_t *c)
{
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c->h, iv, sizeof(iv));
    c->blen = 0; c->total = 0;
}

void fw_sha256_update(fw_sha256_t *c, const uint8_t *d, size_t n)
{
    c->total += n;
    while (n) {
        size_t take = 64 - c->blen; if (take > n) take = n;
        memcpy(c->buf + c->blen, d, take);
        c->blen += take; d += take; n -= take;
        if (c->blen == 64) { sha_block(c, c->buf); c->blen = 0; }
    }
}

void fw_sha256_final(fw_sha256_t *c, uint8_t out[32])
{
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80;
    fw_sha256_update(c, &pad, 1);
    uint8_t z = 0;
    while (c->blen != 56) fw_sha256_update(c, &z, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    fw_sha256_update(c, len, 8);
    for (int i = 0; i < 8; i++) {
        out[i*4] = (uint8_t)(c->h[i] >> 24); out[i*4+1] = (uint8_t)(c->h[i] >> 16);
        out[i*4+2] = (uint8_t)(c->h[i] >> 8); out[i*4+3] = (uint8_t)c->h[i];
    }
}

void fw_hex(const uint8_t *d, size_t n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[i*2] = hx[d[i] >> 4]; out[i*2+1] = hx[d[i] & 15]; }
    out[n*2] = 0;
}

/* ---------------------------------------------------------------------------------------- scan --- */
void fwscan_init(fwscan_t *s)
{
    memset(s, 0, sizeof(*s));
    fw_sha256_init(&s->sha);
}

/* KMP over FW_LOADER_MARKER, so the marker is found even if it straddles two chunks. */
static bool kmp_step(size_t *state, uint8_t byte)
{
    static const char *pat = FW_LOADER_MARKER;
    static int fail[sizeof(FW_LOADER_MARKER)];
    static bool ready = false;
    const size_t m = sizeof(FW_LOADER_MARKER) - 1;
    if (!ready) {
        fail[0] = 0;
        for (size_t i = 1, k = 0; i < m; i++) {
            while (k > 0 && pat[i] != pat[k]) k = (size_t)fail[k - 1];
            if (pat[i] == pat[k]) k++;
            fail[i] = (int)k;
        }
        ready = true;
    }
    size_t k = *state;
    while (k > 0 && (char)byte != pat[k]) k = (size_t)fail[k - 1];
    if ((char)byte == pat[k]) k++;
    if (k == m) { *state = (size_t)fail[m - 1]; return true; }
    *state = k;
    return false;
}

void fwscan_feed(fwscan_t *s, const uint8_t *d, size_t n)
{
    fw_sha256_update(&s->sha, d, n);
    if (s->head_len < FW_MARKER_WITHIN) {
        size_t take = FW_MARKER_WITHIN - s->head_len; if (take > n) take = n;
        memcpy(s->head + s->head_len, d, take);
        s->head_len += take;
    }
    if (!s->has_loader)
        for (size_t i = 0; i < n; i++)
            if (kmp_step(&s->kmp_state, d[i])) { s->has_loader = true; break; }
    s->total += n;
}

static bool product_ok(const char *p)           /* ^[^_]+_\d+$ */
{
    const char *u = strchr(p, '_');
    if (!u || u == p) return false;
    if (strchr(u + 1, '_')) return false;
    if (!u[1]) return false;
    for (const char *q = u + 1; *q; q++) if (*q < '0' || *q > '9') return false;
    return true;
}

static void refuse(fwcheck_result_t *r, const char *why)
{
    r->ok = false;
    snprintf(r->reason, sizeof(r->reason), "%s", why);
}

void fwscan_finish(fwscan_t *s, fwcheck_result_t *r)
{
    memset(r, 0, sizeof(*r));
    r->size = (uint32_t)(s->total > 0xFFFFFFFFu ? 0xFFFFFFFFu : s->total);
    r->has_loader = s->has_loader;
    fw_sha256_t c = s->sha;
    fw_sha256_final(&c, r->sha256);

    if (s->total < FW_MIN_BYTES || s->total > FW_MAX_BYTES) { refuse(r, "Wrong size for FM-1 firmware"); return; }
    /* the marker must be inside the first 4096 bytes */
    const size_t ml = sizeof(FW_MARKER) - 1;
    bool marker = false;
    for (size_t i = 0; i + ml <= s->head_len; i++)
        if (memcmp(s->head + i, FW_MARKER, ml) == 0) { marker = true; break; }
    if (!marker) { refuse(r, "Chip marker missing: not FM-1 firmware"); return; }
    if (!fm1_product_of(s->head, s->head_len, r->product, sizeof(r->product)) || !product_ok(r->product)) {
        r->product[0] = 0;
        refuse(r, "Product name in the header is unreadable");
        return;
    }
    r->official = (s->total == FW_STOCK_SIZE) && memcmp(r->sha256, FW_STOCK_SHA256, 32) == 0 &&
                  strcmp(r->product, FW_STOCK_PRODUCT) == 0;
    if (!r->has_loader && !r->official) { refuse(r, "No Felucca loader marker: the installer refuses it"); return; }
    r->ok = true;
}
