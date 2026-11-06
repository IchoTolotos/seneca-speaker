# Seneca firmware (ESP-IDF 5.5, ESP32-S3)

Hardware: Seeed **ReSpeaker XVF3800 4-mic array + XIAO ESP32-S3** (ESP32-S3R8: 8 MB octal PSRAM,
8 MB flash), a passive 4 Ω speaker on the board's speaker connector (or a powered speaker on the 3.5 mm
jack), optionally an AirPlay 2 speaker. Details: `../docs/hardware-bringup.md`.

The device owns authentication, wake detection, and WebRTC. Signaling HTTP uses the private
WireGuard relay on a home machine (`../relay/README.md`); voice media goes directly to ChatGPT:

1. `GET https://chatgpt.com/api/auth/session` with the NextAuth session cookie returns an `accessToken`.
   The cookie rotates on every call and is persisted A/B in NVS.
2. `POST https://chatgpt.com/realtime/wm?dcid=0` (multipart `sdp` + `session`) returns the SDP answer.
3. WebRTC through `esp_peer`: Opus both ways, a data channel on stream 0, and audio straight to ChatGPT.

The ESP32's direct TLS fingerprint was challenged by Cloudflare. The relay forwards only the
auth/signaling routes over HTTP/1.1; the speaker reaches it through WireGuard.

Status: **in daily use on real hardware.** Sign-in survives reboots, "Seneca" starts a call,
barge-in and voice hang-up work, with output on the built-in speaker or an AirPlay 2 speaker
(echo cancellation in both cases).

## Architecture

```
main.c        NVS -> media/I2S + Seneca -> Wi-Fi -> SNTP -> tunnel -> probe -> controller/setup
setup.c       local sign-in/status/control HTTP API; embedded web/setup.html
voice_control portable PCM speech-hold monitor and addressed user hang-up phrase recognition
wake_word.cpp streaming Seneca TFLite Micro inference, matching 40-channel frontend, cutoff 0.72
controller.c  ONE task owning speaker_core's sc_fsm_t + sc_wm_tracker_t; input queue, 1 s TICK,
              executes actions. A worker task runs blocking jobs in order:
                START_AUTH  -> chatgpt_auth.c  (GET /api/auth/session, rotate + persist cookies)
                START_CALL  -> rtc_peer.c offer -> chatgpt_wm.c POST /realtime/wm -> apply answer
                HANG_UP     -> rtc_peer_stop()
              STOP_SPEAKING / mic mute / LED are executed in the controller itself.
              Local sustained speech during assistant playback sends STOP_SPEAKING, flushes and
              briefly suppresses downlink audio. Only user transcript “Seneca, stop” ends a
              call through the voice-command handler. Split transcription fragments are supported.
              Primary control-channel closure ends the call; ignored streams cannot affect it.
              Mute persists in NVS; fresh setup starts muted.
              BOOT button (GPIO0): short = WAKE (or stop_speaking while it talks),
              1-4 s = hang up, >= 4 s = mute toggle. CONFIG_GLS_AUTO_CALL_ON_BOOT posts WAKE once.
rtc_peer.c    esp_peer directly (esp_webrtc is not on the registry): audio-only offer, data
              channel, pc_task (main loop) + pc_send (Opus -> RTP), callbacks -> controller
media.c       uplink:   XVF3800 I2S -> custom esp_capture source (PCM 16k mono) -> Opus encoder
              downlink: Opus -> av_render (decode, resample to 16k mono) -> PSRAM jitter buffer
              (adaptive 200-800 ms, stretches/drops quiet 10 ms slices) -> playout task -> I2S
              Only Opus is registered. There is no software AEC: the XVF3800 does AEC.
xvf3800_i2s.c I2S0 master, 16 kHz, 32-bit slots, stereo Philips, TX+RX paired, BCLK 8 / WS 7 /
              DOUT 44 / DIN 43, no MCLK. Enabled once and never stopped (TX auto-clears to
              silence), so the XVF3800 gets a steady clock and a continuous AEC reference.
              Mic = top 16 bits of slot CONFIG_GLS_MIC_SLOT. Playback = same sample in L and R
              after digital volume. A continuous reader fans out 10 ms blocks to Seneca and
              the voice FIFO; encoder capture only runs during calls.
              Speaker path: voice boost, volume, 4-band EQ (biquads), soft limiter.
              AirPlay mode: the mix goes to the AirPlay sender; the amp is off and the I2S line
              carries the AEC reference = exactly what the AirPlay receiver plays at each sample's
              output time (RTP-indexed send history + I2S DMA timestamps + measured receiver delay).
xvf_ctrl.c    XVF3800 control over I2C (0x2C): version, amp GPO, echo/double-talk parameters
airplay_tx/   component: AirPlay 1/2 sender (vendored AirPlayKnob, HomeKit transient pairing,
              ChaCha20-Poly1305, realtime stream, FLUSH on barge-in), mDNS discovery, volume
airplay_measure.c  acoustic measurement with the board's own mics: receiver delay, flush time
chime.c       short UI sounds; audio_diag.c: mic/speaker levels, recordings for the setup page
sc_port.c     speaker_core platform: clock (esp_timer), random (esp_random), log (ESP_LOGx),
              KV over NVS namespace "gls_sess" (each set commits)
gls_http.c    esp_http_client wrapper: cert bundle, Safari UA, 16 KB rx / 8 KB tx buffers,
              per-header callback (Set-Cookie), cf-mitigated/content-type/Date capture
probe.c       day-1 reachability probe
led.c         GPIO21 patterns for sc_led_t
```

The portable logic is all in `components/speaker_core`: the FSM, `wm_events` decoding, cookie
jar, Set-Cookie parsing, A/B cookie store, multipart, session JSON, JWT and UUID. `main/` only adds
ESP-IDF glue. Host tests: `cmake -S components/speaker_core/test/host -B /tmp/sc-build && cmake --build /tmp/sc-build && ctest --test-dir /tmp/sc-build`.

### Data-channel events -> FSM

`state_update` → `REMOTE_STATE`. `goodbye`/`close_*` → `REMOTE_CLOSE`. `usage_update` → `USAGE`.
`spawn_update` → `SPAWN`. User transcripts and tool messages → `ACTIVITY`.
`conversation_id` is remembered and passed on rotation, so the conversation continues.

**Remote speaking:** an incoming audio packet, or an assistant `chat_message_delta` with
`direction:"out"`, or a live caption within the last 1.5 s, feeds `REMOTE_STATE(speaking)`.
When that stops, it feeds back the last explicit state. This is needed because `state_update`
speaking does not arrive for relay turns (fake-speaker finding). It keeps idle-close and
rotation from firing while it talks, and it makes the button do `stop_speaking`.

## WebRTC choices (and why)

| Topic | Choice | Basis |
|---|---|---|
| Stack | `espressif/esp_peer` 1.5.7 from the registry, no `esp_webrtc` | esp_webrtc isn't published; peer_demo shows direct use |
| m-lines | audio (Opus, sendrecv) + application; **no video** | fake-speaker runs 02–04 connected without video |
| ICE | controlling, no STUN/TURN, host candidates only, UDP only | answer is ICE-lite with public host candidates; server learns our mapping as prflx |
| `ice_use_lite_mode` | on (`CONFIG_GLS_PEER_ICE_LITE_FLAG`) | same as openai_demo, which works against the same "realtimeapi" stack |
| DTLS | offer rewritten to `a=setup:active` (`CONFIG_GLS_OFFER_DTLS_ACTIVE`) | the DTLS role itself doesn't matter to the server. We want to be the SCTP client so esp_peer's local channel gets the even stream id 0 |
| Data channel | **stream 0 only, no DCEP-ACK wait** (`GLS_DC_MODE_STREAM0`, default). On SCTP up (as SCTP client) `esp_peer_create_data_channel()` registers stream 0 locally; its DCEP OPEN is ignored by the server. The channel counts as open immediately (or on the first stream-0 message). Every send uses stream 0 | Measured against /wm: the server only uses stream 0; it never ACKs DCEP there, but events flow ~77 ms after DTLS; odd streams are reset and **sending on one ABORTs the SCTP association**. esp_peer refuses sends on unknown streams ("Stream not found"), hence the local registration |
| Odd-stream guard | never create a channel as SCTP server (it would get an odd id); ignore open events and drop data on any stream ≠ 0; `rtc_peer_send_text` hard-codes stream 0 | see above |
| Fallbacks | `GLS_DC_MODE_RAW` (no local channel; sends probably fail with "Stream not found 0"), `GLS_DC_MODE_DCEP_ACK` (wait for an ACK that /wm never sends; diagnostics only) | |
| Opus | `audio_info` Opus 16 kHz mono (encode 16k mono, 24 kbps) | same as openai_demo. The SDP still says opus/48000/2 (RTP clock 48k). The answer's 48k/2 stream is decoded by av_render and resampled to 16k mono |
| Call "connected" | on data-channel open (`CONFIG_GLS_PEER_CONNECTED_ON_DTLS` = on DTLS up) | FSM definition |
| Candidate quirks | `ufrag` trailing token, `/` in ufrag, candidates only under first m-line | not verifiable offline: esp_peer's parser is closed source. openai_demo works with the same server family |

## Toolchain setup (macOS, Apple Silicon)

```sh
brew install cmake ninja dfu-util ccache
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.6 --recursive --shallow-submodules --depth 1 https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3          # tools go to ~/.espressif
git clone https://github.com/espressif/esp-webrtc-solution ~/esp/esp-webrtc-solution   # reference only
```

Every new shell: `. ~/esp/esp-idf/export.sh`. esp-webrtc-solution's CI builds with `release-v5.5`.

## AirPlay output

On the setup page: *Find speakers* → pick one → *Use selected for calls*, then *Measure* once
(a few beeps on that speaker). The measured receiver delay is stored and used to build the
echo-cancellation reference. Measure again after changing speakers or grouping them.
While AirPlay is active the XVF3800 uses double-talk 15 and echo over-subtraction 0.5/0.7,
and restores its defaults afterwards. `GET /api/xvf?r=<resid>&c=<cmd>&n=<count>&t=f|i|b[&v=...]`
reads (and, from the setup page, writes) any XVF3800 parameter for tuning.

## Configure, build, flash

```sh
cd firmware
idf.py set-target esp32s3            # once; uses sdkconfig.defaults (the source of truth)
idf.py menuconfig                    # GPT Live Speaker -> Wi-Fi and WireGuard relay
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor      # Ctrl-] quits
```

`sdkconfig` is git-ignored because it holds Wi-Fi and WireGuard credentials. Import the ChatGPT
session through the local setup page; no session needs to be compiled into the image.
The first build downloads ~30 managed components. If the XIAO doesn't enumerate: hold BOOT,
tap RESET, release BOOT.

Main options (menuconfig → GPT Live Speaker):

| Option | Default | What |
|---|---|---|
| `GLS_WIFI_SSID` / `_PASSWORD` | empty | until provisioning exists |
| `GLS_CHATGPT_DEV_SEED` | empty | dev only: session seed if NVS is empty (raw token or the setup page's JSON chunk map) |
| `GLS_RUN_PROBE` | y | reachability probe at boot |
| `GLS_AUTO_CALL_ON_BOOT` | n | start a call without pressing BOOT |
| `GLS_DC_MODE` | STREAM0 | stream 0, no ACK wait (see above) |
| `GLS_OFFER_DTLS_ACTIVE` | y | `a=setup:active` in the offer |
| `GLS_MIC_SLOT` | 0 | XVF3800 output slot used as mic |
| `GLS_DEFAULT_VOLUME` | 80 | digital volume |
| `GLS_IDLE_CLOSE_S` | 300 | FSM idle close |
| `GLS_LANGUAGE`, `GLS_VOICE`, `GLS_TIMEZONE`, `GLS_TZ_POSIX` | en-US, juniper, America/New_York | session JSON (set your own language and IANA/POSIX time zone) |

## Bring-up checklist (first boot on new hardware)

0. **XVF3800 firmware:** flash `respeaker_xvf3800_i2s_slave_v1.0.9_16k.bin` (bring-up guide, steps 1–5).
1. **Boot + probe:** set Wi-Fi only, flash, monitor. Expect:
   - `xvf_i2s: I2S0 master 16000 Hz ...` and `media ready`
   - Wi-Fi IP and SNTP time
   - probe `(a) ... HTTP 200 ... json` and `(b) ... HTTP 401 ... REACHED API (json)`
   - Blocked looks like `403 text/html` + `cf-mitigated`.
   - Note the heap lines (`internal free`, `psram free`).
2. **I2S sanity:** with no call running, the speaker should be silent, not buzzing (TX
   auto-clear). If the XVF3800 LEDs show no activity, check BCLK/WS on GPIO8/7 with a scope or
   logic analyser (1.024 MHz / 16 kHz).
3. **Auth:** open the station IP printed by `setup`, import the ChatGPT session, and expect
   `auth/session -> HTTP 200`, then authenticated idle. Reboot loads rotated cookies from NVS.
   See `web/README.md` for the API and controls.
4. **First call:** say Seneca, press BOOT, or use Start conversation on the setup page. The log prints the local offer
   (ice-pwd redacted), `POST /realtime/wm`, the remote answer, and peer states up to 7 (CONNECTED) / 9 (SCTP).
   - Then `DTLS/SCTP role: client`, `SCTP up: registered local channel for stream 0`,
     `data channel usable on stream 0 (SCTP up)`, and within ~100 ms `remote state idle -> listening`.
   - A press of BOOT during a call sends `stop_speaking` on stream 0. Check that `send on stream 0 failed` does **not** appear.
   - If esp_peer logs `Stream not found 0` or the send fails: esp_peer gates sends on its own
     channel state. Note the log and see "Risk: esp_peer data channel" below. Do not switch to an odd stream.
   - If events never arrive: try `GLS_DC_MODE_RAW` (receive-only test).
   - If ICE never pairs (states stuck at 3/4): toggle `GLS_PEER_ICE_LITE_FLAG`.
   - If it connects but the FSM times out in CONNECTING: try `GLS_PEER_CONNECTED_ON_DTLS=y`, and
     tell us what the data channel did.
5. **Audio down:** say "Hallo". The log shows `remote audio: codec 3, ... Hz` and `gpt: ...`
   captions, and audio comes out of the speaker. If it's distorted or too fast/slow, check the
   reported sample rate and the av_render resample path.
6. **Audio up:** `you: ...` transcripts appear. If they're silent, try `GLS_MIC_SLOT=1`.
7. **Barge-in / AEC:** talk over it. The XVF3800 should cancel the speaker. Watch `AEC_AECCONVERGED`
   over I2C (bring-up step 7). A short BOOT press while it talks sends `stop_speaking`.
8. **Lifecycle:** idle 5 min → hang up (`hang up (reason 2)`). Long-press → hang up. Watch for
   `usage:` lines (session left), and for rotation near the 2 h cap.

## Data-channel validation

The hardware call used the registered stream 0 without waiting for a DCEP ACK. User transcripts,
assistant text, and stop_speaking flowed. The server also opens stream 2, which this firmware
ignores. Longer-call lifecycle and network-drop tests remain.

## Not done yet / stubs

- **Wi-Fi provisioning:** Wi-Fi is set at build time; AP/captive portal remain.
- The 48 kHz XVF3800 master-firmware fallback (ESP32 as I2S slave): not implemented
  (`#error` guard in xvf3800_i2s.c).
- OTA, NVS encryption (`nvs_keys` partition is reserved), remote config `policy` → `sc_fsm_config_t`.
- An expired session currently shows "Reconnecting" instead of asking to sign in again.

## Partition table (8 MB)

`nvs` 64 KB · `otadata` · `phy_init` · `nvs_keys` · `ota_0` 3.5 MB · `ota_1` 3.5 MB · `storage` 896 KB.
App rollback is enabled.

## Dependencies (components.espressif.com, `main/idf_component.yml`)

`espressif/esp_peer` ~1.5, `espressif/esp_capture` ~1.1, `tempotian/av_render` ~1.2,
`espressif/esp_audio_codec` ~2.6, `espressif/esp_codec_dev` ^2.0.0-beta5. They pull ~30 more,
including esp-sr/esp-dl via esp_capture's AEC source. Those are compiled but not linked; only
Opus enc/dec, esp_peer, capture and render end up in the image. Thread stacks for Opus/esp_peer are in
PSRAM (`CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`). Seneca adds pinned
`espressif/esp-tflite-micro` 1.3.3~1 and `esphome/esp-micro-speech-features` 1.2.3. No IDF patches are needed.
