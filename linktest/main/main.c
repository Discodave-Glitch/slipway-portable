/* SPDX-License-Identifier: GPL-3.0-only
 * Slipway Portable - FM-1 link test.
 *
 * READ-ONLY. This program only asks the FM-1 "who are you?" (the same message as `fm1_install.py --info`) and shows
 * the answer on the screen. It never sends the update message, never writes, never erases. It cannot change the FM-1.
 *
 * Board: Espressif ESP32-S3-USB-OTG (or a compatible copy), via Espressif's board support package.
 * Protocol credits: Felucca (GPL-3.0, Leo Kuroshita / Hügelton Instruments).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "usb/usb_host.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "fm1proto.h"

static const char *TAG = "linktest";

/* ------------------------------------------------------------------------------------- screen --- */
#define UI_LINES 9
#define UI_COLS  40
static lv_obj_t *s_title, *s_body;
static char s_lines[UI_LINES][UI_COLS];
static int  s_count;

static void ui_render(void)
{
    char text[UI_LINES * (UI_COLS + 1) + 1] = {0};
    for (int i = 0; i < s_count; i++) {
        strcat(text, s_lines[i]);
        strcat(text, "\n");
    }
    if (bsp_display_lock(200)) {
        lv_label_set_text(s_body, text);
        bsp_display_unlock();
    }
}

/* Adds a line to the screen (and to the serial log). Safe to call from any task. */
static void ui_log(const char *fmt, ...)
{
    char line[UI_COLS];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", line);
    if (s_count == UI_LINES) {
        memmove(s_lines[0], s_lines[1], sizeof(s_lines[0]) * (UI_LINES - 1));
        s_count--;
    }
    strncpy(s_lines[s_count], line, UI_COLS - 1);
    s_lines[s_count][UI_COLS - 1] = 0;
    s_count++;
    ui_render();
}

static void ui_start(void)
{
    bsp_display_start();
    bsp_display_backlight_on();
    bsp_display_lock(0);
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x10243a), 0);
    s_title = lv_label_create(scr);
    lv_label_set_text(s_title, "Slipway link test\nREAD-ONLY: writes nothing");
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xd9b36c), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 4, 2);
    s_body = lv_label_create(scr);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_body, BSP_LCD_H_RES - 8);
    lv_obj_set_style_text_color(s_body, lv_color_hex(0xf2f2f2), 0);
    lv_obj_align(s_body, LV_ALIGN_TOP_LEFT, 4, 42);
    bsp_display_unlock();
}

/* --------------------------------------------------------------------------------- USB-MIDI ---- */
#define IN_BUF 512

typedef enum { ST_IDLE, ST_OPEN, ST_READY, ST_CLOSING } state_t;

static struct {
    usb_host_client_handle_t client;
    usb_device_handle_t      dev;
    volatile state_t         state;
    uint8_t  intf, alt, ep_in, ep_out;
    uint16_t mps_in, mps_out;
    usb_transfer_t *xin, *xout;
    volatile int    pending_in, pending_out;
    SemaphoreHandle_t out_done;
    fm1_sysex_rx_t    rx;
    volatile bool     have_id;
    fm1_identity_t    id;
    TaskHandle_t      probe_task;
} M;

static void on_packet(void *ctx, const uint8_t *pkt, size_t n)
{
    (void)ctx;
    fm1_identity_t id;
    if (fm1_parse_identity(pkt, n, &id)) {
        M.id = id;
        M.have_id = true;
        if (M.probe_task) xTaskNotifyGive(M.probe_task);
    } else {
        ESP_LOGI(TAG, "other SysEx, %u bytes", (unsigned)n);
    }
}

static void in_cb(usb_transfer_t *t)
{
    if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes >= 4) {
        fm1_sysex_rx_feed(&M.rx, t->data_buffer, t->actual_num_bytes - (t->actual_num_bytes % 4), on_packet, NULL);
    }
    bool again = (M.state == ST_READY) &&
                 (t->status == USB_TRANSFER_STATUS_COMPLETED || t->status == USB_TRANSFER_STATUS_TIMED_OUT);
    if (again && usb_host_transfer_submit(t) == ESP_OK) return;
    M.pending_in = 0;
}

static void out_cb(usb_transfer_t *t)
{
    (void)t;
    M.pending_out = 0;
    xSemaphoreGive(M.out_done);
}

static bool send_sysex(const uint8_t *sx, size_t n)
{
    if (M.state != ST_READY) return false;
    size_t k = fm1_usbmidi_encode_sysex(sx, n, 0, M.xout->data_buffer, M.xout->data_buffer_size);
    if (!k) return false;
    M.xout->num_bytes = (int)k;
    M.pending_out = 1;
    if (usb_host_transfer_submit(M.xout) != ESP_OK) { M.pending_out = 0; return false; }
    return xSemaphoreTake(M.out_done, pdMS_TO_TICKS(1000)) == pdTRUE;
}

/* Looks through the active configuration for a USB-MIDI streaming interface with a bulk IN and bulk OUT endpoint. */
static bool find_midi(const usb_config_desc_t *cfg)
{
    const uint8_t *p = (const uint8_t *)cfg;
    const uint8_t *end = p + cfg->wTotalLength;
    int cur_if = -1, cur_alt = 0, cur_cls = 0, cur_sub = 0;
    uint8_t in = 0, out = 0; uint16_t min = 0, mout = 0;
    bool found = false;
    while (p + 2 <= end && p[0] >= 2) {
        if (p[1] == 4 && p[0] >= 9) {                       /* interface descriptor */
            if (cur_cls == 1 && cur_sub == 3 && in && out && !found) {
                M.intf = (uint8_t)cur_if; M.alt = (uint8_t)cur_alt; M.ep_in = in; M.ep_out = out; M.mps_in = min; M.mps_out = mout;
                found = true;
            }
            cur_if = p[2]; cur_alt = p[3]; cur_cls = p[5]; cur_sub = p[6];
            in = out = 0;
            ui_log("if %d alt %d class %02x/%02x", cur_if, cur_alt, cur_cls, cur_sub);
        } else if (p[1] == 5 && p[0] >= 7 && (p[3] & 3) == 2) {   /* bulk endpoint */
            uint8_t addr = p[2]; uint16_t mps = (uint16_t)(p[4] | (p[5] << 8));
            if (addr & 0x80) { if (!in) { in = addr; min = mps; } }
            else             { if (!out) { out = addr; mout = mps; } }
        }
        p += p[0];
    }
    if (cur_cls == 1 && cur_sub == 3 && in && out && !found) {
        M.intf = (uint8_t)cur_if; M.alt = (uint8_t)cur_alt; M.ep_in = in; M.ep_out = out; M.mps_in = min; M.mps_out = mout;
        found = true;
    }
    return found;
}

static void finalize(void)
{
    if (M.xin)  { usb_host_transfer_free(M.xin);  M.xin = NULL; }
    if (M.xout) { usb_host_transfer_free(M.xout); M.xout = NULL; }
    if (M.dev) {
        usb_host_interface_release(M.client, M.dev, M.intf);
        usb_host_device_close(M.client, M.dev);
        M.dev = NULL;
    }
    M.state = ST_IDLE;
    ui_log("FM-1 unplugged. Plug it in.");
}

static void open_device(uint8_t addr)
{
    if (M.state != ST_IDLE) return;
    M.have_id = false;
    fm1_sysex_rx_init(&M.rx);
    if (usb_host_device_open(M.client, addr, &M.dev) != ESP_OK) { ui_log("could not open device"); return; }
    M.state = ST_OPEN;
    const usb_device_desc_t *dd;
    const usb_config_desc_t *cfg;
    if (usb_host_get_device_descriptor(M.dev, &dd) != ESP_OK || usb_host_get_active_config_descriptor(M.dev, &cfg) != ESP_OK) {
        ui_log("could not read descriptors");
        usb_host_device_close(M.client, M.dev); M.dev = NULL; M.state = ST_IDLE; return;
    }
    ui_log("USB device %04x:%04x", dd->idVendor, dd->idProduct);
    if (!find_midi(cfg)) {
        ui_log("No USB-MIDI interface found.");
        usb_host_device_close(M.client, M.dev); M.dev = NULL; M.state = ST_IDLE; return;
    }
    ui_log("MIDI: if %d ep in %02x out %02x", M.intf, M.ep_in, M.ep_out);
    if (usb_host_interface_claim(M.client, M.dev, M.intf, M.alt) != ESP_OK) {
        ui_log("could not claim the MIDI interface");
        usb_host_device_close(M.client, M.dev); M.dev = NULL; M.state = ST_IDLE; return;
    }
    size_t in_size = ((IN_BUF + M.mps_in - 1) / M.mps_in) * M.mps_in;
    if (usb_host_transfer_alloc(in_size, 0, &M.xin) != ESP_OK || usb_host_transfer_alloc(64, 0, &M.xout) != ESP_OK) {
        ui_log("out of memory");
        finalize(); return;
    }
    M.xin->device_handle = M.dev; M.xin->bEndpointAddress = M.ep_in; M.xin->callback = in_cb; M.xin->context = NULL;
    M.xin->num_bytes = (int)in_size; M.xin->timeout_ms = 0;
    M.xout->device_handle = M.dev; M.xout->bEndpointAddress = M.ep_out; M.xout->callback = out_cb; M.xout->context = NULL;
    M.xout->timeout_ms = 1000;
    M.state = ST_READY;
    M.pending_in = 1;
    if (usb_host_transfer_submit(M.xin) != ESP_OK) { M.pending_in = 0; ui_log("could not start reading"); }
    if (M.probe_task) xTaskNotifyGive(M.probe_task);   /* wake the probe: it will see state READY and no id yet */
}

/* The callback only notes what happened; the work is done in the loop below (the pattern ESP-IDF's examples use). */
static volatile int  s_new_addr = -1;
static volatile bool s_gone = false;

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        s_new_addr = msg->new_dev.address;
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE && M.dev && msg->dev_gone.dev_hdl == M.dev) {
        s_gone = true;
    }
}

static void client_task(void *arg)
{
    (void)arg;
    const usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = { .client_event_callback = client_event_cb, .callback_arg = NULL },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&cfg, &M.client));
    /* a device that was already plugged in before we registered */
    uint8_t list[4]; int num = 0;
    if (usb_host_device_addr_list_fill(sizeof(list), list, &num) == ESP_OK && num > 0) s_new_addr = list[0];
    while (1) {
        usb_host_client_handle_events(M.client, pdMS_TO_TICKS(100));
        if (s_new_addr >= 0) {
            int a = s_new_addr; s_new_addr = -1;
            open_device((uint8_t)a);
        }
        if (s_gone) {
            s_gone = false;
            M.state = ST_CLOSING;
            if (M.xin)  usb_host_endpoint_flush(M.dev, M.ep_in);
            if (M.xout) usb_host_endpoint_flush(M.dev, M.ep_out);
        }
        if (M.state == ST_CLOSING && M.pending_in == 0 && M.pending_out == 0) finalize();
    }
}

/* ------------------------------------------------------------------------------------ probe ---- */
static void probe_task(void *arg)
{
    (void)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        if (M.state != ST_READY || M.have_id) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        vTaskDelay(pdMS_TO_TICKS(300));               /* let the first read request get queued */
        bool got = false;
        for (int attempt = 1; attempt <= 3 && M.state == ST_READY && !got; attempt++) {
            ui_log("Asking the FM-1 who it is (%d/3)", attempt);
            M.have_id = false;
            if (!send_sysex(FM1_HS_QUERY, sizeof(FM1_HS_QUERY))) { ui_log("send failed"); continue; }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1200));
            got = M.have_id;
        }
        if (M.state != ST_READY) continue;
        if (got) {
            ui_log("-----------------------");
            ui_log("FM-1 says: %s", M.id.text);
            ui_log("Model %s, version %d", M.id.model, M.id.version);
            ui_log(M.id.loader ? "Mode: UPDATE LOADER (unfinished)" : "Mode: running normally");
            ui_log("LINK WORKS. Nothing was written.");
        } else {
            ui_log("No answer. Try another cable,");
            ui_log("or switch the FM-1 off and on.");
            vTaskDelay(pdMS_TO_TICKS(4000));
            M.have_id = false;                           /* allow another round */
            xTaskNotifyGive(xTaskGetCurrentTaskHandle());
        }
    }
}

void app_main(void)
{
    ui_start();
    ui_log("Power: %s",
#if CONFIG_LINKTEST_POWER_BATTERY
           "battery"
#else
           "USB-A plug"
#endif
    );
    M.out_done = xSemaphoreCreateBinary();
    bsp_usb_host_power_mode_t mode =
#if CONFIG_LINKTEST_POWER_BATTERY
        BSP_USB_HOST_POWER_MODE_BATTERY;
#else
        BSP_USB_HOST_POWER_MODE_USB_DEV;
#endif
    ESP_ERROR_CHECK(bsp_usb_host_start(mode, CONFIG_LINKTEST_LIMIT_500MA));
    xTaskCreate(probe_task, "probe", 4096, NULL, 5, &M.probe_task);
    xTaskCreate(client_task, "usb_client", 6144, NULL, 8, NULL);
    ui_log("Plug the FM-1 into the host");
    ui_log("socket (USB-A to USB-C cable).");
}
