# fake-speaker

A headless Python stand-in for the ESP32-S3 speaker firmware. It speaks the minimum protocol
the device will speak to ChatGPT GPT-Live, so we can learn it before the hardware arrives:

1. `GET https://chatgpt.com/api/auth/session` over **HTTP/1.1** with the stored web-session
   cookie → web access token. The rotated cookie from `Set-Cookie` is written back.
2. `POST https://chatgpt.com/realtime/wm?dcid=0` over **HTTP/1.1**, `multipart/form-data` with
   `sdp` (the offer) and `session` (JSON) → `201` + SDP answer.
3. WebRTC with [aiortc](https://github.com/aiortc/aiortc): Opus audio both ways, an optional
   inert `sendonly` video m-line, and an SCTP data channel negotiated with id 0, max-bundle.

HTTP is plain `http.client` with ALPN `http/1.1` (Cloudflare challenges HTTP/2 clients like curl
but lets HTTP/1.1 through, and the ESP32 will use HTTP/1.1).

## Setup

```sh
cd tools/fake-speaker
python3.12 -m venv .venv
.venv/bin/pip install -r requirements.txt
```
You also need Homebrew `opus` and `ffmpeg` (used by `av`/aiortc) and a saved web session:
start `signaling/` (`npm start`) and paste the cookie once on its page (see `docs/phase0.md`).

### Credentials
- Reads `~/.gpt-live-speaker/state.json` → key `web_session` (AES-256-GCM, `base64(iv).base64(ct+tag)`,
  same as `signaling/src/crypto.ts`), with `ENCRYPTION_KEY` from `signaling/.dev.vars`
  (or the `ENCRYPTION_KEY` env var; `STATE_FILE` env var overrides the path).
- After every `auth/session` call it re-encrypts the rotated `sessionCookies` + `deviceId` into
  `web_session` only (read-modify-write, tmp file + rename). Nothing else is touched.
- Never prints the cookie or the access token. Captures contain no secrets (our ICE password
  and our IP addresses are redacted; server fingerprints/ufrags are kept).

## Usage

```sh
.venv/bin/python fake_speaker.py [options]

  --relay "text"       send a typed relay_message after the data channel opens
  --duration N         call length in seconds (default 30)
  --no-video           omit the inert sendonly video m-line
  --mic / --no-mic     send the Mac default mic (avfoundation) or silence (default: silence)
  --mic-device IDX     avfoundation audio device index
  --play               play received audio on the Mac speakers (sounddevice)
  --record out.wav     save received audio (48 kHz mono s16)
  --log events.jsonl   every data-channel event in/out with timestamps
  --language de-DE     session language_code (default de-DE)
  --voice juniper      voice (default juniper)
  --tag NAME           file prefix for captures/ (offer/answer SDP, session JSON)
  --dtls-setup MODE    rewrite a=setup in the offer: actpass (default) | active | passive
  --dc-mode MODE       negotiated (default; id 0, like the browser) | dcep (in-band open, like esp_peer)
  --dc-id N            force our SCTP stream id (dcep mode: aiortc would otherwise pick 1)
  --dc-label TEXT      data channel label (sent in the DCEP OPEN)
  --dcep-ack-timeout S dcep mode: wait S seconds for DATA_CHANNEL_ACK, then send anyway (default 3)
  --auth-only          only do the auth/session exchange (and cookie rotation)
  -v / --verbose       print the streaming transcript
  --debug              aiortc/aioice debug logging (prints raw SDP incl. ice-pwd; don't share)
```

Examples:
```sh
# baseline: video line on, silence, typed message, record
.venv/bin/python fake_speaker.py --relay "Sag in einem Satz hallo." --duration 25 \
  --record captures/01-baseline.wav --log captures/01-baseline-events.jsonl --tag 01-baseline -v

# talk to it with the Mac mic + speakers (use headphones: no echo cancellation here)
.venv/bin/python fake_speaker.py --mic --play --no-video --duration 60 -v
```

Output: timings since start (auth, signaling, DTLS connected, data channel open, first
`state_update`, first audio frame, first audible frame), state transitions, transcript, and
event-type counts.

## Files
- `fake_speaker.py` – the client.
- `captures/` – SDP offers/answers (redacted), session JSON, event logs (`*-events.jsonl`), and
  recordings (`*.wav`, gitignored).

## Findings (2026-10-24, home connection, Berlin)

### It works, headless, from Python over HTTP/1.1
4 successful calls (baseline with video, without video, web research, forced DTLS role).
Each got `201` with an SDP answer, ICE + DTLS + SCTP came up, `state_update` arrived,
assistant audio was audible in the recordings and the transcript streamed.

| run | auth/session | /realtime/wm | DTLS connected | DC open = 1st state_update | 1st audible audio after relay |
|---|---|---|---|---|---|
| 01 baseline (video m-line) | 1384 ms | 1220 ms | 3939 ms | 4744 ms | 1474 ms |
| 02 `--no-video` | 2086 ms | 2177 ms | 6048 ms | 7107 ms | 1928 ms |
| 03 research (no video) | 1489 ms | 1526 ms | 4519 ms | 5650 ms | 1721 ms |
| 04 `--dtls-setup active` (no video) | 1452 ms | 1535 ms | 5088 ms | 6136 ms | 1460 ms |

(columns 4–5 are ms since program start.) Outliers seen in earlier attempts: auth/session up to
9.2 s, `/realtime/wm` up to 4.9 s. After the answer: ICE ≈ 0.9–1.2 s, DTLS ≈ +0.5 s, SCTP/data
channel ≈ +0.8–1.1 s. The first `state_update` (`idle → listening`) is the very first data-channel
message, together with `session_bootstrap`, `startup_telemetry` and `thinking_update`
(`backend_model: gpt-6-thinking`).

### The video m-line is NOT required
`--no-video` (audio + application only, `BUNDLE 0 1`) connected and answered normally (run 02–04).
The answer simply mirrors the offered m-lines. With video offered, the answer contains
`m=video … H264/90000 … a=recvonly` (only H264 profile 42e01f).

### SDP facts for the firmware (esp_peer)
- **Answer is ICE-lite** (`a=ice-lite`), so the device is the controlling agent and must nominate.
- **Server candidates** (the same 4 IPs in all 4 answers; only the ufrag changes):
  4 × UDP **port 3478** `typ host` + 2 × **TCP port 443** `typ host tcptype passive`.
  No srflx/relay, no TURN offered. Each candidate line carries a non-standard trailing
  `ufrag <ufrag>` extension (parsers must tolerate unknown trailing tokens).
- **Candidates appear only under the first m-line** (audio); the data-channel m-line has none, and
  there is no `a=end-of-candidates`. With BUNDLE that's fine for real stacks; aiortc needed a
  workaround (`fix_answer_for_aiortc`). The device needs no STUN/TURN: host candidates on our
  side were enough (server learns our NAT mapping as peer-reflexive).
- ICE ufrag has `/` in it (e.g. `tp3DmT/u190/3oNDO7`), ~18 chars.
- **DTLS role doesn't matter**: offer `actpass` → answer `a=setup:active` (server = DTLS client,
  device = DTLS server). Offer `active` → answer `a=setup:passive`, also works (run 04).
  Answer fingerprint is `sha-256`, session-level.
- **Audio**: answer `a=rtpmap:96 opus/48000/2`, `a=fmtp:96 minptime=10;useinbandfec=1`, plus
  PCMU/PCMA (0, 8) as fallbacks. No `stereo=1`, no `ptime`. Received audio is effectively
  **mono** (decoded L == R in every frame) in **20 ms packets** (960 samples @ 48 kHz).
  We sent Opus 48 kHz with 20 ms frames; aiortc's offer had no Opus fmtp at all and that was
  accepted. G.722 in the offer was dropped by the answer.
- Answer uses `a=rtcp-mux`, `a=rtcp-rsize`; audio m-line answers with **no extmap** (our
  `sdes:mid` and `ssrc-audio-level` were dropped); server SSRC has `cname:realtimeapi`,
  `msid:realtimeapi audio`. Data channel: `UDP/DTLS/SCTP webrtc-datachannel`,
  `a=sctp-port:5000`, `a=max-message-size:1073741823`.
- **Server RTP is discontinuous**: packets pause when nothing is said (gaps up to ~3 s observed);
  the jitter buffer/player must tolerate gaps (no comfort-noise stream).
- Offer that worked (aiortc): `BUNDLE 0 [1] 2`, `a=setup:actpass`, `a=rtcp-mux`, audio
  `sendrecv` PT 96 opus/48000/2 (+9 G722, 0 PCMU, 8 PCMA), extmaps `sdes:mid` +
  `ssrc-audio-level`, host UDP candidates only, `a=sctp-port:5000`, `max-message-size:65536`.
  See `captures/0*-offer.sdp` / `-answer.sdp`.

### Data channel protocol (observed)
- All messages are `{"type":"data_message","data":"<json string>"}` both ways.
- First messages: `state_update` (`previous_state: idle`, `new_state: listening`),
  `session_bootstrap`, `startup_telemetry` (has `conversation_id`), `thinking_update`.
- A typed `relay_message` → `chat_message_delta` (user message echo), `relay_message_processed`,
  `spawn_update` (`kind: commentary`, `state: start` … `end`), assistant deltas, `conversation_update`.
- **Relay-triggered answers did not produce `state_update` → speaking** (only the initial
  `listening` arrived in every run). Don't rely on `state_update` alone to know the model is
  talking; audio packets and `chat_message_delta` are better signals for relay turns.
- `chat_message_delta.payload.delta` is a compact JSON-patch stream: `c` = message slot,
  `o` = op (`add`/`patch`/`append`/`replace`/`remove`), `p` = JSON pointer, `v` = value; `c` and
  `o` are **sticky** (omitted = same as previous). New message:
  `{"c":1,"o":"add","p":"","v":{"message":{...}}}`; spoken text grows via
  `{"p":"/message/content/parts/0/text","o":"append","v":", schön"}`. Spoken assistant text is
  `parts[0] = {"content_type":"audio_transcription","text":…,"direction":"out"}`; user text is a
  plain string. `Transcript` in `fake_speaker.py` is a working decoder (~60 lines) to port.
- **Web research works headless** (run 03, "Was ist heute die wichtigste Nachricht in Berlin?"):
  the model speaks a preamble ("Ich check das kurz.") ~1.7 s after the relay, then 3 messages with
  `author: {role: "tool", name: "web.run"}`, a `thoughts` message "Searched 21 websites",
  `reasoning_recap` "Worked for 14s", 19 `url_moderation` events (image/article URLs), then the
  spoken answer (~16.7 s after the relay) and a written `text` message with a `genui{news_article_widget…}`
  block. `spawn_update` start→end bracketed the work (17.7 s). Captured in `captures/03-research-events.jsonl`.
- `usage_update`: audio ≈ 53 878 s remaining, session cap ≈ 7 172 s (≈ 2 h), matching Safari.

### Auth / cookies
- `auth/session` over HTTP/1.1 from Python works (no Cloudflare challenge). `Set-Cookie` rotated
  both chunks on **every** call; the returned access token's expiry did not move between calls
  (~180 h left), i.e. the same token is handed back until it nears expiry.
- **Old cookie chunks keep working after rotation**: the running Node server (which still had the
  pre-rotation cookie cached in memory) refreshed successfully afterwards. So a lost write-back is
  not fatal, but the device should still persist the rotated chunks.
- Caveat: the Node server keeps `state.json` cached in memory and rewrites the whole file on its
  next write, which overwrites the `web_session` written by fake-speaker with its own (also valid)
  value. Harmless given the previous point.

### Data channel without pre-negotiation (esp_peer / DCEP), 2026-10-24 follow-up
esp_peer can't create a negotiated channel; it opens one in-band with DCEP (RFC 8832). Tested with
`--dc-mode dcep` (audio-only offer). Result: **ChatGPT's `/wm` only uses SCTP stream 0, as a
pre-negotiated channel. It doesn't need DCEP and never acknowledges a DCEP open on stream 0.**

| run | offer `a=setup` (answer) | our channel | DCEP ACK? | events from server | relay answered? |
|---|---|---|---|---|---|
| 04 (earlier) | `active` (`passive`) | negotiated, id 0 | n/a | yes | **yes** |
| 05 (A) | `active` (`passive`) | DCEP OPEN on **stream 0**, label `""` | **no** (none in 20 s) | **yes**: stream 0 PPID 51 from +77 ms after DTLS (`state_update`, `session_bootstrap`, …) | **yes**: sent on stream 0 without the ACK, answered with audio and transcript |
| 06 (B) | `actpass` (`active`) | DCEP OPEN on **stream 1** (odd id; aiortc's default), label `""` and `oai-events` | **yes**, ACK on stream 1 | only on stream 0, which our side never opened | **no**: the server **reset stream 1** about 40 ms after the ACK (SCTP RE-CONFIG outgoing-stream reset), and data sent on it later got the **SCTP association aborted** by the server |

- The server pushes its events on **stream 0, PPID 51 (string)** right after SCTP comes up,
  whether or not we sent anything. It expects our messages on stream 0 too.
- Any other stream is accepted with DCEP and then immediately reset. Writing to it kills the
  whole SCTP association.
- **The DTLS role doesn't matter; the stream id does.** In run 05 the device was the DTLS client
  (`setup:active`).
- **Firmware recipe for esp_peer:**
  - Offer `a=setup:active` (or `actpass`).
  - Make sure the channel lands on SCTP stream 0. As DTLS client, RFC 8832 even-id allocation
    starts at 0.
  - Do **not wait for a DATA_CHANNEL_ACK**: treat the channel as open as soon as SCTP is up or
    the first stream-0 message arrives, then send on stream 0 with PPID 51.
  - If esp_peer can only report "open" after the ACK, patch it (skip the ACK wait, or use a
    "negotiated id 0" mode). Never use an odd stream id.
  - Sending the DCEP OPEN on stream 0 is harmless; the server ignores it.
- aiortc side note: it picks the DCEP stream parity from the **ICE** role (controlling → odd), not
  the DTLS role. Against an ICE-lite server it's always controlling, so run 05 forced `--dc-id 0`.
  The script waits `--dcep-ack-timeout` (3 s) for an ACK and then sends anyway.

### Opus clock rate / channels (D)
- aiortc's offer is always `a=rtpmap:96 opus/48000/2` with no fmtp. Its encoder is fixed at
  48 kHz, 20 ms (960 samples), `application=voip`, and resamples any input. We could not test
  sending audio at 16 kHz bandwidth with aiortc.
- That's fine for esp_peer: RFC 7587 requires the rtpmap to be `opus/48000/2` whatever the
  internal sample rate or channel count. An ESP32 encoding 16 kHz mono Opus still advertises
  `opus/48000/2`.
- The server's answer has no `maxplaybackrate`, `sprop-maxcapturerate` or `stereo`
  (`minptime=10;useinbandfec=1` only), and its decoder must handle any Opus bandwidth. So 16 kHz
  mono content should be accepted, but this is **untested**.
- Downstream audio is 48 kHz, mono content, 20 ms packets. The device can decode it at 16 kHz,
  since Opus decoders can output any supported rate.

### aiortc quirks we hit (not protocol requirements)
1. With `bundlePolicy=max-bundle`, creating the data channel **before** the first transceiver makes
   aiortc stop the shared ICE transport when the answer arrives (ICE stays `new` → `closed`).
   Fix: add tracks/transceivers first, then `createDataChannel`. The SDP is the same either way.
2. aiortc keys remote candidates per transport and lets the last bundled m-line win; since only
   the audio m-line has candidates, it ended with zero remote candidates and ICE sat in
   `checking`. Fix: copy the candidates into every m-section of the answer before applying it.
3. aioice does not do ICE-TCP, so only the UDP 3478 candidates were used. Whether TCP 443 works
   (useful on networks that block UDP) is untested.
