/* SPDX-License-Identifier: GPL-3.0-only
 * Tiny command line wrapper around fm1proto, so the Python test can compare it with Felucca's own Python code. */
#include "fm1proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t from_hex(const char *h, uint8_t *out) {
    size_t n = 0;
    while (h[0] && h[1]) { unsigned v; sscanf(h, "%2x", &v); out[n++] = (uint8_t)v; h += 2; }
    return n;
}
static void to_hex(const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) printf("%02x", b[i]); printf("\n"); }

static bool file_read(void *ctx, size_t off, uint8_t *buf, size_t n) {
    FILE *f = ctx;
    if (fseek(f, (long)off, SEEK_SET)) return false;
    return fread(buf, 1, n, f) == n;
}
static void on_pkt(void *ctx, const uint8_t *p, size_t n) { (void)ctx; printf("pkt "); to_hex(p, n); }

int main(int argc, char **argv) {
    static uint8_t a[8192], b[8192];
    if (argc < 2) return 2;
    const char *c = argv[1];
    if (!strcmp(c, "pack7"))   { size_t n = from_hex(argv[2], a); to_hex(b, fm1_pack7(a, n, b)); }
    else if (!strcmp(c, "unpack7")) { size_t n = from_hex(argv[2], a); to_hex(b, fm1_unpack7(a, n, b)); }
    else if (!strcmp(c, "response")) {   /* addr flags datahex */
        size_t n = from_hex(argv[4], a);
        size_t m = fm1_build_response((uint32_t)strtoul(argv[2], 0, 0), a, n, (uint8_t)strtoul(argv[3], 0, 0), b, sizeof b);
        if (!m) { printf("fail\n"); return 1; } to_hex(b, m);
    }
    else if (!strcmp(c, "request")) {
        size_t n = from_hex(argv[2], a); uint8_t fl; uint32_t ad, ln;
        if (fm1_parse_request(a, n, &fl, &ad, &ln)) printf("%u %u %u\n", fl, ad, ln); else printf("none\n");
    }
    else if (!strcmp(c, "identity")) {
        size_t n = from_hex(argv[2], a); fm1_identity_t id;
        if (fm1_parse_identity(a, n, &id)) printf("%s %s %d %d\n", id.text, id.model, id.version, id.loader); else printf("none\n");
    }
    else if (!strcmp(c, "product")) {   /* file */
        FILE *f = fopen(argv[2], "rb"); if (!f) return 3;
        size_t n = fread(a, 1, 960, f); fclose(f);
        char out[40]; if (fm1_product_of(a, n, out, sizeof out)) printf("%s\n", out); else printf("none\n");
    }
    else if (!strcmp(c, "logical")) {   /* file addr len */
        FILE *f = fopen(argv[2], "rb"); if (!f) return 3;
        fseek(f, 0, SEEK_END); size_t raw = (size_t)ftell(f);
        size_t len = (size_t)strtoul(argv[4], 0, 0);
        if (len > sizeof b) return 4;
        if (!fm1_read_logical(raw, (uint32_t)strtoul(argv[3], 0, 0), len, b, file_read, f)) { printf("fail\n"); return 1; }
        to_hex(b, len); fclose(f);
    }
    else if (!strcmp(c, "lsize")) { printf("%zu\n", fm1_logical_size((size_t)strtoul(argv[2], 0, 0))); }
    else if (!strcmp(c, "usbenc")) { size_t n = from_hex(argv[2], a); to_hex(b, fm1_usbmidi_encode_sysex(a, n, 0, b, sizeof b)); }
    else if (!strcmp(c, "usbdec")) {   /* hex, optionally split in chunks of 4*k bytes: arg3 = chunk size in bytes */
        size_t n = from_hex(argv[2], a); size_t ch = argc > 3 ? (size_t)atoi(argv[3]) : n;
        fm1_sysex_rx_t rx; fm1_sysex_rx_init(&rx);
        for (size_t i = 0; i < n; i += ch) fm1_sysex_rx_feed(&rx, a + i, (i + ch <= n ? ch : n - i), on_pkt, NULL);
    }
    else return 2;
    return 0;
}
