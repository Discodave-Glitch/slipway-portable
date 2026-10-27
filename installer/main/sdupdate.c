/* SPDX-License-Identifier: GPL-3.0-only */
#include "sdupdate.h"
#include <stdio.h>
#include <string.h>
#include "esp_ota_ops.h"

#define APP_NAME "slipway_installer"

upd_t sdupdate_check_head(const uint8_t *h, unsigned n)
{
    /* esp_image_header_t (24 bytes) + esp_image_segment_header_t (8 bytes) + esp_app_desc_t:
     * magic_word u32 @32, secure_version u32, reserv1 u32[2], version char[32] @48, project_name char[32] @80 */
    if (n < 112 || h[0] != 0xE9) return UPD_NOTAPP;
    uint32_t magic = (uint32_t)h[32] | ((uint32_t)h[33] << 8) | ((uint32_t)h[34] << 16) | ((uint32_t)h[35] << 24);
    if (magic != 0xABCD5432u) return UPD_NOTAPP;
    char name[33]; memcpy(name, h + 80, 32); name[32] = 0;
    if (strcmp(name, APP_NAME) != 0) return UPD_WRONGAPP;
    return UPD_OK;
}

upd_t sdupdate_apply(const char *path, void (*progress)(int percent))
{
    FILE *f = fopen(path, "rb");
    if (!f) return UPD_OPEN;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t head[128];
    size_t hn = fread(head, 1, sizeof(head), f);
    upd_t c = sdupdate_check_head(head, (unsigned)hn);
    if (c != UPD_OK) { fclose(f); return c; }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || size <= 0 || (size_t)size > part->size) { fclose(f); return UPD_SIZE; }
    esp_ota_handle_t h;
    if (esp_ota_begin(part, (size_t)size, &h) != ESP_OK) { fclose(f); return UPD_WRITE; }
    fseek(f, 0, SEEK_SET);
    static uint8_t buf[4096];
    long done = 0; size_t n; int last = -1;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (esp_ota_write(h, buf, n) != ESP_OK) { esp_ota_abort(h); fclose(f); return UPD_WRITE; }
        done += (long)n;
        int pct = (int)(done * 100 / size);
        if (progress && pct != last) { last = pct; progress(pct); }
    }
    bool rd_err = ferror(f);
    fclose(f);
    if (rd_err || done != size) { esp_ota_abort(h); return UPD_WRITE; }
    if (esp_ota_end(h) != ESP_OK) return UPD_END;               /* checks the whole image */
    if (esp_ota_set_boot_partition(part) != ESP_OK) return UPD_SETBOOT;
    return UPD_OK;
}

const char *sdupdate_text(upd_t e)
{
    switch (e) {
    case UPD_OK:       return "Updated.";
    case UPD_OPEN:     return "Could not open the file.";
    case UPD_NOTAPP:   return "That is not an ESP32 app file.";
    case UPD_WRONGAPP: return "That is not a Slipway Portable\ninstaller file.";
    case UPD_SIZE:     return "The file does not fit.";
    case UPD_WRITE:    return "Writing the update failed.";
    case UPD_END:      return "The update file is damaged.";
    case UPD_SETBOOT:  return "Could not select the new version.";
    }
    return "Update failed.";
}
