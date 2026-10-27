/* SPDX-License-Identifier: GPL-3.0-only
 * Host tests for the installer's small modules: the stock record file (sdstate.c) and the update-from-SD logic (sdupdate.c,
 * with ESP-IDF's OTA calls replaced by recorders). */
#include "sdstate.h"
#include "sdupdate.h"
#include "esp_ota_ops.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(name, cond) do { int ok_ = (cond); printf("%s  %s\n", ok_ ? "ok  " : "FAIL", name); if (!ok_) fails++; } while (0)

/* ---- recorders for the OTA calls ---- */
static esp_partition_t part = { 3 * 1024 * 1024 };
static uint8_t *written; static size_t written_n; static int began, ended, aborted, set_boot;
static int fail_write_at = -1;
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *s) { (void)s; return &part; }
esp_err_t esp_ota_begin(const esp_partition_t *p, size_t sz, esp_ota_handle_t *h) { (void)p; began++; free(written); written = malloc(sz); written_n = 0; *h = 1; return ESP_OK; }
esp_err_t esp_ota_write(esp_ota_handle_t h, const void *d, size_t n) {
    (void)h; if (fail_write_at >= 0 && (int)written_n >= fail_write_at) return ESP_FAIL;
    memcpy(written + written_n, d, n); written_n += n; return ESP_OK; }
esp_err_t esp_ota_end(esp_ota_handle_t h) { (void)h; ended++; return ESP_OK; }
esp_err_t esp_ota_abort(esp_ota_handle_t h) { (void)h; aborted++; return ESP_OK; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p) { (void)p; set_boot++; return ESP_OK; }

static void make_image(uint8_t *b, size_t n, const char *name)
{
    memset(b, 0x5a, n);
    b[0] = 0xE9;
    b[32] = 0x32; b[33] = 0x54; b[34] = 0xCD; b[35] = 0xAB;      /* 0xABCD5432, little endian */
    memset(b + 80, 0, 32); strncpy((char *)b + 80, name, 31);
}

static char *write_tmp(const uint8_t *d, size_t n)
{
    static char path[64]; strcpy(path, "/tmp/slipway_test_XXXXXX");
    int fd = mkstemp(path); if (fd < 0) { perror("mkstemp"); exit(2); }
    if (write(fd, d, n) != (ssize_t)n) exit(2);
    close(fd); return path;
}

int main(void)
{
    /* ---- sdstate ---- */
    char sp[64]; strcpy(sp, "/tmp/slipway_state_XXXXXX"); close(mkstemp(sp));
    stockrec_t a, b;
    memset(&a, 0, sizeof a); a.have = true;
    strcpy(a.path, "/sdcard/firmware/FM-1 (V16).fwsc");
    strcpy(a.sha, "d017fb65e70aca97ee29d944470c3515a74581df5c0576bf70de917c7b2b61d5");
    CHECK("state: save", sdstate_save(sp, &a));
    CHECK("state: load gives the same record (path with spaces and brackets)", sdstate_load(sp, &b) && b.have && !strcmp(b.path, a.path) && !strcmp(b.sha, a.sha));
    unlink(sp);
    CHECK("state: missing file -> no record", !sdstate_load(sp, &b) && !b.have);
    FILE *f = fopen(sp, "w"); fputs("stock=/sdcard/x.fwsc\nsha=1234\n", f); fclose(f);
    CHECK("state: a short/invalid sha is not accepted", !sdstate_load(sp, &b) && !b.have);
    f = fopen(sp, "w"); fputs("garbage\r\nstock=/sdcard/y.fwsc\r\nsha=d017fb65e70aca97ee29d944470c3515a74581df5c0576bf70de917c7b2b61d5\r\n", f); fclose(f);
    CHECK("state: CRLF and junk lines are tolerated", sdstate_load(sp, &b) && !strcmp(b.path, "/sdcard/y.fwsc"));
    unlink(sp);
    CHECK("state: unwritable location fails cleanly", !sdstate_save("/nonexistent_dir/x.state", &a));

    /* ---- sdupdate: head check ---- */
    uint8_t img[200000];
    make_image(img, sizeof img, "slipway_installer");
    CHECK("update: a Slipway installer image passes the head check", sdupdate_check_head(img, 128) == UPD_OK);
    make_image(img, sizeof img, "some_other_app");
    CHECK("update: another app is refused", sdupdate_check_head(img, 128) == UPD_WRONGAPP);
    make_image(img, sizeof img, "slipway_installer"); img[0] = 0x00;
    CHECK("update: not an ESP image is refused", sdupdate_check_head(img, 128) == UPD_NOTAPP);
    make_image(img, sizeof img, "slipway_installer"); img[33] = 0;
    CHECK("update: missing app description magic is refused", sdupdate_check_head(img, 128) == UPD_NOTAPP);
    CHECK("update: a truncated head is refused", sdupdate_check_head(img, 50) == UPD_NOTAPP);
    make_image(img, sizeof img, "slipway_installer_extra");
    CHECK("update: a similar-looking name is refused", sdupdate_check_head(img, 128) == UPD_WRONGAPP);

    /* ---- sdupdate: apply ---- */
    make_image(img, sizeof img, "slipway_installer");
    char *p = write_tmp(img, sizeof img);
    upd_t e = sdupdate_apply(p, NULL);
    CHECK("update: good file -> OK, written completely, ended, boot slot set", e == UPD_OK && written_n == sizeof img && !memcmp(written, img, sizeof img) && ended == 1 && set_boot == 1);
    unlink(p);

    began = ended = set_boot = aborted = 0;
    make_image(img, sizeof img, "wrong_app");
    p = write_tmp(img, sizeof img);
    e = sdupdate_apply(p, NULL);
    CHECK("update: wrong app -> refused before anything is written", e == UPD_WRONGAPP && began == 0 && set_boot == 0);
    unlink(p);

    make_image(img, sizeof img, "slipway_installer");
    p = write_tmp(img, sizeof img);
    fail_write_at = 50000;
    e = sdupdate_apply(p, NULL);
    CHECK("update: write error -> aborted, boot slot NOT changed", e == UPD_WRITE && aborted == 1 && set_boot == 0);
    fail_write_at = -1; unlink(p);

    static uint8_t big[4 * 1024 * 1024];
    make_image(big, sizeof big, "slipway_installer");
    p = write_tmp(big, sizeof big);
    began = set_boot = 0;
    e = sdupdate_apply(p, NULL);
    CHECK("update: a file bigger than the slot is refused", e == UPD_SIZE && began == 0 && set_boot == 0);
    unlink(p);
    CHECK("update: missing file", sdupdate_apply("/tmp/slipway_no_such_file.bin", NULL) == UPD_OPEN);

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
