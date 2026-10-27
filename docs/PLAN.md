# Slipway Portable: plan and open questions

## Why the protocol is small
Felucca's installer (tools/fm1_install.py) is all USB-MIDI SysEx:
1. Ask `FM-1_xxx`: send `F0 00 32 45 00 00 00 40 7F F7`; reply is a 34-byte (unpacked) identity.
2. Step 1 (check): send `F0 22 24 35 7F F7` (**upgrade**). The FM-1 sends read requests; we answer each from the package.
   It ends with a request for address 0xE0000000. The FM-1 then restarts into the update loader (`ota-...`) and
   **stays in update mode until the write is finished**.
3. Step 2 (write): same again for the whole package, ending with 0xF0000000, then the FM-1 restarts. We check its identity.
So: the ESP32 only has to be a USB-MIDI host and answer read requests from a file on the SD card (the package is up to
4 MB, the board has no PSRAM, so it is read in pieces: `fm1_read_logical`).

## Why stage 0 never sends the upgrade message
Step 1 puts the FM-1 into update mode. The link test must not do that. It only asks who the FM-1 is.

## Open questions (to answer with the real board)
1. Does the FM-1 enumerate and answer the handshake over this board's host port?
2. Does it need the host to supply 5 V while on its own battery? (The board does supply it.)
3. Is the Amazon copy wired like Espressif's board (host power switch, battery boost, display pins)? The code assumes it is.
4. After the loader appears the FM-1 re-enumerates as a new device; the stage 3 code must reconnect (the link test already
   copes with unplug/replug).
5. Is 8 MB flash enough? Yes for the app; the .fwsc files live on the SD card.
6. Buttons: MENU / UP / DOWN / OK for the UI (no touchscreen).

## Stage 3 safety rules (same as Slipway)
* Never install unless the stock V15 is saved on the SD card and the last install was the stock V15 ("stock first, every time").
* Same file checks as Slipway/Felucca (size, AC791N marker, identity matches the FM-1's model, stock hash allowed).
* A dry-run screen that shows exactly what will be written before OK is pressed.
* Never touch anything below 0x4000 (not reachable by this protocol, but the rule stands).
