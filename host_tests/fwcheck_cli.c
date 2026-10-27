/* SPDX-License-Identifier: GPL-3.0-only
 * Test helper: runs the firmware checker over a file, feeding it in chunks of the given size. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fwcheck.h"

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s file chunk\n", argv[0]); return 2; }
    size_t chunk = (size_t)atol(argv[2]);
    if (!chunk) chunk = 1;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("error=cannot open\n"); return 1; }
    uint8_t *buf = malloc(chunk);
    fwscan_t s; fwscan_init(&s);
    size_t n;
    while ((n = fread(buf, 1, chunk, f)) > 0) fwscan_feed(&s, buf, n);
    fclose(f); free(buf);
    fwcheck_result_t r; fwscan_finish(&s, &r);
    char hex[65]; fw_hex(r.sha256, 32, hex);
    printf("ok=%d official=%d loader=%d size=%u product=%s sha=%s reason=%s\n",
           r.ok, r.official, r.has_loader, r.size, r.product, hex, r.reason);
    return 0;
}
