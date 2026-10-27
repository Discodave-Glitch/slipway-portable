/* SPDX-License-Identifier: GPL-3.0-only
 * Slipway Portable - firmware file checker (pure C, no I/O, no hardware).
 * The same rules as Slipway for Windows/Mac (slipway/library.py check_file + flasher.py can_flash) and the web app:
 *   - size between 100 KB and 4 MB
 *   - the chip marker "AC791N" within the first 4096 bytes
 *   - a product name in the header (e.g. FM-1_015) shaped like  name_number
 *   - the Felucca loader marker "FELUCCA-LOADER-1" somewhere in the file, unless it is exactly the official V15
 *   - "official V15" means: 699,956 bytes, SHA-256 db1642b2...edb8a, product FM-1_015
 * Feed the file in pieces (any size) with fwscan_feed, then call fwscan_finish. Nothing here writes anything. */
#ifndef FWCHECK_H
#define FWCHECK_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FW_MIN_BYTES   (100u * 1024u)
#define FW_MAX_BYTES   (4u * 1024u * 1024u)
#define FW_MARKER      "AC791N"
#define FW_MARKER_WITHIN 4096u
#define FW_LOADER_MARKER "FELUCCA-LOADER-1"
#define FW_STOCK_SIZE  699956u
#define FW_STOCK_PRODUCT "FM-1_015"
extern const uint8_t FW_STOCK_SHA256[32];

/* SHA-256, streaming */
typedef struct { uint32_t h[8]; uint8_t buf[64]; size_t blen; uint64_t total; } fw_sha256_t;
void fw_sha256_init(fw_sha256_t *c);
void fw_sha256_update(fw_sha256_t *c, const uint8_t *d, size_t n);
void fw_sha256_final(fw_sha256_t *c, uint8_t out[32]);

typedef struct {
    fw_sha256_t sha;
    uint64_t    total;
    uint8_t     head[FW_MARKER_WITHIN];
    size_t      head_len;
    size_t      kmp_state;      /* progress matching FW_LOADER_MARKER across chunk boundaries */
    bool        has_loader;
} fwscan_t;

typedef struct {
    bool     ok;              /* may be installed (still needs the stock-first rule) */
    bool     official;        /* exactly the official V15 */
    bool     has_loader;
    uint32_t size;
    char     product[28];     /* e.g. FM-1_900 ("" if unreadable) */
    uint8_t  sha256[32];
    char     reason[56];      /* plain-English reason when ok is false */
} fwcheck_result_t;

void fwscan_init(fwscan_t *s);
void fwscan_feed(fwscan_t *s, const uint8_t *d, size_t n);
void fwscan_finish(fwscan_t *s, fwcheck_result_t *r);
void fw_hex(const uint8_t *d, size_t n, char *out);   /* out needs 2n+1 bytes */

#ifdef __cplusplus
}
#endif
#endif
