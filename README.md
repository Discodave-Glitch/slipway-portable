# Slipway Portable (work in progress)

A pocket FM-1 firmware installer on an **ESP32-S3-USB-OTG** board (USB-A host socket, small screen, buttons, SD slot).
The companion to [Slipway](https://github.com/Discodave-Glitch/slipway) for Windows, Mac and Android.

**Status: stages 0 and 1 have run on a real board with a real FM-1 (read-only). Stages 2 and 3 (stock-first rules and the install itself) are written and tested on a PC against a simulated FM-1, but have NOT yet run on a real board. The first real install is the test.**
The protocol library is tested against Felucca's own Python installer, and the install sequence against a simulated FM-1 that follows the same steps.

## Stages
| Stage | What | State |
|---|---|---|
| 0 | **Link test**: plug the FM-1 into the board, the screen shows what the FM-1 says it is (e.g. `FM-1_900`). Read-only. | verified on a real board with a real FM-1 |
| 1 | **SD browser**: list the `.fwsc` files on the SD card and check each one (size, chip marker, product name, Felucca loader marker, official V15/V16 fingerprint) with the same rules as Slipway. Read-only. | verified on a real board |
| 2 | Stock-first rules (same as Slipway), saved on the SD card (`slipway.state`) | written (in `installer/`); not yet run on the board |
| 3 | **Install**, with the same steps and checks as Felucca's installer: check step (nothing written), update loader, write, verify. | written (in `installer/`); tested against a simulated FM-1 only |
| 4 | WiFi: download from the Slipway catalogue / GitHub | not started |

## The installer (stages 2 and 3)
`installer/` is the SD browser plus the install. Flash it once with esptool-js; after that, new versions go on from the SD card.
1. Run **Actions -> Build link test, SD browser and installer**. Download **slipway-installer** (`installer-merged.bin`) and flash it at `0x0`
   (hold BOOT while plugging in the DEBUG cable if it will not connect). Its partition table has two app slots, so this is the last flash that needs a PC.
2. Put the official stock file (**V15 or V16**) and any custom `.fwsc` files on a FAT32 SD card (top folder or a folder called `firmware`).
3. In the list press **OK** on the **official** file: it is checked and saved as your way back (`slipway.state` on the card).
4. Press **MENU** on the check screen to install. Plug the FM-1 (switched on) into the USB-A host socket with a data cable. The board asks the FM-1
   who it is, shows what will happen, and waits for **OK** (DW cancels).
5. **Stock first, every time:** the official stock file can always be installed. A custom firmware only goes on while the saved stock file is intact
   on the card and the FM-1 is on the official stock (or in update mode after an unfinished install). Otherwise put stock back first.
6. If a step fails before the write, nothing was written. If it fails during or after the write, the FM-1 stays in update mode: replug USB and run the same install again.

**Updating the board without a PC:** download **slipway-installer-update** (`slipway_installer.bin`), copy it onto the SD card, open it in the list (**OK**) and confirm.
It is written to the spare app slot and only selected when complete, so a failed update leaves the old version running. Only this project's own app image is accepted.

## Try the link test (no programming tools needed)
1. Put these files in a new GitHub repository (for example `slipway-portable`).
2. On GitHub open **Actions -> Build link test, SD browser and installer -> Run workflow**. It first runs the tests, then builds the firmware.
   When it finishes (a few minutes), download the **slipway-linktest** artifact and unzip it: you get `linktest-merged.bin`.
3. Connect the board's **Type-C debug port** to your PC. In Chrome or Edge open Espressif's web flasher
   (https://espressif.github.io/esptool-js/), click Connect, pick the board's serial port, set the flash address to
   `0x0`, choose the `.bin` and click Program. If it will not connect, hold the board's **OK/BOOT** button
   while plugging in the cable (or while pressing **RESET**), then try again.
4. Power the board from the **male USB-A plug** (a charger or power bank), switch the FM-1 on, and connect the FM-1 to the
   board's **USB-A socket** with a USB-A to USB-C **data** cable.
5. The screen should show the USB device, the MIDI interface, and then `FM-1 says: ...`.

If the build fails, copy the red error lines from the Actions log to me and I will fix it.

## What is and isn't tested
* **Tested on a PC:** `components/fm1proto` (the pure protocol code: 7-bit packing, read requests and answers, identity parsing,
  package header and marker bytes, USB-MIDI packet encoding) is compared byte for byte with Felucca's Python
  installer by `host_tests/run_tests.py` (1,000+ checks, built with AddressSanitizer and UBSan). `components/fwcheck` (the file rules) by `host_tests/run_fw_tests.py`.
  `components/fm1install` (the install sequence: check step, loader, write, verify, and the failure cases) against a simulated FM-1 on a virtual clock,
  plus the SD update logic and the stock record file, by `host_tests/run_install_tests.py`.
* **Verified on the real board:** the link test with a real FM-1 (`FM-1_900`), the SD browser (screen, buttons, SD card), the file checks.
* **Not tested on the real board:** everything in `installer/` that writes: the install itself (the simulated FM-1 follows what Felucca's Python does, it
  is not a real FM-1), the stock record on the card, and update from SD. Installing V16 is untested everywhere.
* **Not tested at all:** the case.

## Licence and credits
GPL-3.0-only (see `LICENSE`), because the protocol code is a port of Felucca's installer.
* **Felucca** and its installer, by Leo Kuroshita (@kurogedelic) / Hügelton Instruments, GPL-3.0: https://github.com/hugelton/Felucca
* **ESP32-S3-USB-OTG board support package**, Espressif (Apache-2.0): https://github.com/espressif/esp-bsp
* Board: Espressif ESP32-S3-USB-OTG (the Amazon version is a generic copy).

At your own risk. This is hobby software for a hobby device.
