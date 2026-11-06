# Speaker sign-in and controls

`setup.html` is embedded in the firmware and served by `main/setup.c` at `/` and `/setup`.
Open `http://<station-ip>/` on the same Wi-Fi. The boot log prints the address. Wi-Fi credentials
are still configured through ESP-IDF; captive portal, AP provisioning, mDNS, and voice editing
are not implemented.

The page imports a Safari ChatGPT session into the device's A/B NVS store. It clears both input
fields after a successful save. Cookies and access tokens are never returned by the status API.
A successful save means persistence succeeded; subsequent status polling reports authentication.
Logout clears the stored session and stops the call asynchronously.

Every API request requires `X-Speaker-Setup: 1`. POST requests require `application/json`.
The Host must match the station IP (optionally `:80`), and Origin, when present, must match
`http://<Host>`. There are no CORS headers or permissive OPTIONS responses. Request bodies are
limited to 24 KB and read in a loop. Responses use `Cache-Control: no-store`.

| Route | Request / response |
| --- | --- |
| `GET /api/status` | Wi-Fi SSID/connection/RSSI, firmware, FSM state, credential presence, authenticated email/expiry, error, mute, tunnel, Seneca readiness/detections, uptime |
| `POST /api/setup` | `{"session":{"chunk0":"<cookie .0>","chunk1":"<cookie .1>"}}`; `chunk1` can be null for an unnumbered cookie. A cookie-name JSON map in `chunk0` is also accepted by the existing parser. |
| `POST /api/control` | `{"action":"wake"}`, `{"action":"hangup"}`, or `{"action":"mute"}` (toggle) |
| `POST /api/logout` | `{}` |

Successful POST responses are `{"ok":true}`; errors include `{"ok":false,"message":"..."}`.
Diagnostic status fields `mic_frames`, `mic_peak`, and `wake_score` report sampling progress,
latest 10 ms PCM peak, and rolling wake confidence percent. `barge_ins` counts locally detected
speech interruptions posted to the controller. They contain no recorded audio.

Say “Seneca” (English SEN-e-ka) to start a conversation. In a call, another wake interrupts
assistant speech. Speaking over a reply also triggers local interruption (0.045 PCM RMS,
120 ms hold, 800 ms cooldown), sends `stop_speaking`, clears queued playback, and silences
late downlink audio for 350 ms while the microphone uplink continues.

Only **“Seneca, stop”** ends a call through the device's voice-command handler. Case and
punctuation are ignored; “Seneka”/“stopp” are accepted as ASR spellings of that same phrase.
Other phrases, including “auflegen”, “hang up”, “please end the call”, and polite variants,
are rejected. Assistant/tool text cannot issue voice commands. This uses server transcription,
not an offline stop model. Whole and split user-transcript events are handled; a standalone
Seneca fragment can prefix a stop fragment for three seconds. A 500 ms settling interval lets
later words cancel a partial command that belongs to a longer sentence.

The status API now includes `call_active`, `call_connected`, `remote_state`, last hang-up
reason/time, and last user transcript/time (RAM only). The page distinguishes connected calls,
paused calls, teardown, and idle. A network failure marks status unknown instead of retaining
an old in-call label. Status polling is bounded by a timeout, does not overlap periodic polls,
and ignores obsolete responses after newer actions/status requests.

Hang up through the page or hold BOOT for 1–4 seconds and release for a physical fallback. Holding
BOOT for at least 4 seconds toggles mute. Calls close after 300 seconds without activity;
speaking, thinking and background research keep them active. Mute suppresses microphone audio
and wake detections, but does not itself end a call. Mute is saved in NVS and restored at boot;
a fresh setup starts muted until explicitly unmuted through the page or button. Between calls, wake detection runs locally
and the voice encoder/uplink is stopped.

Verified on the ReSpeaker/XIAO on 2026-10-28: same-origin checks, invalid request rejection,
private credential import, reboot persistence, authenticated refresh/rotation, microphone-driven
Seneca detection, a WebRTC call with user transcript and assistant response, mute and hang-up.
There is no built-in loudspeaker; audible reply output and AEC quality still need testing with
headphones or an attached speaker. Do not run acoustic playback tests on the Mac without the
user explicitly asking for playback.

The 2026-10-28 interruption/hang-up update was validated with silent generated PCM and transcript
unit tests (nine ASan/UBSan host suites), compiled, and flashed. Real acoustic interruption,
AEC/double-talk behavior, and spoken hang-up recognition still need a user-led test. No audio
playback or new voice call is started by this validation.
