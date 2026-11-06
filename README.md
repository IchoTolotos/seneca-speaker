# Seneca – a DIY ChatGPT voice speaker

<p align="center"><img src="docs/images/enclosure.png" width="420" alt="Seneca speaker enclosure"></p>

A small smart speaker that talks to **ChatGPT's voice mode** in full duplex. Say "Seneca", talk,
interrupt it whenever you like. It runs on a **Seeed ReSpeaker XVF3800 (4-mic array with hardware
echo cancellation) + XIAO ESP32-S3**. Audio goes straight from the device to ChatGPT over WebRTC.
It uses your ChatGPT subscription, so there are no per-minute API costs.

```
             ┌──────────── WebRTC (Opus audio + data channel) ───────────┐
             │                                                            ▼
 ReSpeaker XVF3800 + XIAO ESP32-S3                          ChatGPT voice (/realtime/wm)
 wake word · AEC · Opus · setup page                                     ▲
             │                                                            │
             └── WireGuard (sign-in + SDP over HTTP) ──► home relay ──────┘ (HTTPS)
             │
             └── AirPlay 2 (optional) ──► HomePod / AirPlay speaker
```

> ⚠️ **Unofficial, and a risk to your account.** This uses `chatgpt.com/realtime/wm`, the private
> endpoint behind ChatGPT's web voice mode. It is undocumented, can change or break at any time,
> and is not affiliated with or endorsed by OpenAI.
>
> The speaker signs in with your real ChatGPT web session, so OpenAI sees it as your account. Using
> a private endpoint from a non-browser client may violate OpenAI's terms. It could get the session
> revoked or your account restricted or banned.
>
> Use your own account, for personal use only, and only if you accept that risk.

## Features

- **Hands-free, full-duplex conversation.** On-device wake word "Seneca" (microWakeWord, TFLite
  Micro). You can interrupt it mid-sentence, and "Seneca, stop" ends the call.
- **Hardware echo cancellation.** The XVF3800 removes the speaker's own voice from the mics, so the
  assistant does not hear itself.
- **No server costs.** The ESP32 holds the ChatGPT web session and does sign-in and WebRTC
  itself. A tiny relay on an always-on machine at home forwards only the sign-in/SDP HTTP requests,
  because chatgpt.com challenges the ESP32's TLS fingerprint. Voice audio never touches the relay.
- **AirPlay 2 output (optional).** Plays the conversation on a HomePod or other AirPlay speaker,
  during calls and with barge-in. The echo canceller still works: the device feeds the XVF3800 a
  sample-exact copy of what the AirPlay speaker is playing, and measures that speaker's delay
  acoustically.
- **Local setup page.** Sign in, start/stop calls, mute, set volume, voice boost and EQ, pick an
  AirPlay speaker, test the wake word, and view microphone/speaker diagnostics.
- **Audio robustness.** Adaptive jitter buffer with pause stretching for bursty network delivery,
  plus a soft limiter.
- **3D-printable enclosure.** Parametric CadQuery model. Every assembly step is collision-checked
  in the model.

## Hardware

| Part | Notes |
|---|---|
| Seeed **reSpeaker XVF3800 4-mic array with XIAO ESP32-S3** | XVF3800 runs the I2S 16 kHz firmware |
| Seeed [**Mono Enclosed Speaker 4R 5W**](https://www.seeedstudio.com/Mono-Enclosed-Speaker-4R-5W-p-5931.html) (SKU 114993346) | 49.6 × 44.6 × 22 mm box with two screw tabs; on the board's JST speaker connector. The enclosure is designed for exactly this speaker (a powered speaker on the 3.5 mm jack also works) |
| USB-C power supply | |
| Optional: HomePod / AirPlay 2 speaker | |
| An always-on computer at home | **required**, runs the relay (see below). macOS scripts are included; any machine that runs Python and WireGuard works (Linux box, Raspberry Pi, …) |
| Enclosure hardware | heat-set inserts, M2/M3 screws, 3 mm EPDM foam; see [enclosure/](enclosure/README.md) |

### Rough cost

| Part | approx. |
|---|---|
| [reSpeaker XVF3800 with XIAO ESP32-S3](https://www.seeedstudio.com/ReSpeaker-XVF3800-4-Mic-Array-With-XIAO-ESP32S3-p-6489.html) | $67 |
| [Mono Enclosed Speaker 4R 5W](https://www.seeedstudio.com/Mono-Enclosed-Speaker-4R-5W-p-5931.html) | $2 |
| USB-C power supply | $10 |
| Heat-set inserts, screws, EPDM foam, rubber feet | $10 |
| Filament (~60 g, two colours) | $2 |
| **Total** | **≈ $90** |

Not included: the relay computer (one you already have, or a Raspberry Pi) and an optional AirPlay speaker.
Prices are from late 2026, without shipping.

## The home relay (required)

chatgpt.com's bot protection challenges the ESP32's TLS fingerprint, so the device can't call the
sign-in and session-setup endpoints directly. A small relay on a computer at home makes those
calls instead:
- The speaker reaches the relay through **WireGuard**. Only one UDP port is forwarded on your
  router, and the relay is not exposed otherwise.
- The relay forwards **only** the few allow-listed sign-in/SDP requests, guarded by a private URL
  token.
- The voice audio itself does **not** go through the relay: WebRTC runs directly between the
  speaker and ChatGPT.

See [relay/README.md](relay/README.md) for setup.

Since a computer at home is part of the setup anyway, it can do more than relay. It can also run
**Codex** sessions, which count against your ChatGPT plan's limits rather than API billing. That
makes the speaker a possible voice front-end for getting real work done on that computer. This
bridge is not built yet; the relay currently only handles sign-in and session setup.

## Signing in

The speaker uses your ChatGPT **web session** (the same login as chatgpt.com in a browser). You
import it once on the speaker's setup page. It is stored on the device, refreshed automatically
and never shown again.

1. Find the speaker's address. The boot log prints `sign in at http://<ip>/`, or check your
   router's client list. Open it from a device on the same Wi-Fi.
2. On a Mac, sign in at **chatgpt.com in Safari**. Enable *Settings → Advanced → Show features for
   web developers* once.
3. Open **Develop → Show Web Inspector → Storage → Cookies → chatgpt.com**.
4. Copy the value of `__Secure-next-auth.session-token.0` into the first field (*Session cookie .0*).
   Copy `__Secure-next-auth.session-token.1` into the second field, if it exists; long sessions are
   split into two cookies. If there is only an unnumbered `__Secure-next-auth.session-token`, put
   it into the first field and leave the second empty.
   - Chrome/Firefox work too: DevTools → Application/Storage → Cookies → chatgpt.com.
5. Press **Save and sign in**. The status turns to *signed in* (with your account email) within a few seconds.
   Say **"Seneca"**.

Logging out of chatgpt.com in that browser can end the session for the speaker too. To be safe,
import the cookie from a separate browser profile that stays signed in. *Sign out* on the setup
page deletes the session from the device.

## How the AirPlay echo cancellation works

Echo cancellation needs a **reference**: the exact signal coming out of the loudspeaker, in time
with the sound in the room. The XVF3800 gets it on its I2S input, the same line that normally
drives the built-in speaker. That is easy when the speaker is on the board. An AirPlay speaker,
though, plays the audio about half a second later, and the delay depends on the network, the
sender's queue and the receiver.

The firmware rebuilds the reference exactly:

1. **Timestamped send history.** AirPlay stamps every packet with an RTP time derived from the
   sender's clock, and the receiver plays it at *RTP time + a fixed playout delay*. The sender keeps
   a 3-second history of everything it sent, indexed by RTP time. Gaps, catch-ups after Wi-Fi
   stalls, and flushes on barge-in are recorded exactly as they happened.
2. **Exact output timing.** An I2S DMA callback timestamps every buffer, so the firmware knows the
   moment each sample written to the XVF3800 leaves the I2S line.
3. **Measured receiver delay.** A one-time measurement uses the board's own microphones. It plays
   short beeps on the AirPlay speaker and times them against their RTP timestamps. That gives the
   receiver's playout delay, e.g. 567 ms for a HomePod mini.
4. **Lookup.** For every sample going to the XVF3800, the firmware takes its output time, converts
   it to RTP time, subtracts the measured delay and reads the history. The reference is the sample
   the AirPlay speaker is playing at that instant, shifted 40 ms early so the echo lands well
   inside the XVF3800's 192 ms echo filter.

Meanwhile the board's own amplifier is switched off, so the I2S line only feeds the echo
canceller.

Because the reference is sample-exact, the linear echo canceller stays converged for whole calls.
The firmware can then relax the XVF3800's residual echo suppression during AirPlay calls (double-talk 15,
over-subtraction 0.5/0.7). That lets you interrupt at normal speaking volume instead of having
to shout over the assistant.

An earlier version estimated the delay from queue fill levels. That estimate shifted by 60–150 ms
at every turn, the canceller kept losing track, and the suppressor then muted the user as well.

## Repository layout

| Path | What |
|---|---|
| [`firmware/`](firmware/README.md) | ESP-IDF 5.5 firmware: wake word, WebRTC, ChatGPT sign-in, setup page, AirPlay sender, XVF3800 control |
| [`firmware/components/speaker_core/`](firmware/components/speaker_core/README.md) | Portable, host-tested core: session state machine, data-channel events, cookie handling, request building |
| [`relay/`](relay/README.md) | Home relay: userspace WireGuard endpoint (wireproxy) + a minimal allow-listed HTTP relay + UPnP port mapping |
| [`wakeword/`](wakeword/seneca/README.md) | The "Seneca" wake-word model and its reproducible training recipe |
| [`enclosure/`](enclosure/README.md) | Parametric CadQuery enclosure, assembly checks and a browser 3D viewer |
| [`docs/`](docs/) | Hardware bring-up guide, board annotations, protocol research notes |
| [`signaling/`](signaling/) | Browser/Node prototype used to reverse-engineer the protocol (not needed for the device) |
| [`tools/fake-speaker/`](tools/fake-speaker/README.md) | Headless Python stand-in for the device, used to learn the protocol before the hardware existed |
| [`config/`](config/README.md) | Optional remote device config (for adapting to web-client changes without reflashing) |

## Getting started

1. **Flash the XVF3800** with Seeed's I2S firmware (16 kHz). See
   [docs/hardware-bringup.md](docs/hardware-bringup.md).
2. **Set up the relay** on an always-on machine at home, and forward UDP 51820 to it (or let the
   UPnP script do that). See [relay/README.md](relay/README.md).
3. **Build and flash the firmware.** Set your Wi-Fi and WireGuard settings in `idf.py menuconfig`.
   They stay in the git-ignored `sdkconfig`. See [firmware/README.md](firmware/README.md).
4. **Sign in** on the setup page (see [Signing in](#signing-in)) and say "Seneca".
5. **Optional: AirPlay.** On the setup page, press *Find AirPlay speakers*, select one, press *Use selected for calls*, then
   *Measure latency & stop time* once.

## Status

Working daily on real hardware: sign-in survives reboots, wake word → call, barge-in, voice
hang-up, built-in speaker and AirPlay output with echo cancellation.

Known gaps:
- **Wi-Fi is set at build time.** There is no setup hotspot or captive portal yet, so changing
  networks means reflashing.
- **Expired sessions aren't detected.** If the ChatGPT session expires or is revoked, the device
  keeps retrying ("Reconnecting") instead of asking you to sign in again. Import a fresh cookie.
- **No OTA updates.** Firmware updates go over USB.
- **No NVS encryption yet.** The session is stored unencrypted in flash (the partition is
  reserved).
- **Remote config is not wired up.** `config/device-config.json` is documented but not fetched yet.
- **XVF3800 48 kHz fallback** (ESP32 as I2S slave) is not implemented; the 16 kHz firmware is
  required.
- **Wake word is English only.** "SEN-e-ka" was tested on synthetic voices plus real use; the
  false-wake rate in real homes isn't measured systematically.

## License

MIT, see [LICENSE](LICENSE). Third-party code keeps its own licenses:
- AirPlay sender from [AirPlayKnob](https://github.com/amcchord/AirPlayKnob) (MIT,
  `firmware/components/airplay_tx/vendor/`).
- [login-with-chatgpt](https://github.com/opencoredev/login-with-chatgpt) core (MIT,
  `signaling/vendor/`).
- The wake-word model was trained on third-party voices and datasets with their own terms; see
  [wakeword/seneca/README.md](wakeword/seneca/README.md#training-data-and-credits).
