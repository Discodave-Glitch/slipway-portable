# Slipway Portable (work in progress)

A pocket FM-1 firmware installer on an **ESP32-S3-USB-OTG** board (USB-A host socket, small screen, buttons, SD slot).
The companion to [Slipway](https://github.com/Discodave-Glitch/slipway) for Windows and Mac.

**Status: stages 0 and 1 (both read-only). Nothing here can install or change anything on an FM-1.**
Stage 0 has run on a real board (screen, USB host and 5 V verified with a USB stick; not yet with an FM-1). The protocol library is tested against Felucca's own Python installer;
the board code has not been compiled by its author (see "What is and isn't tested").

## Stages
| Stage | What | State |
|---|---|---|
| 0 | **Link test**: plug the FM-1 into the board, the screen shows what the FM-1 says it is (e.g. `FM-1_015`). Read-only. | runs on the board; USB stick detected; FM-1 not yet tried |
| 1 | **SD browser**: list the `.fwsc` files on the SD card and check each one (size, chip marker, product name, Felucca loader marker, official V15 fingerprint) with the same rules as Slipway. Read-only, no FM-1 needed. | written; rules tested on a PC against Slipway's own code, screen/buttons/SD not yet run on the board |
| 2 | Stock-first rules (same as Slipway), saved on the SD card | not started |
| 3 | Install, with the same safety checks as Felucca's installer. Only after stage 0 and 1 work on a real board. | not started |
| 4 | WiFi: download from the Slipway catalogue / GitHub | not started |

## Try the link test (no programming tools needed)
1. Put these files in a new GitHub repository (for example `slipway-portable`).
2. On GitHub open **Actions → Build link test → Run workflow**. It first checks the protocol library against Felucca's
   Python installer, then builds the firmware. When it finishes (a few minutes), download the **slipway-linktest** artifact
   and unzip it: you get `linktest-merged.bin`.
3. Connect the board's **Type-C debug port** to your PC. In Chrome or Edge open Espressif's web flasher
   (https://espressif.github.io/esptool-js/), click Connect, pick the board's serial port, set the flash address to
   `0x0`, choose `linktest-merged.bin` and click Program. If it will not connect, hold the board's **OK/BOOT** button
   while pressing **RESET**, then try again.
4. Power the board from the **male USB-A plug** (a charger or power bank), switch the FM-1 on, and connect the FM-1 to the
   board's **USB-A socket** with a USB-A to USB-C **data** cable.
5. The screen should show the USB device, the MIDI interface, and then `FM-1 says: FM-1_015`.
   Send me a photo of the screen, or the lines you see, whatever happens.

If the build fails, copy the red error lines from the Actions log to me and I will fix it.

## What is and isn't tested
* **Tested:** `components/fm1proto` (the pure protocol code: 7-bit packing, read requests and answers, identity parsing,
  package header and marker bytes, USB-MIDI packet encoding) is compared byte for byte with Felucca's Python
  installer by `host_tests/run_tests.py` (1,000+ checks, built with AddressSanitizer and UBSan).
* **Not tested:** `linktest/main/main.c` (USB host, screen). It was only syntax-checked against hand-written stand-ins for the
  ESP-IDF headers. The ESP-IDF and board-support calls were written from the documentation and may need small fixes.
* **Not tested at all:** the real board, a real FM-1, the case.

## Licence and credits
GPL-3.0-only (see `LICENSE`), because the protocol code is a port of Felucca's installer.
* **Felucca** and its installer, by Leo Kuroshita (@kurogedelic) / Hügelton Instruments, GPL-3.0: https://github.com/hugelton/Felucca
* **ESP32-S3-USB-OTG board support package**, Espressif (Apache-2.0): https://github.com/espressif/esp-bsp
* Board: Espressif ESP32-S3-USB-OTG (the Amazon version is a generic copy).

At your own risk. This is hobby software for a hobby device.

## Try the SD browser (stage 1)
1. Run **Actions -> Build link test and SD browser**. Download the **slipway-browser** artifact: `browser-merged.bin`.
2. Flash it the same way as the link test (address `0x0`).
3. Put `.fwsc` files on a **FAT32** micro SD card (top folder, or a folder called `firmware`) and put the card in the slot on the back of the board.
4. Power the board. **UP / DW** move, **OK** checks the highlighted file, **MENU** looks at the card again.
5. You should see `OFFICIAL V15 (stock)` for the M-VAVE file, `OK: custom firmware ...` for a custom firmware, and `REFUSED: ...` with a reason otherwise.
This program never uses USB and never touches an FM-1. The checking rules are in `components/fwcheck` and are tested on a PC
(`python3 host_tests/run_fw_tests.py [path-to-Slipway] [official-FM-1.fwsc]`) against Python's hashlib and Slipway's own rules.
