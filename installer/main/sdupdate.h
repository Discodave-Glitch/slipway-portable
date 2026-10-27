/* SPDX-License-Identifier: GPL-3.0-only
 * Updating Slipway Portable itself from a file on the SD card (no cable, no esptool). The file must be this
 * project's own app image (checked: ESP image header, app description, project name "slipway_installer").
 * It is written to the spare app slot and only then selected for the next start, so a failed or interrupted
 * update leaves the running version untouched. */
#ifndef SDUPDATE_H
#define SDUPDATE_H
#include <stdbool.h>
#include <stdint.h>

typedef enum { UPD_OK = 0, UPD_OPEN, UPD_NOTAPP, UPD_WRONGAPP, UPD_SIZE, UPD_WRITE, UPD_END, UPD_SETBOOT } upd_t;

/* Pure check of the first bytes of a file: 0 = fine, else UPD_NOTAPP / UPD_WRONGAPP. (Testable on the host.) */
upd_t sdupdate_check_head(const uint8_t *head, unsigned n);
upd_t sdupdate_apply(const char *path, void (*progress)(int percent));
const char *sdupdate_text(upd_t e);
#endif
