/* SPDX-License-Identifier: GPL-3.0-only
 * The "way back" record: which file on the SD card is the official stock firmware (V15 or V16). Kept in a tiny text
 * file on the card (slipway.state) so it survives power-off and moves with the card. */
#ifndef SDSTATE_H
#define SDSTATE_H
#include <stdbool.h>

typedef struct {
    bool have;
    char path[160];     /* e.g. /sdcard/firmware/FM-1.fwsc */
    char sha[65];       /* SHA-256, 64 hex characters */
} stockrec_t;

bool sdstate_load(const char *state_path, stockrec_t *s);          /* false: no record (or unreadable) */
bool sdstate_save(const char *state_path, const stockrec_t *s);    /* false: could not write the card */
#endif
