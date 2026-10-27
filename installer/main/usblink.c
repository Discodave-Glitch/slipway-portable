/* SPDX-License-Identifier: GPL-3.0-only
 * USB host side of Slipway Portable. Adapted from the link test, which was verified on a real board and a real FM-1.
 * Differences from the link test: packets are queued for the install task, the send buffer holds a whole read answer
 * (about 800 bytes), a send reports failure if the transfer failed, and a device that appears while the previous one is
 * still being closed (the FM-1 restarts as an update loader and later as the new firmware) is not missed. */
#include "usblink.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "usb/usb_host.h"
#include "bsp/esp-bsp.h"
#include "fm1proto.h"

static const char *TAG = "usblink";

#define IN_BUF   512
#define OUT_BUF  1024            /* one answer to a read request: 512 data bytes -> about 800 bytes of USB-MIDI */
#define PKT_MAX  128             /* identity answers and read requests are far smaller; anything longer is not for us */

typedef enum { ST_IDLE, ST_OPEN, ST_READY, ST_CLOSING } state_t;
typedef struct { uint16_t n; uint8_t d[PKT_MAX]; } pkt_t;

static struct {
    usb_host_client_handle_t client;
    usb_device_handle_t      dev;
    volatile state_t         state;
    uint8_t  intf, alt, ep_in, ep_out;
    uint16_t mps_in, mps_out;
    usb_transfer_t *xin, *xout;
    volatile int    pending_in, pending_out;
    volatile int    out_status;
    SemaphoreHandle_t out_done;
    QueueHandle_t   q;
    fm1_sysex_rx_t  rx;
    char            note[40];
    bool            started;
} M;

static volatile int  s_new_addr = -1;
static volatile bool s_gone = false;

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(M.note, sizeof(M.note), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", M.note);
}

const char *usblink_status(void) { return M.note; }
bool usblink_ready(void) { return M.state == ST_READY; }
void usblink_flush(void) { if (M.q) xQueueReset(M.q); }

static void on_packet(void *ctx, const uint8_t *pkt, size_t n)
{
    (void)ctx;
    if (n > PKT_MAX) return;
    pkt_t p; p.n = (uint16_t)n; memcpy(p.d, pkt, n);
    xQueueSend(M.q, &p, 0);
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
    M.out_status = (int)t->status;
    M.pending_out = 0;
    xSemaphoreGive(M.out_done);
}

bool usblink_send(const uint8_t *sx, size_t n)
{
    if (M.state != ST_READY) return false;
    size_t k = fm1_usbmidi_encode_sysex(sx, n, 0, M.xout->data_buffer, M.xout->data_buffer_size);
    if (!k) return false;
    xSemaphoreTake(M.out_done, 0);                       /* clear a stale signal */
    M.xout->num_bytes = (int)k;
    M.out_status = -1;
    M.pending_out = 1;
    if (usb_host_transfer_submit(M.xout) != ESP_OK) { M.pending_out = 0; return false; }
    if (xSemaphoreTake(M.out_done, pdMS_TO_TICKS(1500)) != pdTRUE) return false;
    return M.out_status == (int)USB_TRANSFER_STATUS_COMPLETED;
}

int usblink_recv(uint8_t *buf, size_t cap, uint32_t ms)
{
    pkt_t p;
    if (xQueueReceive(M.q, &p, 0) != pdTRUE) {
        if (M.state != ST_READY) return -1;
        if (xQueueReceive(M.q, &p, pdMS_TO_TICKS(ms)) != pdTRUE) return M.state == ST_READY ? 0 : -1;
    }
    size_t n = p.n < cap ? p.n : cap;
    memcpy(buf, p.d, n);
    return (int)n;
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
    note("USB device unplugged");
}

static void open_device(uint8_t addr)
{
    if (M.state != ST_IDLE) return;
    fm1_sysex_rx_init(&M.rx);
    xQueueReset(M.q);
    if (usb_host_device_open(M.client, addr, &M.dev) != ESP_OK) { note("could not open USB device"); return; }
    M.state = ST_OPEN;
    const usb_device_desc_t *dd;
    const usb_config_desc_t *cfg;
    if (usb_host_get_device_descriptor(M.dev, &dd) != ESP_OK || usb_host_get_active_config_descriptor(M.dev, &cfg) != ESP_OK) {
        note("could not read USB descriptors");
        usb_host_device_close(M.client, M.dev); M.dev = NULL; M.state = ST_IDLE; return;
    }
    uint16_t vid = dd->idVendor, pid = dd->idProduct;
    if (!find_midi(cfg)) {
        note("USB %04x:%04x is not MIDI", vid, pid);
        usb_host_device_close(M.client, M.dev); M.dev = NULL; M.state = ST_IDLE; return;
    }
    if (usb_host_interface_claim(M.client, M.dev, M.intf, M.alt) != ESP_OK) {
        note("could not claim the MIDI port");
        usb_host_device_close(M.client, M.dev); M.dev = NULL; M.state = ST_IDLE; return;
    }
    size_t in_size = ((IN_BUF + M.mps_in - 1) / M.mps_in) * M.mps_in;
    if (usb_host_transfer_alloc(in_size, 0, &M.xin) != ESP_OK || usb_host_transfer_alloc(OUT_BUF, 0, &M.xout) != ESP_OK) {
        note("out of memory");
        finalize(); return;
    }
    M.xin->device_handle = M.dev; M.xin->bEndpointAddress = M.ep_in; M.xin->callback = in_cb; M.xin->context = NULL;
    M.xin->num_bytes = (int)in_size; M.xin->timeout_ms = 0;
    M.xout->device_handle = M.dev; M.xout->bEndpointAddress = M.ep_out; M.xout->callback = out_cb; M.xout->context = NULL;
    M.xout->timeout_ms = 1000;
    M.state = ST_READY;
    M.pending_in = 1;
    if (usb_host_transfer_submit(M.xin) != ESP_OK) { M.pending_in = 0; note("could not start reading"); return; }
    note("MIDI device %04x:%04x ready", vid, pid);
}

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
    uint8_t list[4]; int num = 0;                           /* a device that was already plugged in before we registered */
    if (usb_host_device_addr_list_fill(sizeof(list), list, &num) == ESP_OK && num > 0) s_new_addr = list[0];
    while (1) {
        usb_host_client_handle_events(M.client, pdMS_TO_TICKS(100));
        if (s_gone) {
            s_gone = false;
            M.state = ST_CLOSING;
            if (M.xin)  usb_host_endpoint_flush(M.dev, M.ep_in);
            if (M.xout) usb_host_endpoint_flush(M.dev, M.ep_out);
        }
        if (M.state == ST_CLOSING && M.pending_in == 0 && M.pending_out == 0) finalize();
        if (s_new_addr >= 0 && M.state == ST_IDLE) {        /* only when the previous device is fully closed */
            int a = s_new_addr; s_new_addr = -1;
            open_device((uint8_t)a);
        }
    }
}

void usblink_start(void)
{
    if (M.started) return;
    M.started = true;
    M.out_done = xSemaphoreCreateBinary();
    M.q = xQueueCreate(16, sizeof(pkt_t));
    bsp_usb_host_power_mode_t mode =
#if CONFIG_INSTALLER_POWER_BATTERY
        BSP_USB_HOST_POWER_MODE_BATTERY;
#else
        BSP_USB_HOST_POWER_MODE_USB_DEV;
#endif
    ESP_ERROR_CHECK(bsp_usb_host_start(mode, CONFIG_INSTALLER_LIMIT_500MA));
    xTaskCreate(client_task, "usb_client", 6144, NULL, 8, NULL);
    note("USB host started");
}
