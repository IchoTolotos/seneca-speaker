# Hardware bring-up: reSpeaker XVF3800 + XIAO ESP32-S3

Board: Seeed "ReSpeaker XMOS XVF3800 4-Mic Array with XIAO ESP32S3", SKU 114993700, bare board with no case.
Output: a powered speaker on the 3.5 mm jack, or a small passive speaker on the JST connector.
Target: full-duplex GPT-Live over WebRTC, using esp-webrtc-solution, esp_peer and Opus, with barge-in. The XVF3800 does the AEC.

Researched on 2026-10-24. The sources were Seeed wiki sources (`Seeed-Studio/wiki-documents`, branch `docusaurus-version`), `respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY` at commit `4b49bfd` (2026-09-29), `respeaker/reSpeaker_ESP32S3_USB_BRIDGE`, `Seeed-Projects/Xiaozhi_Esp32S3_reSpeaker`, `Seeed-Projects/ESP32S3_reSpeaker_agora`, `formatBCE/Respeaker-XVF3800-ESPHome-integration`, `espressif/esp-webrtc-solution` (2026-10-15), `espressif/esp-board-manager`, the XMOS XVF3800 docs, and the GitHub issues on these repos.
Where sources disagree, or I could not verify something, the text says so. Those points are also collected in the last section.

---

## 1. Overview

```
                 I2C 0x2C (control)          I2C 0x18 (codec, also visible on host bus)
     XIAO ESP32-S3 ───────────────► XVF3800 ───────────────► TLV320AIC3104 ──► 3.5 mm jack (HP out)
       GPIO5 SDA / GPIO6 SCL          │  ▲                       ▲            └► LM48511 class-D ─► JST speaker
                                      │  │ reference (far-end)   │
       GPIO8  BCLK ───────────────────┼──┼───────────────────────┤   (shared BCLK/WS)
       GPIO7  WS/LRCLK ───────────────┼──┼───────────────────────┤
       GPIO44 DOUT (playback) ────────┴──┘───────────────────────┘   ONE data line feeds BOTH XVF3800 DIN and codec DIN
       GPIO43 DIN  (mic) ◄──── XVF3800 processed output (2 ch)
       (no MCLK driven by the ESP32 in any Seeed example)
```

Key facts:

- **AEC reference is wired in hardware.** The ESP32's single I2S data-out line (GPIO44) goes to the XVF3800 audio input and to the AIC3104 DAC input in parallel. The XVF3800 uses that stream as its far-end (AEC) reference while the codec plays it. Seeed's USB-bridge `PLAYBACK.md` says: *"This data line feeds the reSpeaker XVF3800 audio input and the codec audio input in parallel, so both receive the same playback stream. The reSpeaker XVF3800 uses the stream as the far-end reference, and the codec converts it for playback."* ([PLAYBACK.md](https://github.com/respeaker/reSpeaker_ESP32S3_USB_BRIDGE/blob/main/PLAYBACK.md)). You do not have to route a reference yourself. Anything played through GPIO44 is the reference. Anything played some other way (for example a Bluetooth speaker) is not.
- **The XVF3800 runs one of two firmware families.** USB firmware makes it a USB Audio Class device. I2S ("INT-Device") firmware makes it talk to the XIAO over I2S/I2C. The XIAO variant must run I2S firmware.
- **The board ships with I2S firmware, but plan to reflash anyway.** The version from the factory is old in the units reported online (v1.0.4 in [formatBCE#52](https://github.com/formatBCE/Respeaker-XVF3800-ESPHome-integration/issues/52)). The current release is **v1.0.9** (Sept 2026). v1.0.7 changed the AEC reference delay default (`SYS_DELAY` 12 → −30), the setting the community found was blocking AEC convergence on 48k builds ([issue #31](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/issues/31), [I2S changelog](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/blob/master/xmos_firmwares/i2s/readme.md)).
- **Recommended start: `respeaker_xvf3800_i2s_slave_v1.0.9_16k.bin`.** With it the XVF3800 is the I2S slave and the ESP32 is the I2S master at 16 kHz, 32-bit stereo Philips. This matches every Seeed ESP32 example, the Xiaozhi and Agora ESP-IDF ports, and the master role that esp-webrtc-solution's I2S code uses by default. The fallback is `respeaker_xvf3800_i2s_master_v1.0.9_48k.bin`, where the XVF3800 is master and the ESP32 is slave at 48 kHz. That is what the Home Assistant/ESPHome community runs. See section 3.4 and the risks section for why we might switch.

---

## 2. Day-1 checklist

Do these in order. Each step has a pass criterion. Commands are for macOS on Apple Silicon.

### Step 0: Identify the hardware

1. Identify the **two USB-C ports**:
   - **XMOS USB-C**, on the bottom edge next to the 3.5 mm jack. Use it to DFU-flash the XVF3800. It is also a good single power input.
   - **XIAO USB-C**, on the opposite (top) edge, on the XIAO module. Use it for `idf.py flash monitor` (USB-Serial/JTAG).
   - Sources: [wiki "Update Firmware"](https://wiki.seeedstudio.com/respeaker_xvf3800_introduction/#update-firmware), [dfu_guide.md](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/blob/master/xmos_firmwares/dfu_guide.md).
2. Find the **Mute** button (side edge, labelled `BUT_A`/Mute) and the **RST** button (opposite side edge). Mute is also used to enter Safe Mode.
3. Make sure the XIAO's **U.FL antenna pigtail** is connected to the carrier's antenna. Wi-Fi depends on it ([XIAO getting started](https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_getting_started/)).
4. Connect the speaker. Start with the **powered speaker on the 3.5 mm jack at low volume**. The mic inlets are on the side with the "seeed studio" logo, which is also the side with the 12-LED ring. Point that side toward the talker.

### Step 1: Install tools

```bash
brew install dfu-util
dfu-util -V                      # expect 0.11+
# ESP-IDF: esp-webrtc-solution's openai_demo says IDF master or v5.4.
# esp_codec_dev 2.0.x requires IDF >=5.4.4 and excludes 5.5.0–5.5.2, so pick v5.4.x (>=5.4.4) or v5.5.3+.
IDF_TAG=v5.4.4   # or newer v5.4.x / v5.5.3+; check against the solution's resolved dependencies.lock
mkdir -p ~/esp && cd ~/esp && git clone -b $IDF_TAG --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3 && . ./export.sh
pip install --upgrade esp-bmgr-assist   # needed by current esp-webrtc-solution (esp_board_manager)
```

### Step 2: Get the XVF3800 firmware the right way

Use `git clone` or "Download ZIP". Seeed warns that saving the `.bin` from the GitHub web page with "save as" corrupts it.

```bash
mkdir -p ~/hw && cd ~/hw
git clone --depth 1 https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY.git
ls -la reSpeaker_XVF3800_USB_4MIC_ARRAY/xmos_firmwares/i2s/
#   respeaker_xvf3800_i2s_slave_v1.0.9_16k.bin    888832 bytes   <- primary choice
#   respeaker_xvf3800_i2s_master_v1.0.9_48k.bin   888832 bytes   <- fallback (HA/ESPHome uses this)
md5 reSpeaker_XVF3800_USB_4MIC_ARRAY/xmos_firmwares/i2s/*.bin
#   master_48k md5 on 2026-10-24: b62766ccf8fbbaf924d0d13beace495b (identical to formatBCE's bundled copy)
```

### Step 3 (optional): Read what is on the chip now

The I2S firmware **does not enumerate over USB**, so `xvf_host`/`dfu-util` on the Mac cannot see it. You read the version from the XIAO over I2C instead.
- Flash any small sketch or IDF app that scans I2C on SDA=GPIO5 and SCL=GPIO6. Expect **0x18** (AIC3104) and **0x2C** (XVF3800).
- Then read VERSION: write `[0x30, 0x80, 0x04]`, read 4 bytes, and expect `[status=0, major, minor, patch]`. Protocol details are in section 4.6.
- If 0x2C is missing, the chip is probably running USB firmware. Plug the XMOS port into the Mac and run `dfu-util -l`. If it lists `reSpeaker DFU Upgrade`, it is on USB firmware.

You can skip this step. Step 4 works whatever is on the chip.

### Step 4: Flash I2S firmware via Safe Mode and dfu-util

1. Unplug **all** power (both USB-C ports).
2. **Hold Mute**, then plug a data cable from the **XMOS USB-C** to the Mac while still holding it.
3. Release Mute when the **red LED blinks**. The chip is now in Safe Mode, the factory partition, which supports both USB DFU and I2C DFU.
4. Flash:
```bash
dfu-util -l
# expect:
# Found DFU: [2886:001a] ... alt=1, name="reSpeaker DFU Upgrade", ...
# Found DFU: [2886:001a] ... alt=0, name="reSpeaker DFU Factory", ...
dfu-util -R -e -a 1 -D ~/hw/reSpeaker_XVF3800_USB_4MIC_ARRAY/xmos_firmwares/i2s/respeaker_xvf3800_i2s_slave_v1.0.9_16k.bin
# -a 1 = "Upgrade" partition (NEVER write alt 0 "Factory"); -e = detach; -R = reset when done.
# "Warning: Invalid DFU suffix signature" is expected and harmless (see Seeed's sample log).
```
5. Unplug and replug power. A normal boot with I2S firmware does **not** show a USB device. That is expected.

Sources: [Safe Mode](https://wiki.seeedstudio.com/respeaker_xvf3800_introduction/#safe-mode), [Update Firmware](https://wiki.seeedstudio.com/respeaker_xvf3800_introduction/#update-firmware), [dfu_guide.md](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/blob/master/xmos_firmwares/dfu_guide.md).
The Xiaozhi wiki page says `-e` means "erase". In dfu-util, `-e` is `--detach`.

### Step 5: Check the version from the XIAO

Read VERSION (resid 48, cmd 0) as in Step 3. Expect `1.0.9`.
Caveat: [issue #29](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/issues/29) reports a file named v1.0.7 that reported v1.0.5. If the reported version does not match the file name, write down the version and the file's md5 and continue.

### Step 6: Minimal full-duplex I2S test (plain ESP-IDF, before WebRTC)

Use these settings for the **slave_16k** firmware. Section 3.3 has the reasoning.

- `i2s_new_channel(I2S_NUM_0, I2S_ROLE_MASTER)` with **both TX and RX on one controller**, so they share BCLK and WS.
- STD **Philips**, `sample_rate=16000`, `data_bit_width=32`, `slot_bit_width=32`, `ws_width=32`, stereo (`I2S_STD_SLOT_BOTH`). That gives BCLK = 1.024 MHz.
- Pins: `mclk=I2S_GPIO_UNUSED, bclk=8, ws=7, dout=44, din=43`.
- I2C: port 0, SDA=5, SCL=6, 100 kHz (Seeed's bridge default).
- **Console:** set `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`. UART0 TX/RX are GPIO43/44, which are now I2S pins.

Test:
1. Write a 1 kHz sine at −12 dBFS into **both** slots. You should hear the tone on the speaker.
2. At the same time, read RX and print RMS for L and R. Speaking should move the levels. Pass: values are not all-zero and not all `0xFFFFFFFF`.
3. Seeed's equivalent Arduino test is [respeaker_xvf3800_xiao_i2s](https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_i2s/). Its pass threshold is "more than half the samples non-zero".

### Step 7: Prove the AEC gets the reference

1. Read and print these values at boot. Command IDs are in section 4.6.
   - `AUDIO_MGR_SYS_DELAY` (35/26). Expect −30 on v1.0.7+.
   - `AUDIO_MGR_OP_L` (35/15) and `AUDIO_MGR_OP_R` (35/19).
   - `AEC_NUM_FARENDS` (33/72).
   - `I2S_INACTIVE` (35/24). Expect 0, meaning I2S is active.
2. As a temporary test, route **category 5, "reference with delay"**, to the R output: write `AUDIO_MGR_OP_R = [5, 0]`. While playing the tone, the R channel you capture must contain the tone. That shows the reference arrives over I2S. Then restore R.
3. Play 20–30 s of pink noise or speech at normal listening level and poll `AEC_AECCONVERGED` (**33/3**). Expect 1 within about 30 s.
   - [Issue #36](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/issues/36) "never converged" because the reporter polled the wrong command (33/91). The documented ID is 33/3.

### Step 8: Barge-in and double-talk test (go/no-go for this hardware path)

1. Play recorded speech, an assistant-like voice, continuously at the intended volume. Record the processed L channel for 30 s while you stay silent, then while you talk over it.
2. A/B against `SHF_BYPASS = 1` (33/70). Measure echo reduction: RMS during far-end-only with bypass, divided by RMS without bypass. Check that your own voice survives during double-talk.
3. Tune `PP_DTSENSITIVE` (17/31). The default 0 favours echo suppression. Try 12–15: two-digit values add an extra near-end detector and favour double-talk. That helps barge-in, if there is not too much non-linear distortion ([XMOS control appendix](https://www.xmos.com/documentation/XM-014888-PC/html/modules/fwk_xvf/doc/user_guide/AA_control_command_appendix.html)).
4. If convergence or double-talk is poor, run the **fallback**:
   - Flash `i2s_master_v1.0.9_48k`.
   - Reconfigure the ESP32 as I2S **slave**, 48 kHz, 32-bit stereo.
   - Repeat Steps 6–8. For SYS_DELAY, try −32, as in issue #31.

### Step 9 (optional, useful): Test the AEC from the Mac before writing firmware

Flash Seeed's [reSpeaker_ESP32S3_USB_BRIDGE](https://github.com/respeaker/reSpeaker_ESP32S3_USB_BRIDGE) Arduino sketch to the XIAO:
- Arduino-ESP32 **3.3.11**, USB Mode = USB-OTG (TinyUSB), CDC/MSC/DFU on boot disabled, PSRAM disabled.
- `#define BRIDGE_MODE BRIDGE_I2S_SLAVE_16K`.

The XIAO then shows up on the Mac as a 16 kHz stereo 32-bit UAC1 sound card plus a CDC port. You can play and record in Audacity, and send XVF3800 control commands through `host/bridge_host.py --port /dev/cu.usbmodemXXXX VERSION`.
Seeed says the 16 kHz modes "still require hardware validation", and only 48 kHz was tested on hardware ([wiki](https://wiki.seeedstudio.com/respeaker_xvf3800_i2s_usb_bridge/)).

### Step 10: Port to esp-webrtc-solution

See section 5.

### Step 11: Wire up the UI (LEDs and mute)

See section 6. At boot, set `MUTE_FUNCTION_ENABLE` (36/8) as needed, the LED effect, and the output routing. None of these persist unless you call `SAVE_CONFIGURATION`.

---

## 3. Firmware on the XVF3800

### 3.1 Variants (repo `xmos_firmwares/`, as of commit 4b49bfd, 2026-09-29)

| Image | Interface | XVF3800 I2S role | Rate / channels | Notes |
|---|---|---|---|---|
| `usb/respeaker_xvf3800_usb_dfu_firmware_v2.1.1.bin` | USB UAC2 | (I2S master to its own DAC) | Generic 2-ch | For PC/Pi use. **No I2S to the XIAO.** |
| `usb/..._v2.1.1_16k6ch.bin` | USB | – | 16 kHz, 6 ch (2 processed + 4 raw mics) | Raw mics on ch 3–6 |
| `usb/..._v2.1.1_48k2ch.bin` | USB | – | 48 kHz, 2 ch | Ships SYS_DELAY=+12, so AEC never converges until you set about −32 ([#31](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/issues/31)) |
| `i2s/respeaker_xvf3800_i2s_slave_v1.0.9_16k.bin` | I2S + I2C | **Slave** (ESP32 drives BCLK/WS) | 16 kHz, 2 ch, 32-bit | Successor of the old `i2s_dfu_firmware_v1.0.x` and `application_..._slave_v1.0.8_16k` |
| `i2s/respeaker_xvf3800_i2s_master_v1.0.9_48k.bin` | I2S + I2C | **Master** (XVF drives BCLK/WS) | 48 kHz, 2 ch, 32-bit | "HA firmware". Used by formatBCE ESPHome and by Seeed's WebSocket/DoA examples |
| `recover/4mb_all_ff.bin` | – | – | – | Flash-erase image for recovery |

- The I2S README says plainly that the role and rate are **compiled in**. No control command changes them. A 16 kHz master build is requested in [#34](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/issues/34) but has not been published. 6-channel (raw mic) output over I2S is not available ([#27](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/issues/27)).
- I2S changelog highlights:
  - **v1.0.9:** `MUTE_FUNCTION_ENABLE`, and a fix so `GPI_EVENT_PENDING_ALL` works.
  - **v1.0.8:** `DOA_VALUE` moved to cmd 18 and `LED_RING_COLOR` to cmd 19.
  - **v1.0.7:** AEC 125 Hz high-pass filter off; AGC max gain 64 → 32; **SYS_DELAY 12 → −30**; both I2S outputs routed to ASR beam 0; AIC3104 HP/line gain 6 → 3 dB.
  - **v1.0.5:** LED ring mode 5.
- Source: [i2s/readme.md](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/blob/master/xmos_firmwares/i2s/readme.md).

### 3.2 What ships, and do we need to flash?

- The Seeed XIAO page says: *"By default, the reSpeaker XVF3800 USB 4 Microphone Array with XIAO ESP32S3 uses the I2S firmware. In the I2S mode it is NOT detected as an USB device."* The Seeed FAQ says the same ([XIAO getting started](https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_getting_started/)).
- **Sources disagree:**
  - The Xiaozhi wiki page says *"The default USB firmware does not support I2S output, so you must flash a dedicated I2S firmware"* ([xiaozhi](https://wiki.seeedstudio.com/respeaker_xvf_3800_xiaozhi/)).
  - formatBCE says *"Some older boards have the Seeed USB firmware"*.
  - Treat the factory firmware as unknown and old.
- **Decision: flash `i2s_slave_v1.0.9_16k` on Day 1** (Step 4). It is cheap, it is recoverable through Safe Mode, and it gives us the post-v1.0.7 AEC delay default.

### 3.3 Flashing methods

| Method | Works when the chip runs | Tool |
|---|---|---|
| USB DFU over the **XMOS** USB-C | USB firmware, or **Safe Mode** | `dfu-util -R -e -a 1 -D file.bin` (macOS: `brew install dfu-util`) |
| I2C DFU from a host | **I2S firmware**, or Safe Mode | Seeed's `host_control/rpi_64bit/xvf_i2c_dfu` (Raspberry Pi only). formatBCE's ESPHome component also does I2C DFU from the ESP32 in 128-byte blocks, with the DFU servicer at resid 0xF0 ([dfu_cmds.yaml](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/blob/master/host_control/rpi_64bit/dfu_cmds.yaml)) |
| Safe Mode (factory partition) | Always (hold Mute while powering up; red LED blinks) | Supports USB DFU and I2C DFU |

- `xvf_host` (the C binary in `host_control/mac_arm64/`, and `python_control/xvf_host.py`) talks to the chip **over USB only** on macOS.
- With I2S firmware loaded it cannot reach the chip. Do control from the ESP32 over I2C, or use the USB-bridge sketch from Step 9.
- Later, for OTA of the DSP firmware, we could copy formatBCE's approach: embed the `.bin` in the ESP32 image and update over I2C when the versions differ.

### 3.4 Slave-16k vs master-48k

| | slave_16k (primary) | master_48k (fallback) |
|---|---|---|
| ESP32 I2S role | master (codec_board/board-manager default) | slave (`role: slave`) |
| Clock domain | BCLK from ESP32 PLL. The mic PDM clock comes from the XVF's own crystal (my assumption) | XVF3800 crystal clocks everything |
| AEC calibration | 16 kHz is the native/default calibration. SYS_DELAY −30 since v1.0.7 | The 48k USB builds had the wrong SYS_DELAY ([#31](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/issues/31)). Assume v1.0.7+ I2S builds have −30, but verify |
| Who uses it | All Seeed Arduino examples (I2S test, record/playback, volume), Xiaozhi port, Agora port | Seeed HA wiki, formatBCE ESPHome (large user base with on-device "stop" barge-in), Seeed WebSocket/DoA examples |
| Bandwidth / CPU | 16 kHz: Opus encodes and decodes at 16 kHz directly, no resampling | 48 kHz: resample mic 48→16 or 24 kHz, or run Opus at 48 kHz. 3× the I2S DMA load |

- [Issue #34](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/issues/34) argues that slave mode puts the converters and the bus in different clock domains, so the echo path drifts and the AEC "chases it". No vendor has confirmed this. Measure it in Step 8.
- Seeed's HA page also says *"The XVF3800 mic array needs a 12.288 MHz MCLK to work, but ESPHome … can't generate it … This firmware makes the XVF3800 act as the I2S master"* ([HA wiki](https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_home_assistant/)). That implies slave mode may want an MCLK. Yet no Seeed slave-mode example drives MCLK (AudioTools default, Xiaozhi `mclk = I2S_GPIO_UNUSED`, Agora `mck_io_num = -1`), and they are documented as working. See the open questions.

---

## 4. I2S/I2C wiring and the audio path

### 4.1 Pins (fixed on the PCB; consistent across all sources)

| Signal | ESP32-S3 GPIO | XIAO pin | Direction (ESP32 view) |
|---|---|---|---|
| I2S BCLK | **GPIO8** | D9 | out (slave_16k) / in (master_48k) |
| I2S WS/LRCLK | **GPIO7** | D8 | out / in |
| I2S DOUT → XVF3800 DIN **and** AIC3104 DIN | **GPIO44** | D7 (was UART0 RX) | out |
| I2S DIN ← XVF3800 processed audio | **GPIO43** | D6 (was UART0 TX) | in |
| MCLK | **not driven** in any Seeed example | (header pad "MCLK"; formatBCE has a commented-out `GPIO9` = D10) | – |
| I2C SDA | **GPIO5** | D4 | 100 kHz in Seeed's bridge |
| I2C SCL | **GPIO6** | D5 | |

Sources: [I2S test](https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_i2s/), [PLAYBACK.md](https://github.com/respeaker/reSpeaker_ESP32S3_USB_BRIDGE/blob/main/PLAYBACK.md), [Xiaozhi config.h](https://github.com/Seeed-Projects/Xiaozhi_Esp32S3_reSpeaker/blob/main/main/boards/xiao-esp32s3-sense/config.h), [formatBCE hardware.yaml](https://github.com/formatBCE/Respeaker-XVF3800-ESPHome-integration/blob/main/packages/hardware.yaml), [Agora board_pins_config_respeaker.c](https://github.com/Seeed-Projects/ESP32S3_reSpeaker_agora).

**Free XIAO pins for our own buttons:** D0–D3 (GPIO1–4) are broken out on the carrier's header as `XIAO_D0..D3`. GPIO3 (D2) is a strapping pin, so do not pull it at boot. GPIO9 (D10) may be the MCLK pad, which I have not verified. The XIAO also has its own BOOT button (GPIO0) and an orange user LED (GPIO21) on the module.

**Bus format** (Seeed bridge `PLAYBACK.md`):
- Standard **Philips I2S**: two 32-bit slots per frame, WS low = left, MSB one BCLK after the WS edge.
- 16 kHz gives BCLK 1.024 MHz; 48 kHz gives 3.072 MHz.
- Signed 32-bit samples. Treat the audio as left-justified and take the top 16 bits for Opus.
- **Keep 32-bit slots in both directions.** Opening the codec device at 16-bit would make BCLK 32·fs and break the framing the XVF expects. That is my inference, not tested.

### 4.2 Mic data from the XVF3800 (GPIO43)

- **2 channels** of processed audio. Raw mics are not available over I2S.
- The channel contents **depend on firmware version, and the sources disagree:**
  - Seeed intro wiki (I2S tab): "Channel 0: Conference, Channel 1: ASR". HA/master firmware: "Channel 0: ASR, Channel 1: Wake word".
  - I2S changelog v1.0.7: *"Routed both default I2S output channels to ASR output beam 0."*
  - formatBCE (master 48k) describes the taps as **comms**, which is the auto-select beam with noise suppression and adaptive AGC, and **ASR**, which is recognition-tuned with fixed gain. It switches between them at runtime.
- **What to do:** do not trust the defaults. At every boot, write `AUDIO_MGR_OP_L` and `AUDIO_MGR_OP_R` (resid 35, cmd 15 and 19, payload `[category, source]`). The outputs reset on power-up unless saved.
  - `[6, 3]` = processed beam, auto-selected (the "conference/comms" path).
  - `[7, 0]` = ASR beam 0, or AEC residual.
  - `[5, 0]` = reference with delay. Use it for debugging only.
  - Full category table: [python_control/readme.md](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/blob/master/python_control/readme.md#signal-categories-and-sources) and [XMOS output selection](https://www.xmos.com/documentation/XM-014888-PC/html/modules/fwk_xvf/doc/user_guide/03_using_the_host_application.html#output-selection).
- **Suggestion:** L = ASR beam (`[7,0]`) and R = comms beam (`[6,3]`). A/B which one GPT-Live's server VAD and recognition prefer, then send that one as mono Opus. Seeed's own ASR benchmark found neither channel consistently better ([FAQ](https://wiki.seeedstudio.com/respeaker_xvf3800_faq/)).

### 4.3 Playback path and the AEC reference

- ESP32 GPIO44 feeds the XVF3800 DIN (the reference, which the AEC uses) and the AIC3104 DIN (the DAC) in parallel. The AIC3104 drives the 3.5 mm headphone jack (HPLOUT/HPROUT). It also drives the line-out, which most likely feeds the **LM48511** class-D amp on the JST connector. The codec-to-amp routing is not documented, and I inferred it from the volume example's "HPLOUT vs LOP" switch.
- **The reference is taken automatically** from whatever you send on GPIO44. Write identical audio into L and R. Neither firmware says which slot the AEC uses as its far-end reference. `AEC_NUM_FARENDS` (33/72) tells you how many it uses.
- Seeed's I2C page describes two playback routes, "Host → XVF3800 → DAC" and "Host → DAC" ([I2C list](https://wiki.seeedstudio.com/respeaker_xvf_3800_i2c_list/)). That confused people ([#36](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/issues/36)). Physically, there is **one shared data line**, as stated in `PLAYBACK.md`. The two routes are about who controls the codec over I2C and whether the optional far-end DSP is applied.
- Leave `AUDIO_MGR_FAR_END_DSP_ENABLE` (35/25) and `I2S_DAC_DSP_ENABLE` (35/27) at their default 0. In #36, enabling them made things worse.
- **Volume control: do it digitally on the ESP32**, before I2S. Scaling the reference and the speaker together keeps the echo path constant, so the AEC does not have to re-converge.
  - Analog or codec volume changes (the AIC3104 DAC volume, the powered speaker's knob) change the echo-path gain. The AEC re-adapts, which causes a short echo burst.
  - Set the analog gain once and leave it alone.
- `AUDIO_MGR_REF_GAIN` (35/1) defaults to 1.5 according to XMOS. `AUDIO_MGR_SYS_DELAY` (35/26) is the reference delay in samples, range −64…256, and the I2S firmware defaults to −30.

### 4.4 Codec (TLV320AIC3104, I2C 0x18) and amp

- The XVF3800 firmware **initialises the codec itself**. The ESP32 does not need to send any codec init ([PLAYBACK.md](https://github.com/respeaker/reSpeaker_ESP32S3_USB_BRIDGE/blob/main/PLAYBACK.md)). I2S firmware v1.0.7+ sets the HP and line outputs to +3 dB.
- The AIC3104 still appears at **0x18 on the ESP32's I2C bus** (GPIO5/6). Seeed's [volume example](https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_volume/) writes DAC volume registers 0x2B/0x2C and output levels 0x33/0x41 (HP) and 0x56/0x5D (LOP). formatBCE's `aic3104` component does the same. The Agora example even soft-resets and reconfigures the codec from the ESP32.
  - **Risk:** Seeed also says the XVF3800 is the codec's I2C master. If both masters share the bus, collisions are possible. Avoid touching 0x18 unless you need to, and never during XVF boot.
- `AIC3104_HP_LEVEL` and `AIC3104_LINEOUT_LEVEL` (resid 48, cmd 11 and 12, range 0..9) appear in Seeed's I2C list. The changelog only mentions them for USB v2.1.0, so whether they exist in I2S v1.0.9 is unverified. Try them, and check that the status byte is 0.
- Amp enable is XVF GPO **X0D31, low = enabled**. Set it high (GPO_WRITE_VALUE `[31, 1]`) to silence the JST amp when you use the 3.5 mm output, to save power and avoid hiss.
- Seeed rates the JST output for *"5W amplified speakers"*. The onboard LM48511 is a class-D amplifier, so the JST almost certainly takes a **passive** speaker. The wording is ambiguous, and the speaker impedance and power ratings are not published.

### 4.5 Control interface of the XVF3800

- **I2C slave at 7-bit address 0x2C.** The ESP32 is the master.
- **Write:** `[resid][cmd][n][data…]`.
- **Read:** first write `[resid][cmd|0x80][n+1]`, then read `n+1` bytes `[status][data…]`.
  - status 0 = OK. Status 64 (0x40) = retry: the DSP answers "wait" while busy, especially for DoA during silence.
  - Codes 0x41 and 0x42 appear for invalid or unsupported commands ([#28](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/issues/28)).
- Multi-byte values are little-endian (the RGB example sends `R,G,B,0` low byte first). Floats are IEEE-754 LE, which I infer from xvf_host and have not verified byte-by-byte.
- The authoritative command table is the dictionary in [`python_control/xvf_host.py`](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/blob/master/python_control/xvf_host.py). It gives (resid, cmd, count, rw, type) and works for both USB and I2C.
- Parameters reset to defaults on reboot unless you call `SAVE_CONFIGURATION` (48/9). [#8](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/issues/8) is a case where saving a config "bricked" USB mode until Safe Mode. **Prefer re-applying settings from the ESP32 at every boot over saving them to flash.**

### 4.6 Commands we will use

| Purpose | Name | resid/cmd | Payload |
|---|---|---|---|
| Version | VERSION | 48/0 | ro, 3×u8 |
| Reboot | REBOOT | 48/7 | wo u8 |
| AEC converged? | AEC_AECCONVERGED | 33/3 | ro int32 |
| AEC bypass (A/B test) | SHF_BYPASS | 33/70 | rw u8 |
| # far-end refs | AEC_NUM_FARENDS | 33/72 | ro int32 |
| Ref delay | AUDIO_MGR_SYS_DELAY | 35/26 | rw int32 (samples) |
| Ref gain | AUDIO_MGR_REF_GAIN | 35/1 | rw float |
| Mic gain | AUDIO_MGR_MIC_GAIN | 35/0 | rw float |
| Output routing | AUDIO_MGR_OP_L / _R | 35/15, 35/19 | rw 2×u8 `[category, source]` |
| I2S running? | I2S_INACTIVE | 35/24 | ro u8 (0 = active) |
| Double-talk tradeoff | PP_DTSENSITIVE | 17/31 | rw int32, 0..5 or 10..15 |
| Echo suppression on/off | PP_ECHOONOFF | 17/23 | rw int32 |
| AGC on/off, max gain | PP_AGCONOFF, PP_AGCMAXGAIN | 17/10, 17/11 | rw |
| ASR output gain | AEC_ASROUTGAIN | 33/36 | rw float |
| DoA + speech flag | DOA_VALUE | 20/18 (v1.0.8+) | ro 2×u16 |
| Mic mute (and red LED) | GPO_WRITE_VALUE | 20/1 | `[30, 1]` mute, `[30, 0]` unmute |
| Amp enable | GPO_WRITE_VALUE | 20/1 | `[31, 0]` on, `[31, 1]` off |
| LED power | GPO_WRITE_VALUE | 20/1 | `[33, 1]` on |
| Mute built-in action | MUTE_FUNCTION_ENABLE | 36/8 (v1.0.9) | rw u8, 0 = button only reports events |
| Button events | GPI_EVENT_PENDING_ALL | 36/7 (fixed in v1.0.9) | ro u32 bitmap (bit 0 = Mute button) |
| Raw GPI levels | GPI_VALUE_ALL | 36/6 | ro |

---

## 5. Seeed example code, and how to describe this board in esp-webrtc-solution

### 5.1 Examples that exist

| Example | Framework | Firmware | I2S config | Simultaneous record + play? |
|---|---|---|---|---|
| [I2S test](https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_i2s/) | Arduino + AudioTools | I2S (slave) | `RXTX_MODE`, master, 16 kHz, 2 ch, 32-bit, pins 8/7/44/43 | Writes then reads (not concurrent) |
| [Record & playback](https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_record_playback/) | Arduino | I2S (slave) | Same, switches RX/TX with `end()/begin()` | **No** (half-duplex) |
| [Volume via I2C](https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_volume/) | Arduino | I2S | TX only, 16 kHz, 2 ch, **16-bit**, AIC3104 at 0x18 | No |
| [UDP](https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_udp_audio_stream/), [HTTP](https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_http_audio_stream/), [MQTT](https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_mqtt_audio_stream/) streaming | Arduino | I2S (slave) | RX, 16 kHz, 2 ch, 32-bit | No (capture only) |
| [WebSocket stream](https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_websocket_audio_stream/) | Arduino | **master 48k** | RX, 48 kHz, 2 ch, 32-bit | No |
| [RGB](https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_rgb/), [GPIO](https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_gpio/), [DoA/VAD](https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_doa_vad/), [I2C list](https://wiki.seeedstudio.com/respeaker_xvf_3800_i2c_list/) | Arduino (Wire) | I2S | – | – |
| [USB audio bridge](https://github.com/respeaker/reSpeaker_ESP32S3_USB_BRIDGE) | Arduino-ESP32 3.3.11 | slave16k / master16k / master48k | **Full-duplex**, Philips, 32-bit stereo. ESP32 master uses I2S0 TX+RX paired. ESP32 slave uses I2S0 TX + I2S1 RX sharing external clocks | **Yes**. The best reference for duplex I2S on this board |
| [Xiaozhi port](https://github.com/Seeed-Projects/Xiaozhi_Esp32S3_reSpeaker) ([wiki](https://wiki.seeedstudio.com/respeaker_xvf_3800_xiaozhi/)) | **ESP-IDF** | I2S (slave 16k) | `NoAudioCodecDuplex`: I2S0 master, 16 kHz, 32-bit, **mono left slot**, no MCLK. Octal PSRAM, 8 MB flash, console on USB-Serial/JTAG | **Yes** (voice assistant) |
| [Agora conversational client](https://github.com/Seeed-Projects/ESP32S3_reSpeaker_agora) ([wiki](https://wiki.seeedstudio.com/respeaker_xvf3800_agora_convo_client/)) | **ESP-IDF 5.2.3 + ESP-ADF** | I2S | i2s_stream, 32-bit, `ONLY_LEFT`, 16k or 8k, `mck_io_num=-1`. ADF software AEC commented out (relies on XVF) | **Yes** (real-time bidirectional with AI agent). The closest analogue to our project |
| [formatBCE ESPHome](https://github.com/formatBCE/Respeaker-XVF3800-ESPHome-integration) | ESPHome (ESP-IDF) | master 48k v1.0.9 (bundled, auto-flashed over I2C) | Secondary (slave), 48 kHz, 32-bit stereo in and out, octal PSRAM 80 MHz | **Yes**, with on-device "stop" during playback |

### 5.2 esp-webrtc-solution: codec_board vs esp_board_manager

**Important:** as of 2026-10-15, **every solution in esp-webrtc-solution, including `openai_demo`, uses `espressif/esp_board_manager ~0.7`**. It no longer uses `codec_board`/`board_cfg.txt`.
- `components/codec_board` is still in the repo as a legacy option.
- Board selection now works through `idf.py gen-bmgr-config -b <board>` / `idf.py bmgr -b <board> -c <custom_dir>` ([openai_demo README](https://github.com/espressif/esp-webrtc-solution/tree/main/solutions/openai_demo)).
- Both options are described below. Using esp_board_manager means less fighting with upstream.

**Option A: esp_board_manager custom board (recommended).** Scaffold with `idf.py bmgr -n boards/respeaker_xvf3800_xiao`, then edit. This is modelled on the `esp_sparkbot` board, which also shares one I2S port for in and out ([board manager repo](https://github.com/espressif/esp-board-manager)). Check the exact keys against your installed esp_board_manager version.

`board_peripherals.yaml`:
```yaml
version: 1.0.0
peripherals:
  - name: i2c_master
    type: i2c
    role: master
    config:
      port: 0
      pins: {sda: 5, scl: 6}

  - name: i2s_audio_out
    type: i2s
    role: master            # slave_16k XVF firmware. For master_48k XVF firmware: role: slave
    format: std-out
    config: &i2s_cfg
      port: 0               # SAME port for in and out -> shared BCLK/WS
      sample_rate_hz: 16000 # 48000 for master_48k
      clk_src: I2S_CLK_SRC_DEFAULT
      mclk_multiple: 256
      data_bit_width: 32
      slot_bit_width: I2S_SLOT_BIT_WIDTH_32BIT
      slot_mode: I2S_SLOT_MODE_STEREO
      slot_mask: I2S_STD_SLOT_BOTH
      ws_width: 32
      ws_pol: false
      bit_shift: true       # Philips
      left_align: true
      big_endian: false
      bit_order_lsb: false
      pins: {mclk: -1, bclk: 8, ws: 7, dout: 44, din: 43}

  - name: i2s_audio_in
    type: i2s
    role: master            # must match i2s_audio_out
    format: std-in
    config: *i2s_cfg
```

`board_devices.yaml`:
```yaml
version: 1.0.0
devices:
  - name: audio_dac                 # ESP_BOARD_DEVICE_NAME_AUDIO_DAC
    chip: internal                  # internal + dac_enabled -> esp_codec_dev "dummy" codec (no I2C ctrl)
    type: audio_codec
    config: {adc_enabled: false, dac_enabled: true}
    peripherals:
      - i2s_name: i2s_audio_out

  - name: audio_adc                 # ESP_BOARD_DEVICE_NAME_AUDIO_ADC
    chip: internal                  # internal + adc_enabled + input i2s -> plain I2S data, codec_if NULL
    type: audio_codec
    config: {adc_enabled: true, dac_enabled: false}
    peripherals:
      - i2s_name: i2s_audio_in
```

Plus `board_info.yaml` (chip esp32s3) and sdkconfig defaults:
```
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y               # openai_demo defaults to 120M + flash 120M/HPM; use 80M on XIAO unless proven
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y    # GPIO43/44 are I2S
```
- Do **not** model the AIC3104 as a codec device. The XVF firmware owns it, and the board manager has no AIC3104 driver.
- Make the XVF3800 control a small component of our own on the `i2c_master` handle at 0x2C. Get it with `esp_board_periph_get_handle("i2c_master", …)` or the equivalent API in your version.

**Code changes in `openai_demo`'s `media_sys.c`, or our copy of it:**
1. **Turn off ESP-side AEC.** Replace `esp_capture_new_audio_aec_src(&codec_cfg)` with `esp_capture_new_audio_dev_src(...)`. The commented-out line is already in the demo. The XVF3800 already removed the echo, and a second AEC that has no reference channel would do harm.
2. **Record format:** open the record device as 16 kHz, 2 ch, 32-bit to match the bus, then take one channel and convert 32→16 bit before the Opus encoder. Check whether `esp_capture`'s dev source can do the channel pick and bit conversion. If not, add a thin custom `esp_capture_audio_src_if_t`.
3. **Playback format:** the demo sets `av_render_set_fixed_frame_info(16 kHz, 2 ch, 16-bit)`. The I2S bus must stay at **32-bit slots**, so either set the fixed frame to 32-bit, if av_render's I2S render supports it, or convert in a custom render. Duplicate mono into L and R.
4. **Keep both I2S directions running for the whole session.** Send silence when nothing is playing.
   - The AEC needs a continuous reference.
   - In slave_16k mode, the XVF loses its clocks if the ESP32 stops the I2S master.
   - Watch out for `esp_codec_dev_close()` on the playback device when av_render goes idle. In duplex mode TX and RX share one clock, so check that RX keeps running.
5. **Remove `esp_codec_dev_set_in_gain(…, 32)`.** It does nothing on the dummy device, but it is misleading. Do **volume** with `esp_codec_dev_set_out_vol` on the dummy codec, which is software volume and is applied before I2S (good for AEC, see 4.3). Or scale samples yourself.

**Option B: legacy `codec_board` `board_cfg.txt` entry.** This works only if we pin an older solution commit, or call `codec_board` directly:
```
Board: RESPEAKER_XVF3800_XIAO
i2c: {sda: 5, scl: 6}
i2s: {mclk: -1, bclk: 8, ws: 7, dout: 44, din: 43}
in_out: {codec: DUMMY, pa: -1}
```
- `codec_board`'s `_i2s_init()` **hard-codes `I2S_ROLE_MASTER`**. With master_48k XVF firmware you would have to patch it to `I2S_ROLE_SLAVE`.
- It starts in MSB (left-justified) slot format. `esp_codec_dev` reconfigures to **Philips** on `esp_codec_dev_open()` (`audio_codec_data_i2s_hw.c` uses `I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG`), so the open-time format is what counts.
- The bundled `XIAO_ESP32S3_Sense` entry (PDM mic, GPIO41/42) is **not** our board.

---

## 6. LEDs and buttons

- **12× WS2812(-2020) RGB ring**, on the **mic/logo side**. They are **driven by the XVF3800**, not by an ESP32 GPIO. Control them over I2C, resid 20:
  - `LED_EFFECT` (cmd 12): 0 off, 1 breathing, 2 rainbow, 3 solid, 4 DoA, 5 ring.
  - `LED_BRIGHTNESS` (13), `LED_SPEED` (15), `LED_COLOR` (16, u32 RGB), `LED_DOA_COLOR` (17).
  - `LED_RING_COLOR` (**19** on v1.0.8+, 12×u32, one per LED with effect 5).
  - LED power is gated by GPO X0D33 (high = on).
  - formatBCE runs all its voice-state animations by writing `LED_RING_COLOR` every 50 ms from the ESP32. That is a proven pattern for listening, thinking and speaking states.
  - Sources: [RGB example](https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_rgb/), [I2C list](https://wiki.seeedstudio.com/respeaker_xvf_3800_i2c_list/).
- **Red mute LED** is tied to GPO X0D30, which mutes the mics in the DSP when high and lights the LED.
- **Mute button** is XVF GPI X1D09 (high when released). By default it toggles DSP mute in hardware, which works even if the ESP32 hangs.
  - On I2S v1.0.9 you can set `MUTE_FUNCTION_ENABLE=0` (36/8). The button then only produces events, which you poll with `GPI_EVENT_PENDING_ALL` (36/7). That makes it a general **talk/stop button**.
  - The setting is **not persisted** on I2S firmware, so re-send it at every boot.
  - The same button is used to enter Safe Mode at power-up.
- **RST button:** hardware reset of the **XVF3800** (and the XIAO can be reset through an IO line, per the wiki).
- **On the XIAO module:** BOOT (GPIO0) and RESET buttons, and an orange user LED (GPIO21). They are tiny and will be hidden in most enclosures.
- **Extra buttons:** wire them to `XIAO_D0..D3` (GPIO1–4) on the carrier header. Avoid GPIO3 (strapping).
- **Caution:** [#18](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/issues/18) reports mics going silent and DoA freezing after experimenting with LED/GPO writes and SAVE/CLEAR_CONFIGURATION on USB firmware, and reflashing did not fix it. Only write the GPOs listed here, and avoid `SAVE_CONFIGURATION` during experiments.

---

## 7. Power

- **Use one input.** Power the whole board from **either** USB-C port, not both at once.
  - The formatBCE maintainer: *"stop feeding both USB ports. It should work with one, the XVF one (closer to the audio jack)"* ([formatBCE#52](https://github.com/formatBCE/Respeaker-XVF3800-ESPHome-integration/issues/52)).
  - The same thread shows the board also runs from the XIAO port alone. Seeed's Agora guide uses the XIAO USB-C for power and flashing.
- **During development:** power from the **XIAO USB-C**, so you get flash and monitor. The XMOS port is only needed for DFU in Safe Mode, and it has no ESP32 serial console.
- **In the finished device:** one 5 V USB-C supply into either port.
- **Current:** Seeed does not publish a figure. My estimate, **not from a datasheet**:
  - ESP32-S3 Wi-Fi peaks of about 0.35–0.5 A.
  - XVF3800 plus codec, a few hundred mA at most.
  - WS2812 ring up to a few hundred mA at full white.
  - The LM48511 boosted class-D amp on the JST output can draw more than 1 A on peaks at "5 W".
  - Budget **5 V / 2 A** if you use the JST amp, and about 1 A for AUX-only with dim LEDs. Measure with a USB power meter on Day 1.
- The header also exposes 5 V, 3.3 V and GND for accessories.
- **Powered speaker:** give it its own supply.
  - If you hear hum or buzz (a ground loop between the speaker PSU and the board's USB supply), power both from the same supply, or use an AUX ground-loop isolator.
  - Hum is also bad for the AEC, because it is uncorrelated noise.
  - Avoid speakers with heavy DSP or "bass boost", long internal latency, or Bluetooth. SYS_DELAY only covers −64…+256 samples (−4…+16 ms at 16 kHz).

---

## 8. Known issues and community reports

| Topic | Report | Implication |
|---|---|---|
| AEC never converges at 48 kHz (USB builds) | [#31](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/issues/31): SYS_DELAY default +12 is wrong at 48k. −8…−64 converges; −32 is recommended | The I2S v1.0.7+ default is −30. Verify by reading `AUDIO_MGR_SYS_DELAY` and `AEC_AECCONVERGED` |
| "Device hears itself" (I2S master v1.0.8, AIC3104 playback example) | [#36](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/issues/36): DoA/VAD `SPEECH_DETECTED` latched for 1–2 s after every self-played hit. The reporter worked around it in host logic | VAD/DoA is not proof of AEC failure (the reporter polled the wrong command ID). **Do not use the XVF VAD flag as a barge-in trigger without gating it on playback.** Measure ERLE in the audio itself (Step 8) |
| Slave mode clock domains | [#34](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/issues/34): theory that slave mode drifts the echo path. Request for a 16 kHz master build (open) | This is why the 48k master fallback exists. Measure |
| I2S data all zeros | [formatBCE#46](https://github.com/formatBCE/Respeaker-XVF3800-ESPHome-integration/issues/46) (custom board, v1.0.7 master) and [#52](https://github.com/formatBCE/Respeaker-XVF3800-ESPHome-integration/issues/52) (**XIAO case version**, flat-zero mic data after flashing USB and then I2S; maintainer suspects hardware). Also [#7](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/issues/7) | The firmware role must match the ESP32 role (two masters or two slaves gives silence). The Step 6 test catches this. Also try Safe Mode reflash and a full power cycle |
| USB ↔ I2S switching leaves I2C dead | formatBCE#52: after USB firmware, 0x2C does not answer (only 0x18), so I2C DFU cannot fix it | Use Safe Mode + USB DFU to get back to I2S firmware |
| SAVE_CONFIGURATION bricks USB mode | [#8](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/issues/8) | Avoid saving. Re-apply settings at boot |
| Version labels vs reported version | [#29](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/issues/29) (file v1.0.7 reports 1.0.5). [#32](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/issues/32) (same filename published twice with different bytes) | Record the md5 of what you flash |
| DoA cmd returns 0x41/0x42 | [#28](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/issues/28). The ID moved to 20/18 in v1.0.8 | Use v1.0.9 IDs |
| Firmware is closed source | [#33](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/issues/33) | We cannot fix DSP bugs ourselves |
| XIAO pin conflicts | GPIO43/44 are UART0 (console must go to USB-Serial/JTAG). The ROM bootloader still prints on UART0 TX = GPIO43 at reset, which briefly drives against XVF DOUT. Seeed's design accepts this, and eFuse `BOOT_ROM_LOG_ALWAYS_OFF` would stop it. GPIO7/8 are also the XIAO SPI pins | Do not use the XIAO SPI or UART0 on the default pins |
| PSRAM type | XIAO ESP32S3 = **ESP32-S3R8: 8 MB octal PSRAM + 8 MB flash** ([CNX](https://www.cnx-software.com/2023/03/22/xiao-esp32s3-tiny-esp32-s3-wifi-4-and-ble-5-0-module-for-iot-ai-robotics/)). Xiaozhi uses `CONFIG_SPIRAM_MODE_OCT=y`; formatBCE uses `psram: mode: octal, speed: 80MHz` | Use **octal**. Start at 80 MHz. openai_demo's 120 MHz + 4 MB flash defaults must be overridden |
| Mute button default | Hardware mute is independent of the ESP32 | On I2S v1.0.9, use MUTE_FUNCTION_ENABLE=0 for custom logic |
| External speaker not on GPIO44 | formatBCE README: *"An external speaker gives no echo signal to the XVF3800 canceller"* | All assistant audio must go out through GPIO44 |

---

## 9. Enclosure

- Seeed's wiki Resources section ([intro page](https://wiki.seeedstudio.com/respeaker_xvf3800_introduction/#resources)) publishes:
  - 2D drawing: `respeaker_xvf3800_2d_mechanical_drawing.pdf`.
  - **Board STEP including the XIAO**: `3d/respeaker_mic_array_xvf3800_1_with-xiao-0820.stp` (16 MB, Creo, 2025-08-20).
  - **Enclosure halves**: `3d/1-up.stp` and `3d/1-down.stp` (Creo, 2025-09-28).
- I measured the enclosure from the STEP vertices:
  - Both halves are about **108 mm in diameter**. Together they are about **18 mm tall**: the upper part spans about 13 mm and the lower about 10.4 mm, with some overlap.
  - Both halves have openings at two opposite ends (±90°) and features at 0° and 180° (the button positions).
  - That pattern fits cutouts for the XMOS USB-C plus 3.5 mm jack on one end and the XIAO USB-C on the other. **I could not confirm this in CAD.**
- **Does it account for the XIAO? Probably yes, but unverified.**
  - Seeed sells a **"Case Version" of the XIAO variant** (The Pi Hut: "reSpeaker XMOS XVF3800 with XIAO ESP32S3 (Case Version)"). The FAQ describes a clip-fit case with **three locking clips** ([FAQ](https://wiki.seeedstudio.com/respeaker_xvf3800_faq/)). formatBCE#52 is from a user with the "XIAO ESP32S3 (case version)".
  - There is no separate STEP for a non-XIAO case. The board STEP is explicitly "with-xiao".
  - Before printing, load `with-xiao-0820.stp` together with `1-up.stp` and `1-down.stp` in FreeCAD or Fusion. Check the XIAO's USB-C and U.FL antenna clearance, the JST connector, and the mic-port and LED alignment.
- **Our needs differ from Seeed's case.** It is a slim puck with no room for a speaker. If we use the JST and a passive driver, we need our own design. Points for that design:
  - Mic inlets and the LED ring are on the logo side and should face the room. Use light pipes or a diffuser for the LEDs.
  - Isolate the speaker mechanically from the PCB (soft mounts) and acoustically (a sealed back volume). Structure-borne vibration and rattles are **non-linear echo** that the AEC cannot cancel.
  - Put the speaker below or away from the mic plane.
- **Third-party enclosures:** I found **no** XVF3800-specific designs on Printables, Thingiverse or MakerWorld. Searches only returned cases for the bare XIAO ESP32-S3, for example [Printables 664858](https://www.printables.com/model/664858-seeed-studio-xiao-esp32-s3-case), which do not fit this board. Seeed's acrylic enclosure accessory is for the reSpeaker Lite, not this product.

---

## 10. Open questions and risks

1. **AEC quality in slave_16k vs master_48k.** No public measurement of ERLE or double-talk on either I2S build. Step 8 is the go/no-go test; switch to master_48k if slave drifts. *Risk: medium-high. This is the core requirement.*
2. **MCLK.** Seeed's HA page says the XVF3800 "needs a 12.288 MHz MCLK" (which is why the master firmware exists). All Seeed slave-mode examples run without MCLK. It is unknown whether the header "MCLK" pad goes to a XIAO GPIO (formatBCE comments out GPIO9). If slave_16k gives zeros or poor AEC, try driving MCLK on GPIO9, after checking continuity with a meter.
3. **Default output channel mapping** varies by version and document (section 4.2). Always set `AUDIO_MGR_OP_L/R` explicitly.
4. **Which reference slot** (L, R or both) the AEC uses is not documented. Send identical L and R.
5. **AIC3104 shared I2C bus.** Is the codec on the same bus as the host and the XVF as master? Seeed's wording conflicts with the observed 0x18 on the host bus. Avoid host writes to 0x18 if possible.
6. **`AIC3104_HP_LEVEL` / `_LINEOUT_LEVEL` on I2S firmware** are unverified (listed on the I2C page, changelogged only for USB).
7. **esp-webrtc-solution plumbing.** av_render and esp_capture with 32-bit I2S and channel picking need verifying. Keeping I2S running continuously when the player is idle also needs care. The upstream move from `codec_board` to `esp_board_manager ~0.7` means the YAML keys above must be checked against the installed version.
8. **Factory firmware is unknown** (I2S per the main docs, USB per the Xiaozhi page and older boards). Covered by reflashing on Day 1.
9. **Power budget** is unpublished. Measure it. A ground loop with a separately powered AUX speaker is possible.
10. **XVF3800 VAD/DoA flags trigger on our own playback** (#36). Do not drive barge-in UI from them while speaking. Let GPT-Live's server VAD and the clean AEC'd stream do it.
11. **Enclosure fit for the XIAO** has not been verified in CAD. No third-party enclosures exist. A speaker enclosure is our own work.
12. **The firmware is closed source.** If AEC is inadequate, the only levers are the control parameters (SYS_DELAY, REF_GAIN, DTSENSITIVE, GAMMA_E/ENL, NLATTEN, AGC) and the acoustic design.
