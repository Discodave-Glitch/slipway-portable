/* SPDX-License-Identifier: GPL-3.0-only
 * USB host side of Slipway Portable: finds the FM-1 (a USB-MIDI device), sends and receives SysEx packets.
 * Based on the verified link test (linktest/main/main.c). */
#ifndef USBLINK_H
#define USBLINK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void usblink_start(void);                                            /* starts the USB host (once; later calls do nothing) */
bool usblink_ready(void);                                            /* a MIDI-capable device is connected and open */
bool usblink_send(const uint8_t *sysex, size_t n);                   /* one complete F0..F7 packet */
int  usblink_recv(uint8_t *buf, size_t cap, uint32_t ms);            /* length, 0 = timeout, -1 = device gone */
void usblink_flush(void);
const char *usblink_status(void);                                    /* short text about the last USB event, for the screen */
#endif
