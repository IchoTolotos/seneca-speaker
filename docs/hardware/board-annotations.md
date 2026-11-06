# reSpeaker XVF3800 board photo legend

Seeed **reSpeaker XVF3800 4-Mic Array with XIAO ESP32-S3**, SKU **114993700**. The back silkscreen reads **reSpeaker XVF3800 4-Mic Array V1.1**.

- [Front / component side](board-front-annotated.png)
- [Back / LED side](board-back-annotated.png)

The front keeps the input orientation. The back is rotated **180°** so the **cable-connected XIAO USB-C is at the top in both images**. In the original back photograph, the bare port at the top is the **XMOS USB-C**; the cable-connected XIAO port is at the bottom. Numbers are shared between images. Repeated numbers and matching outlines identify multiple parts in a group; they are not electrical pin numbers.

## Setup first

Flash and use serial through **1, XIAO USB-C**. To force download mode, **hold 3 (B / BOOT), tap and release 2 (R / RESET), then release B**. These are the two tiny buttons beside the XIAO port, shown in the enlarged inset. **11 (board RST) is a separate button.** Use **7, XMOS USB-C**, for the upcoming XVF3800 firmware update. Connect the **powered speaker to 8, the 3.5 mm jack**. Keep the XIAO antenna connection attached.

## Numbered parts

| # | Name | What it is for | Needed now? |
|---|---|---|---|
| 1 | **XIAO USB-C**, cable connected | ESP32-S3 flashing and serial connection. It belongs to the small XIAO module. | **Yes — flashing / serial.** |
| 2 | **R / XIAO RESET** | Tiny button to the **left** of the XIAO USB-C in the front photo; resets the ESP32-S3. | **Yes — download-mode sequence.** |
| 3 | **B / XIAO BOOT** | Tiny button to the **right** of that port; hold while resetting to force ESP32-S3 download mode. | **Yes — download-mode sequence.** |
| 4 | XIAO ESP32-S3 module | Small module with the metal shield marked “seeed studio” and “XIAO-ESP32-S3”; runs the ESP32 application. | Yes, already installed; no separate connection here. |
| 5 | Wi-Fi antenna lead / U.FL connection | Thin black coax and gold connectors near the XIAO. The module-side connection is visible. | **Keep connected.** The antenna element / other-end connection cannot be established from these photos. |
| 6 | XMOS XVF3800 voice processor | Large square chip marked **XMOS**, near the middle; processes the microphone audio. | Used automatically; update through **7**. |
| 7 | **XMOS USB-C**, bottom of front | XVF3800 DFU firmware update and board power. | **Yes — upcoming XVF3800 update.** |
| 8 | **3.5 mm headphone / line-out jack** | Audio output for the **powered speaker**, beside the bottom USB-C. | **Yes — speaker connection.** |
| 9 | **SPK**, 2-pin JST | On-board class-D amplifier output for a small **passive** speaker. “SPK” is visible. | No — use **8** for the powered speaker. |
| 10 | **BUT_A** side button | Mute control; also used for XVF3800 safe mode. Right edge in the front photo. | As needed for mute / firmware recovery. |
| 11 | **RST** side button | Board reset. Left edge in the front photo. | As needed; use **2**, not this button, in the XIAO BOOT sequence. |
| 12 | Four MEMS microphones | Four small silver/beige square packages near the perimeter, each outlined pink and marked **12**. | Used automatically; keep the sound openings clear. |
| 13 | XMOS expansion GPIO header **pads** | Unpopulated two-column header footprint on the left, labelled with X0D… / X1D…, 5V, GND and VDDIO. | No. |
| 14 | **20-pin FPC connector footprint** | Two exposed rows of gold contacts right of center, with **2 / 20** and **1 / 19** markings. **No plastic FPC connector is fitted in this photo.** | No. |
| 15 | **GND / 5V** power pads | Solder pads near the bottom edge, right of the XMOS USB-C. | No — no soldering needed for this setup. |
| 16 | XIAO GPIO breakout pads | Four holes/pads at the top-left, visibly labelled **GND, D3, D2, D0** in that order. **D1 is not visible in this row.** | No. |
| 17 | **4R7** inductor / power circuitry | Readably marked “4R7” component in the lower middle and its surrounding power circuit. | Used automatically; nothing to connect. |
| 18 | Audio codec / amplifier area | Two outlined IC areas around the lower-middle circuitry. Individual part identities are not established from the photo. | Used automatically; nothing to connect. |
| 19 | **12 RGB LEDs** | Twelve cyan-outlined packages around the back form the feedback ring. | Observe as needed; firmware determines the display. |
| 20 | Small center status LED | Small rectangular LED near the center of the back, below the barcode after rotation. | Observe as needed; no specific status meaning is established from the photo. |
| 21 | Mounting holes / points | Six inner, white-silkscreened circular mounting positions, highlighted green. Three larger open holes are clear; the three smaller centers are marked as positions because their drilling/plating is not fully clear in the photo. Screw sizes are not established. | No, unless mounting the board. |
| 22 | Microphone sound inlets | Four small holes near the outer perimeter on the back, highlighted pink; these align with the four front microphone packages. | Keep clear; these are not the inner mounting positions. |

## Photo limits

The input photos were read only; the PNGs are cropped, resized annotated copies. Identification uses visible placement and silkscreen plus the supplied board functions. The FPC connector is **unfitted**, the antenna's other end is not identifiable, and codec/amp chip model numbers are not legible enough to assign. D1 is not visible among the four top-left breakout labels. The three smaller mounting centers are not clear enough to establish their drilling/plating. No firmware status meanings, pin electrical specifications or screw dimensions were inferred.
