/* SPDX-License-Identifier: GPL-3.0-only
 * See fm1install.h. Port of the Updater class in Felucca's tools/fm1_install.py (GPL-3.0, Leo Kuroshita). */
#include "fm1install.h"
#include <string.h>

const fm1_delays_t FM1_DELAYS_REAL = {
    .start = 2000, .loader = 3000, .reboot = 3000, .retry = 1000, .poll = 1000, .hs = 1000, .reply = 10,
    .idle_check = 8000, .idle_write = 180000, .wait_loader = 30000, .wait_reboot = 40000,
};

/* "ota-FM-1" -> "FM-1";  "FM-1_015" (a product name) -> "FM-1" */
static void model_of_identity(const fm1_identity_t *id, char *out, size_t cap)
{
    const char *m = id->model;
    if (strncmp(m, "ota-", 4) == 0) m += 4;
    size_t n = strnlen(m, cap - 1);
    memcpy(out, m, n);
    out[n] = 0;
}

static void model_of_product(const char *product, char *out, size_t cap)
{
    size_t n = 0;
    while (product[n] && product[n] != '_' && n < cap - 1) { out[n] = product[n]; n++; }
    out[n] = 0;
}

bool fm1i_handshake(const fm1_io_t *io, const fm1_delays_t *d, int tries, fm1_identity_t *id)
{
    uint8_t pkt[128];
    io->flush_rx(io->ctx);
    for (int t = 0; t < tries; t++) {
        if (!io->send(io->ctx, FM1_HS_QUERY, sizeof(FM1_HS_QUERY))) return false;
        uint32_t end = io->now_ms(io->ctx) + d->hs;
        while (1) {
            uint32_t now = io->now_ms(io->ctx);
            if ((int32_t)(end - now) <= 0) break;
            int n = io->recv(io->ctx, pkt, sizeof(pkt), end - now);
            if (n < 0) return false;
            if (n == 0) break;
            if (fm1_parse_identity(pkt, (size_t)n, id)) return true;
        }
    }
    return false;
}

bool fm1i_wait_identity(const fm1_io_t *io, const fm1_delays_t *d, int want_loader, uint32_t ms, fm1_identity_t *id)
{
    uint32_t start = io->now_ms(io->ctx);
    while ((uint32_t)(io->now_ms(io->ctx) - start) < ms) {
        if (io->device_ready(io->ctx)) {
            fm1_identity_t got;
            if (fm1i_handshake(io, d, 2, &got) && (want_loader < 0 || (int)got.loader == want_loader)) {
                *id = got;
                return true;
            }
        }
        io->sleep_ms(io->ctx, d->retry);
    }
    return false;
}

typedef enum { SERVE_FINISHED, SERVE_LOST, SERVE_STOPPED, SERVE_BADREQ, SERVE_READ } serve_t;

/* Answers read requests until the device asks for `finish`. `served` counts the data answers. */
static serve_t serve(const fm1_io_t *io, const fm1_delays_t *d, size_t image_size, uint32_t finish, uint32_t idle,
                     bool writing, int *served)
{
    uint8_t pkt[128], data[FM1_MAXDATA], resp[640];
    uint32_t last = io->now_ms(io->ctx);
    int lastpct = -1;
    *served = 0;
    while (1) {
        int n = io->recv(io->ctx, pkt, sizeof(pkt), d->poll);
        if (n < 0) return SERVE_LOST;
        if (n == 0) {
            if (!io->device_ready(io->ctx)) return SERVE_LOST;
            if ((uint32_t)(io->now_ms(io->ctx) - last) > idle) return SERVE_STOPPED;
            continue;
        }
        uint8_t fl; uint32_t addr, len;
        if (!fm1_parse_request(pkt, (size_t)n, &fl, &addr, &len)) continue;
        last = io->now_ms(io->ctx);
        if (addr == FM1_FINISH_CHECK || addr == FM1_FINISH_WRITE) {
            static const uint8_t ok[8] = { 's', 'u', 'c', 'c', 'e', 's', 's', 0 };
            size_t k = fm1_build_response(addr, ok, sizeof(ok), 0, resp, sizeof(resp));
            if (!k || !io->send(io->ctx, resp, k)) return SERVE_LOST;
            if (addr == finish) return SERVE_FINISHED;
            continue;
        }
        if (len > FM1_MAXDATA || (size_t)addr + len > image_size) return SERVE_BADREQ;
        io->sleep_ms(io->ctx, d->reply);
        if (!io->read_image(io->ctx, addr, len, data)) return SERVE_READ;
        size_t k = fm1_build_response(addr, data, len, fl, resp, sizeof(resp));
        if (!k || !io->send(io->ctx, resp, k)) return SERVE_LOST;
        (*served)++;
        if (writing) {
            int pct = (int)(((uint64_t)(addr + len) * 100) / image_size);
            if (pct > 99) pct = 99;
            if (pct != lastpct) { lastpct = pct; io->progress(io->ctx, "write", pct); }
        } else if (((*served) & 31) == 1) {
            io->progress(io->ctx, "check", *served);
        }
    }
}

static fm1i_err_t serve_err(serve_t s)
{
    switch (s) {
    case SERVE_FINISHED: return FM1I_OK;
    case SERVE_LOST:     return FM1I_LOST;
    case SERVE_BADREQ:   return FM1I_BADREQ;
    case SERVE_READ:     return FM1I_READ;
    default:             return FM1I_STOPPED;
    }
}

fm1i_err_t fm1i_install(const fm1_io_t *io, const fm1_delays_t *d, const fm1_identity_t *dev,
                        const char *product, size_t image_size, fm1_identity_t *final_id)
{
    char m1[28], m2[28];
    model_of_identity(dev, m1, sizeof(m1));
    model_of_product(product, m2, sizeof(m2));
    if (strcmp(m1, m2) != 0) return FM1I_MODEL;

    fm1_identity_t loader = *dev;
    int served;
    if (!dev->loader) {
        /* step 1: the running firmware checks the package and starts the loader. Nothing is written yet. */
        io->progress(io->ctx, "check", 0);
        if (!io->send(io->ctx, FM1_UPGRADE, sizeof(FM1_UPGRADE))) return FM1I_LOST;
        io->sleep_ms(io->ctx, d->start);
        serve_t s = serve(io, d, image_size, FM1_FINISH_CHECK, d->idle_check, false, &served);
        if (s != SERVE_FINISHED) return serve_err(s);
        io->progress(io->ctx, "loader", 0);
        io->sleep_ms(io->ctx, d->loader);
        if (!fm1i_wait_identity(io, d, 1, d->wait_loader, &loader)) return FM1I_NOLOADER;
    }

    /* step 2: the loader reads and writes the whole package, then restarts */
    io->progress(io->ctx, "write", 0);
    if (!io->send(io->ctx, FM1_UPGRADE, sizeof(FM1_UPGRADE))) return FM1I_LOST;
    io->sleep_ms(io->ctx, d->start);
    serve_t s = serve(io, d, image_size, FM1_FINISH_WRITE, d->idle_write, true, &served);
    if (s != SERVE_FINISHED) return serve_err(s);
    io->progress(io->ctx, "write", 100);

    /* step 3: it must come back and report the package's identity */
    io->progress(io->ctx, "reboot", 0);
    io->sleep_ms(io->ctx, d->reboot);
    fm1_identity_t back;
    if (!fm1i_wait_identity(io, d, 0, d->wait_reboot, &back)) return FM1I_NORETURN;
    if (final_id) *final_id = back;
    if (strcmp(back.text, product) != 0) return FM1I_MISMATCH;
    return FM1I_OK;
}

const char *fm1i_err_text(fm1i_err_t e)
{
    switch (e) {
    case FM1I_OK:       return "Done.";
    case FM1I_NOTFOUND: return "No FM-1 answered.";
    case FM1I_MODEL:    return "That firmware is for a different model.";
    case FM1I_LOST:     return "The FM-1 was disconnected.";
    case FM1I_STOPPED:  return "The FM-1 stopped answering.";
    case FM1I_BADREQ:   return "The FM-1 asked for data outside the file.";
    case FM1I_NOLOADER: return "The update loader did not appear.";
    case FM1I_NORETURN: return "The FM-1 did not come back.";
    case FM1I_MISMATCH: return "It came back as another firmware.";
    case FM1I_READ:     return "The file could not be read from the card.";
    }
    return "Unknown error.";
}
