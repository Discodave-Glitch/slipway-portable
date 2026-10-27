/* SPDX-License-Identifier: GPL-3.0-only
 * Slipway Portable - installer.
 *
 * Lists the .fwsc files on the SD card, checks them with the same rules as Slipway, and installs one onto an FM-1 plugged
 * into the host socket, following the same steps as Felucca's installer (components/fm1install).
 *
 * Stock first, every time (same rule as Slipway for Windows/Mac/Android):
 *   - the official stock file (V15 or V16) can always be installed: it is the way back. Opening it in the list saves it
 *     as your way back (a small file, slipway.state, on the card);
 *   - a custom firmware is installed only while that stock file is on the card, intact, AND the FM-1 is currently on
 *     the official stock firmware (or in update mode from an unfinished install). Otherwise: put stock back first.
 *
 * Buttons: UP / DW move, OK checks the file, MENU (on the check screen) installs it, MENU in the list looks at the card again.
 * A file called slipway*.bin on the card is a new version of this program: OK on it offers to update from the card.
 * Board: Espressif ESP32-S3-USB-OTG (or a compatible copy), via Espressif's board support package.
 * Protocol credits: Felucca (GPL-3.0, Leo Kuroshita / Hügelton Instruments).
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
#include "esp_system.h"
#include "driver/gpio.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "fwcheck.h"
#include "fm1install.h"
#include "usblink.h"
#include "sdstate.h"
#include "sdupdate.h"

static const char *TAG = "installer";
#define STATE_PATH BSP_SD_MOUNT_POINT "/slipway.state"
#define TITLE_MAIN "Slipway Portable"

/* ------------------------------------------------------------------------------------- screen --- */
#define COLS 30
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
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_body, BSP_LCD_H_RES - 8);
    lv_obj_set_style_text_color(s_body, lv_color_hex(0xf2f2f2), 0);
    lv_obj_align(s_body, LV_ALIGN_TOP_LEFT, 4, 42);
    bsp_display_unlock();
}

static void screenf(const char *title, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void screenf(const char *title, const char *fmt, ...)
{
    char body[700];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    screen_set(title, body);
}

/* ------------------------------------------------------------------------------------ buttons --- */
typedef enum { B_NONE, B_UP, B_DW, B_OK, B_MENU } button_t;
static const struct { gpio_num_t io; button_t b; } s_map[] = {
    { BSP_BUTTON_UP_IO, B_UP }, { BSP_BUTTON_DW_IO, B_DW }, { BSP_BUTTON_OK_IO, B_OK }, { BSP_BUTTON_MENU_IO, B_MENU },
};
#define NBTN (sizeof(s_map) / sizeof(s_map[0]))

static void buttons_init(void)
{
    for (size_t i = 0; i < NBTN; i++) {
        gpio_config_t c = {
            .pin_bit_mask = 1ULL << s_map[i].io, .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&c);
    }
}

static button_t read_pins(void)
{
    for (size_t i = 0; i < NBTN; i++) if (gpio_get_level(s_map[i].io) == 0) return s_map[i].b;
    return B_NONE;
}

/* Waits for one press (the buttons pull the pin to ground) and returns it after the button was released.
 * timeout_ms = 0 waits for ever; otherwise returns B_NONE when the time is up. */
static button_t poll_button(uint32_t timeout_ms)
{
    uint32_t waited = 0; int held = 0; button_t which = B_NONE;
    while (1) {
        button_t now = read_pins();
        if (now != B_NONE && now == which) {
            if (++held == 2) {                              /* pressed for ~40 ms: a real press */
                while (read_pins() != B_NONE) vTaskDelay(pdMS_TO_TICKS(20));
                return which;
            }
        } else { which = now; held = (now == B_NONE) ? 0 : 1; }
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
        if (timeout_ms && waited >= timeout_ms) return B_NONE;
    }
}
static button_t wait_button(void) { return poll_button(0); }

/* ------------------------------------------------------------------------------------- files --- */
#define MAX_FILES 40
typedef struct { char path[160]; const char *name; uint32_t size; bool upd; } entry_t;
static entry_t s_files[MAX_FILES];
static int s_count;

static bool has_ext(const char *n, const char *ext)
{
    size_t l = strlen(n), e = strlen(ext);
    return l > e && strcasecmp(n + l - e, ext) == 0;
}

static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_count < MAX_FILES) {
        if (e->d_type == DT_DIR || e->d_name[0] == '.') continue;
        bool fw = has_ext(e->d_name, ".fwsc");
        bool upd = has_ext(e->d_name, ".bin") && strncasecmp(e->d_name, "slipway", 7) == 0;
        if (!fw && !upd) continue;
        entry_t *f = &s_files[s_count];
        if (snprintf(f->path, sizeof(f->path), "%s/%s", dir, e->d_name) >= (int)sizeof(f->path)) continue;
        f->name = f->path + strlen(dir) + 1;
        f->upd = upd;
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

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = 0;
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
    snprintf(line, sizeof(line), "\n%d of %d   UP/DW move, OK opens", sel + 1, s_count);
    strcat(body, line);
    screen_set(TITLE_MAIN "\nfiles on the SD card", body);
}

/* ------------------------------------------------------------------------------------- check --- */
static fwscan_t s_scan;

static bool check_path(const char *path, fwcheck_result_t *r)
{
    FILE *fp = fopen(path, "rb");
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

/* The official file you opened becomes your way back. Returns 1 saved, 0 already saved, -1 could not write the card. */
static int register_stock(const char *path, const fwcheck_result_t *r)
{
    stockrec_t cur, n;
    char hex[65];
    fw_hex(r->sha256, 32, hex);
    sdstate_load(STATE_PATH, &cur);
    if (cur.have && !strcmp(cur.sha, hex) && !strcmp(cur.path, path)) return 0;
    memset(&n, 0, sizeof(n));
    n.have = true;
    copy_str(n.path, sizeof(n.path), path);
    copy_str(n.sha, sizeof(n.sha), hex);
    return sdstate_save(STATE_PATH, &n) ? 1 : -1;
}

typedef enum { STOCK_OK, STOCK_NONE, STOCK_BAD } stockstate_t;

/* Is the saved way back still on the card and exactly the official file? */
static stockstate_t stock_state(void)
{
    stockrec_t st;
    if (!sdstate_load(STATE_PATH, &st)) return STOCK_NONE;
    fwcheck_result_t r;
    if (!check_path(st.path, &r) || !r.official) return STOCK_BAD;
    char hex[65];
    fw_hex(r.sha256, 32, hex);
    return strcmp(hex, st.sha) == 0 ? STOCK_OK : STOCK_BAD;
}

static bool dev_is_stock(const fm1_identity_t *id)
{
    return !id->loader && strcmp(id->model, "FM-1") == 0 && (id->version == 15 || id->version == 16);
}

static const char *official_label(const fwcheck_result_t *r)
{
    return strcmp(r->product, "FM-1_016") == 0 ? "V16" : "V15";
}

static void show_result(const entry_t *f, const fwcheck_result_t *r, int saved)
{
    char body[700], nm[COLS + 1], hex[65], line[96];
    shorten(f->name, nm, COLS);
    snprintf(body, sizeof(body), "%s\n%u bytes (%u KB)\n", nm, (unsigned)r->size, (unsigned)(r->size / 1024));
    if (r->product[0]) { snprintf(line, sizeof(line), "Product: %s\n", r->product); strcat(body, line); }
    fw_hex(r->sha256, 6, hex);
    snprintf(line, sizeof(line), "Fingerprint: %s...\n\n", hex); strcat(body, line);
    if (r->official) {
        snprintf(line, sizeof(line), "OFFICIAL %s (stock).\n", official_label(r)); strcat(body, line);
        strcat(body, saved < 0 ? "Could not save it on the card.\n" : "Saved as your way back.\n");
        if (strcmp(r->product, "FM-1_016") == 0) strcat(body, "Put-back of V16 is untested.\n");
    } else if (r->ok) {
        strcat(body, "OK: custom firmware with the\nFelucca loader marker. Keys\nnot checked.\n");
    } else {
        snprintf(line, sizeof(line), "REFUSED:\n%s\n", r->reason); strcat(body, line);
    }
    strcat(body, r->ok ? "\nMENU: install on the FM-1\nany other button: back" : "\nany button: back to the list");
    screen_set(TITLE_MAIN "\nfile check", body);
}

/* ------------------------------------------------------------------------------------ install --- */
static FILE *s_img;
static size_t s_img_size;
static char s_stage[8];

static bool img_raw(void *ctx, size_t off, uint8_t *buf, size_t n)
{
    (void)ctx;
    if (fseek(s_img, (long)off, SEEK_SET)) return false;
    return fread(buf, 1, n, s_img) == n;
}
static bool io_read_image(void *c, uint32_t addr, size_t len, uint8_t *out) { (void)c; return fm1_read_logical(s_img_size, addr, len, out, img_raw, NULL); }
static bool io_send(void *c, const uint8_t *p, size_t n) { (void)c; return usblink_send(p, n); }
static int  io_recv(void *c, uint8_t *b, size_t cap, uint32_t ms) { (void)c; return usblink_recv(b, cap, ms); }
static bool io_ready(void *c) { (void)c; return usblink_ready(); }
static void io_flush(void *c) { (void)c; usblink_flush(); }
static void io_sleep(void *c, uint32_t ms) { (void)c; vTaskDelay(pdMS_TO_TICKS(ms)); }
static uint32_t io_now(void *c) { (void)c; return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

static void io_progress(void *c, const char *stage, int v)
{
    (void)c;
    copy_str(s_stage, sizeof(s_stage), stage);
    if (!strcmp(stage, "check"))
        screenf(TITLE_MAIN "\ninstalling", "Step 1 of 3\nThe FM-1 is checking the file.\nNOTHING IS WRITTEN YET.\n\nrequests: %d\n\nDo not unplug.", v);
    else if (!strcmp(stage, "loader"))
        screenf(TITLE_MAIN "\ninstalling", "Step 2 of 3\nWaiting for the FM-1's update\nloader to appear...\n\nDo not unplug.");
    else if (!strcmp(stage, "write"))
        screenf(TITLE_MAIN "\ninstalling", "Step 3 of 3: WRITING\n\n%d%%\n\nDO NOT UNPLUG OR\nSWITCH OFF.", v);
    else
        screenf(TITLE_MAIN "\ninstalling", "Writing finished.\nWaiting for the FM-1 to\nrestart (up to 40 seconds).");
}

static void install_flow(const entry_t *f, const fwcheck_result_t *r)
{
    const char *T = TITLE_MAIN "\ninstall";

    /* 1. stock first (the part that needs only the card) */
    if (!r->official) {
        stockstate_t ss = stock_state();
        if (ss == STOCK_NONE) {
            screen_set(T, "Stock first.\n\nBefore a custom firmware goes\non, put the OFFICIAL stock\nfile (V15 or V16) on the card,\nopen it in the list and press\nOK once so it is saved as your\nway back.\n\nany button: back");
            wait_button(); return;
        }
        if (ss == STOCK_BAD) {
            screen_set(T, "Stock first.\n\nYour saved stock file is\nmissing or has changed.\n\nOpen the official file in the\nlist and press OK again.\n\nany button: back");
            wait_button(); return;
        }
    }

    /* 2. wait for the FM-1 */
    usblink_start();
    uint32_t waited = 0;
    while (!usblink_ready()) {
        screenf(T, "Plug the FM-1 into the HOST\nsocket. FM-1 on, data cable.\n\nWaiting for it...\n%s\n\nDW: cancel", usblink_status());
        if (poll_button(500) == B_DW) return;
        waited += 500;
        if (waited > 180000) {
            screen_set(T, "No FM-1 found.\n\nany button: back");
            wait_button(); return;
        }
    }

    /* 3. who is it? */
    fm1_identity_t id;
    screen_set(T, "Asking the FM-1 who it is...");
    fm1_io_t io = { .ctx = NULL, .send = io_send, .recv = io_recv, .device_ready = io_ready, .flush_rx = io_flush,
                    .read_image = io_read_image, .sleep_ms = io_sleep, .now_ms = io_now, .progress = io_progress };
    while (!fm1i_handshake(&io, &FM1_DELAYS_REAL, 3, &id)) {
        screen_set(T, "The FM-1 did not answer.\n\nTry another cable, or switch\nthe FM-1 off and on.\n\nOK: try again   DW: cancel");
        if (wait_button() == B_DW) return;
        screen_set(T, "Asking the FM-1 who it is...");
    }

    /* 4. stock first: a custom firmware only goes onto an FM-1 that is on the official stock (or in update mode) */
    if (!r->official && !id.loader && !dev_is_stock(&id)) {
        screenf(T, "Stock first.\n\nThe FM-1 runs %s,\nnot stock. Put the official\nstock back first: open the\nOFFICIAL file, press OK, then\nMENU.\n\nany button: back", id.text);
        wait_button(); return;
    }

    /* 5. confirm */
    char nm[COLS + 1], note[96] = "";
    shorten(f->name, nm, COLS);
    if (id.loader) snprintf(note, sizeof(note), "FM-1 is in update mode: this\nfinishes the install.\n");
    else if (r->official) snprintf(note, sizeof(note), "Back up your songs first.\n%s", strcmp(r->product, "FM-1_016") == 0 ? "V16 put-back is untested.\n" : "");
    else snprintf(note, sizeof(note), "Its keys are not checked.\n");
    screenf(T, "Install on the FM-1?\n%s\n=> %s\nFM-1 now: %s\n%s\nOK: START   DW: cancel", nm, r->product, id.text, note);
    if (wait_button() != B_OK) return;

    /* 6. install */
    s_img = fopen(f->path, "rb");
    if (!s_img) { screen_set(T, "Could not open the file.\n\nany button: back"); wait_button(); return; }
    s_img_size = r->size;
    s_stage[0] = 0;
    fm1_identity_t fin;
    memset(&fin, 0, sizeof(fin));
    fm1i_err_t e = fm1i_install(&io, &FM1_DELAYS_REAL, &id, r->product, fm1_logical_size(r->size), &fin);
    fclose(s_img); s_img = NULL;

    /* 7. result */
    if (e == FM1I_OK) {
        screenf(TITLE_MAIN "\ninstall", "DONE.\n\nThe FM-1 now reports:\n%s\n\nIf its screen stays dark,\nswitch it off and on.\n\nany button: back", fin.text);
    } else if (!strcmp(s_stage, "check") || !s_stage[0]) {
        screenf(TITLE_MAIN "\ninstall", "FAILED: %s\n\nNothing was written. Switch the\nFM-1 off and on, then try\nagain.\n\nany button: back", fm1i_err_text(e));
    } else {
        screenf(TITLE_MAIN "\ninstall", "FAILED: %s\n\nThe FM-1 may be in update mode.\nThat is safe: unplug and\nreplug the USB cable, then run\nthe same install again to\nfinish.\n\nany button: back", fm1i_err_text(e));
    }
    wait_button();
}

/* ----------------------------------------------------------------------------------- self update --- */
static void upd_progress(int pct)
{
    screenf(TITLE_MAIN "\nupdating", "Writing the new version.\n\n%d%%\n\nDo not switch off.", pct);
}

static void update_flow(const entry_t *f)
{
    const char *T = TITLE_MAIN "\nupdate";
    char nm[COLS + 1];
    shorten(f->name, nm, COLS);
    screenf(T, "Update Slipway Portable\nfrom this file?\n\n%s\n\nOK: update   DW: cancel", nm);
    if (wait_button() != B_OK) return;
    upd_t e = sdupdate_apply(f->path, upd_progress);
    if (e == UPD_OK) {
        screen_set(T, "Updated.\n\nRestarting...");
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    screenf(T, "FAILED:\n%s\n\nThe old version is untouched.\n\nany button: back", sdupdate_text(e));
    wait_button();
}

/* ------------------------------------------------------------------------------------- main ---- */
static void app_task(void *arg)
{
    (void)arg;
    buttons_init();
    screen_set(TITLE_MAIN "\nfiles on the SD card", "Looking at the SD card...");
    int sel = 0;
    while (1) {
        if (!card_scan()) {
            screen_set(TITLE_MAIN "\nfiles on the SD card",
                       "No SD card, or it can't be\nread.\n\nUse a FAT32 card (not exFAT).\nPut it in the slot on the back,\nthen press MENU.");
            while (wait_button() != B_MENU) {}
            continue;
        }
        if (!s_count) {
            screen_set(TITLE_MAIN "\nfiles on the SD card",
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
                const entry_t *f = &s_files[sel];
                if (f->upd) { update_flow(f); break; }
                screen_set(TITLE_MAIN "\nfile check", "Checking... (a big file takes\na few seconds)");
                fwcheck_result_t r;
                if (!check_path(f->path, &r)) {
                    screen_set(TITLE_MAIN "\nfile check", "Could not read that file.\n\nany button: back to the list");
                    wait_button();
                    break;
                }
                int saved = r.official ? register_stock(f->path, &r) : 0;
                show_result(f, &r, saved);
                if (wait_button() == B_MENU && r.ok) install_flow(f, &r);
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
    ESP_LOGI(TAG, "Slipway Portable installer");
    xTaskCreate(app_task, "app", 12288, NULL, 5, NULL);
}
