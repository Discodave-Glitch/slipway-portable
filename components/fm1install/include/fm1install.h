/* SPDX-License-Identifier: GPL-3.0-only
 * Slipway Portable: the FM-1 install sequence, with no hardware in it.
 * A C port of the Updater class in tools/fm1_install.py from the Felucca project (GPL-3.0, Copyright (C) 2026
 * Leo Kuroshita (@kurogedelic), Hügelton Instruments): same steps, same waiting times, same checks.
 *
 *   1. the running FM-1 is asked who it is                       (done by the caller, result passed in)
 *   2. "upgrade" is sent; the FM-1 reads the whole package from us (check step) and says it is happy
 *      -> nothing has been written yet; if this fails, the FM-1 is untouched
 *   3. the FM-1 restarts as an update loader ("ota-...")
 *   4. "upgrade" is sent again; the loader reads the package and writes it (write step)
 *   5. the FM-1 restarts; it must report the identity of the package
 * A device that is already a loader (an install that was not finished) only needs steps 4 and 5.
 * Everything that touches USB, the SD card or the clock goes through fm1_io_t, so this runs unchanged on the host in tests. */
#ifndef FM1INSTALL_H
#define FM1INSTALL_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "fm1proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FM1I_OK = 0,
    FM1I_NOTFOUND,   /* no FM-1 answered */
    FM1I_MODEL,      /* the package is for a different model */
    FM1I_LOST,       /* the connection was lost */
    FM1I_STOPPED,    /* the FM-1 stopped asking for data */
    FM1I_BADREQ,     /* the FM-1 asked for something outside the package */
    FM1I_NOLOADER,   /* the update loader did not appear */
    FM1I_NORETURN,   /* the FM-1 did not come back after the write */
    FM1I_MISMATCH,   /* it came back, but reports another identity */
    FM1I_READ        /* the package could not be read from the card */
} fm1i_err_t;

typedef struct {
    void *ctx;
    /* Sends one complete SysEx packet (F0 ... F7) to the FM-1. */
    bool (*send)(void *ctx, const uint8_t *sysex, size_t n);
    /* Waits up to `ms` for the next complete SysEx packet. Returns its length, 0 on timeout, -1 if the device is gone. */
    int  (*recv)(void *ctx, uint8_t *buf, size_t cap, uint32_t ms);
    /* True while a MIDI-capable USB device is connected and open. */
    bool (*device_ready)(void *ctx);
    /* Throws away any packets received so far. */
    void (*flush_rx)(void *ctx);
    /* Reads `len` bytes of the package's logical image (the file without its 20 marker bytes) at `addr`. */
    bool (*read_image)(void *ctx, uint32_t addr, size_t len, uint8_t *out);
    void (*sleep_ms)(void *ctx, uint32_t ms);
    uint32_t (*now_ms)(void *ctx);
    /* stage: "check" (value = number of requests served), "loader", "write" (value = percent), "reboot" */
    void (*progress)(void *ctx, const char *stage, int value);
} fm1_io_t;

/* All the waiting times of the Python installer (milliseconds); tests shorten them. */
typedef struct {
    uint32_t start, loader, reboot, retry, poll, hs, reply, idle_check, idle_write, wait_loader, wait_reboot;
} fm1_delays_t;
extern const fm1_delays_t FM1_DELAYS_REAL;

/* Asks the connected FM-1 who it is (up to `tries` queries). */
bool fm1i_handshake(const fm1_io_t *io, const fm1_delays_t *d, int tries, fm1_identity_t *id);

/* Waits up to `ms` for a device that answers the identity query and, when `want_loader` is 0/1, is (not) a loader. */
bool fm1i_wait_identity(const fm1_io_t *io, const fm1_delays_t *d, int want_loader, uint32_t ms, fm1_identity_t *id);

/* Runs the whole install. `dev` is what the FM-1 said before. `product` is the package's identity (e.g. "FM-1_015"),
 * `image_size` the size of its logical image. On FM1I_OK, *final_id is what the FM-1 reports afterwards. */
fm1i_err_t fm1i_install(const fm1_io_t *io, const fm1_delays_t *d, const fm1_identity_t *dev,
                        const char *product, size_t image_size, fm1_identity_t *final_id);

/* Plain-English text for the screen. */
const char *fm1i_err_text(fm1i_err_t e);

#ifdef __cplusplus
}
#endif
#endif
