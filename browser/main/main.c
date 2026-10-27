/* SPDX-License-Identifier: GPL-3.0-only
 * Slipway Portable - stage 1: firmware browser.
 *
 * READ-ONLY. Lists the .fwsc files on the SD card (in the card's top folder and in a folder called "firmware"),
 * and on OK checks the highlighted one with the same rules as Slipway (size, chip marker, product name, Felucca
 * loader marker, official V15 fingerprint). It never uses USB, never talks to an FM-1 and never writes to the card.
 *
 * Buttons: UP / DW move, OK checks the file, MENU looks at the card again.
 * Board: Espressif ESP32-S3-USB-OTG (or a compatible copy), via Espressif's board support package.
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "fwcheck.h"

static const char *TAG = "browser";

/* ------------------------------------------------------------------------------------- screen --- */
#define COLS 36
static lv_obj_t *s_title, *s_body;

static void screen_set(const char *title, const char *body)
{
    if (bsp_display_lock(500)) {
        lv_label_set_text(s_title, title);
        lv_label_set_text(s_body, body);
        bsp_display_unlock();
    }
}

static void screen_start(void)
{
    bsp_display_start();
    bsp_display_backlight_on();
    bsp_display_lock(0);
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x10243a), 0);
    s_title = lv_label_create(scr);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xd9b36c), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 4, 2);
    s_body = lv_label_create(scr);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_body, BSP_LCD_H_RES - 8);
    lv_obj_set_style_text_color(s_body, lv_color_hex(0xf2f2f2), 0);
    lv_obj_align(s_body, LV_ALIGN_TOP_LEFT, 4, 42);
    bsp_display_unlock();
}

/* ------------------------------------------------------------------------------------ buttons --- */
typedef enum { B_NONE, B_UP, B_DW, B_OK, B_MENU } button_t;

static void buttons_init(void)
{
    const gpio_num_t pins[] = { BSP_BUTTON_UP_IO, BSP_BUTTON_DW_IO, BSP_BUTTON_OK_IO, BSP_BUTTON_MENU_IO };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        gpio_config_t c = {
            .pin_bit_mask = 1ULL << pins[i], .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&c);
    }
}

/* Waits for one press (the buttons pull the pin to ground). Returns after the button has been released. */
static button_t wait_button(void)
{
    static const struct { gpio_num_t io; button_t b; } map[] = {
        { BSP_BUTTON_UP_IO, B_UP }, { BSP_BUTTON_DW_IO, B_DW }, { BSP_BUTTON_OK_IO, B_OK }, { BSP_BUTTON_MENU_IO, B_MENU },
    };
    int held = 0; button_t which = B_NONE;
    while (1) {
        button_t now = B_NONE;
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
            if (gpio_get_level(map[i].io) == 0) { now = map[i].b; break; }
        if (now != B_NONE && now == which) {
            if (++held == 2) {                              /* pressed for ~40 ms: a real press */
                while (1) {                                 /* wait for release */
                    vTaskDelay(pdMS_TO_TICKS(20));
                    bool down = false;
                    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) if (gpio_get_level(map[i].io) == 0) down = true;
                    if (!down) return which;
                }
            }
        } else { which = now; held = (now == B_NONE) ? 0 : 1; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ------------------------------------------------------------------------------------- files --- */
#define MAX_FILES 40
typedef struct { char path[160]; const char *name; uint32_t size; } entry_t;
static entry_t s_files[MAX_FILES];
static int s_count;

static bool is_fwsc(const char *n)
{
    size_t l = strlen(n);
    return l > 5 && strcasecmp(n + l - 5, ".fwsc") == 0;
}

static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_count < MAX_FILES) {
        if (e->d_type == DT_DIR || !is_fwsc(e->d_name) || e->d_name[0] == '.') continue;
        entry_t *f = &s_files[s_count];
        if (snprintf(f->path, sizeof(f->path), "%s/%s", dir, e->d_name) >= (int)sizeof(f->path)) continue;
        f->name = f->path + strlen(dir) + 1;
        FILE *fp = fopen(f->path, "rb");
        if (!fp) continue;
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fclose(fp);
        f->size = sz < 0 ? 0 : (uint32_t)sz;
        s_count++;
    }
    closedir(d);
}

static bool s_mounted;

static bool card_scan(void)
{
    s_count = 0;
    if (!s_mounted) {
        if (bsp_sdcard_mount() != ESP_OK) return false;
        s_mounted = true;
    }
    scan_dir(BSP_SD_MOUNT_POINT);
    char sub[40];
    snprintf(sub, sizeof(sub), "%s/firmware", BSP_SD_MOUNT_POINT);
    scan_dir(sub);
    return true;
}

static void shorten(const char *in, char *out, size_t cols)
{
    size_t l = strlen(in);
    if (l <= cols) { strcpy(out, in); return; }
    memcpy(out, in, cols - 3);
    strcpy(out + cols - 3, "...");
}

static void show_list(int sel)
{
    char body[600] = {0}, line[96], nm[COLS + 1];
    const int rows = 7;
    int top = sel - rows / 2; if (top > s_count - rows) top = s_count - rows; if (top < 0) top = 0;
    for (int i = top; i < s_count && i < top + rows; i++) {
        shorten(s_files[i].name, nm, COLS - 2);
        snprintf(line, sizeof(line), "%c %s\n", i == sel ? '>' : ' ', nm);
        strcat(body, line);
    }
    snprintf(line, sizeof(line), "\n%d of %d   UP/DW move, OK checks", sel + 1, s_count);
    strcat(body, line);
    screen_set("Slipway: files on the SD card\nREAD-ONLY: writes nothing", body);
}

/* ------------------------------------------------------------------------------------- check --- */
static fwscan_t s_scan;

static bool check_file(const entry_t *f, fwcheck_result_t *r)
{
    FILE *fp = fopen(f->path, "rb");
    if (!fp) return false;
    uint8_t *buf = malloc(4096);
    if (!buf) { fclose(fp); return false; }
    fwscan_init(&s_scan);
    size_t n;
    while ((n = fread(buf, 1, 4096, fp)) > 0) fwscan_feed(&s_scan, buf, n);
    bool err = ferror(fp);
    fclose(fp); free(buf);
    if (err) return false;
    fwscan_finish(&s_scan, r);
    return true;
}

static void show_result(const entry_t *f, const fwcheck_result_t *r)
{
    char body[700], nm[COLS + 1], hex[65], line[96];
    shorten(f->name, nm, COLS - 2);
    snprintf(body, sizeof(body), "%s\n%u bytes (%u KB)\n", nm, (unsigned)r->size, (unsigned)(r->size / 1024));
    if (r->product[0]) { snprintf(line, sizeof(line), "Product: %s\n", r->product); strcat(body, line); }
    fw_hex(r->sha256, 6, hex);
    snprintf(line, sizeof(line), "Fingerprint: %s...\n\n", hex); strcat(body, line);
    if (r->official)      strcat(body, "OFFICIAL V15 (stock). This is\nthe way back: keep it safe.\n");
    else if (r->ok)       strcat(body, "OK: custom firmware with the\nFelucca loader marker. Not\nchecked for what its keys do.\n");
    else                  { snprintf(line, sizeof(line), "REFUSED:\n%s\n", r->reason); strcat(body, line); }
    strcat(body, "\nany button: back to the list");
    screen_set("Slipway: file check\nREAD-ONLY: writes nothing", body);
}

/* ------------------------------------------------------------------------------------- main ---- */
static void app_task(void *arg)
{
    (void)arg;
    buttons_init();
    screen_set("Slipway: files on the SD card\nREAD-ONLY: writes nothing", "Looking at the SD card...");
    int sel = 0;
    while (1) {
        if (!card_scan()) {
            screen_set("Slipway: files on the SD card\nREAD-ONLY: writes nothing",
                       "No SD card, or it can't be\nread.\n\nUse a FAT32 card (not exFAT).\nPut it in the slot on the back,\nthen press MENU.");
            while (wait_button() != B_MENU) {}
            continue;
        }
        if (!s_count) {
            screen_set("Slipway: files on the SD card\nREAD-ONLY: writes nothing",
                       "No .fwsc files found.\n\nPut firmware files (.fwsc) on\nthe card, in the top folder or\nin a folder called firmware,\nthen press MENU.");
            while (wait_button() != B_MENU) {}
            continue;
        }
        if (sel >= s_count) sel = 0;
        bool rescan = false;
        while (!rescan) {
            show_list(sel);
            switch (wait_button()) {
            case B_UP:   sel = (sel + s_count - 1) % s_count; break;
            case B_DW:   sel = (sel + 1) % s_count; break;
            case B_MENU: rescan = true; break;
            case B_OK: {
                screen_set("Slipway: file check\nREAD-ONLY: writes nothing", "Checking... (a big file takes\na few seconds)");
                fwcheck_result_t r;
                if (check_file(&s_files[sel], &r)) show_result(&s_files[sel], &r);
                else screen_set("Slipway: file check\nREAD-ONLY: writes nothing", "Could not read that file.\n\nany button: back to the list");
                wait_button();
                break;
            }
            default: break;
            }
        }
    }
}

void app_main(void)
{
    screen_start();
    ESP_LOGI(TAG, "Slipway browser: read-only");
    xTaskCreate(app_task, "app", 8192, NULL, 5, NULL);
}
