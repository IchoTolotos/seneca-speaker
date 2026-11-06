#!/usr/bin/env python3
"""Fake speaker: a headless stand-in for the ESP32-S3 firmware talking to ChatGPT GPT-Live.

Does exactly what the device will do:
  1. GET  https://chatgpt.com/api/auth/session   (HTTP/1.1, session cookie)  -> access token
  2. POST https://chatgpt.com/realtime/wm?dcid=0 (HTTP/1.1, multipart sdp+session) -> SDP answer
  3. WebRTC (aiortc): Opus audio both ways + data channel negotiated id 0.

Secrets (session cookie, access token) are never printed or logged. The rotated session
cookie is written back, encrypted, into ~/.gpt-live-speaker/state.json (`web_session` only),
using the same AES-256-GCM envelope as signaling/src/crypto.ts.
"""
from __future__ import annotations

import argparse
import asyncio
import base64
import datetime as dt
import http.client
import json
import math
import os
import re
import secrets
import ssl
import sys
import time
import uuid
import wave
from collections import Counter
from fractions import Fraction
from pathlib import Path

import numpy as np
from aiortc import (
    MediaStreamTrack,
    RTCConfiguration,
    RTCPeerConnection,
    RTCSessionDescription,
)
from aiortc.rtcconfiguration import RTCBundlePolicy
from av import AudioFrame
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
DEV_VARS = REPO / "signaling" / ".dev.vars"
STATE_FILE = Path(os.environ.get("STATE_FILE", Path.home() / ".gpt-live-speaker" / "state.json"))

HOST = "chatgpt.com"
BASE = f"https://{HOST}"
WM_PATH = "/realtime/wm"
CLIENT_VERSION = "prod-e8f67765f24e1c6773b4b61b4310d5ff9297439a"
CLIENT_BUILD = "8338682"
WEB_CLIENT_ID = "app_X8zY6vW2pQ9tR3dE7nK1jL5gH"
BROWSER_UA = (
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko) Version/18.0 Safari/605.1.15"
)
COOKIE_NAME = "__Secure-next-auth.session-token"

T0 = time.perf_counter()


def ms(t: float | None = None) -> int:
    return round(((t if t is not None else time.perf_counter()) - T0) * 1000)


def say(msg: str) -> None:
    print(f"[{ms():>6} ms] {msg}", flush=True)


# --------------------------------------------------------------------------- secrets store

def load_dev_vars() -> dict[str, str]:
    out: dict[str, str] = {}
    for line in DEV_VARS.read_text().splitlines():
        m = re.match(r"^\s*([A-Z0-9_]+)\s*=\s*(.*?)\s*$", line)
        if m:
            out[m.group(1)] = re.sub(r'^"(.*)"$', r"\1", m.group(2))
    return out


def _key() -> bytes:
    raw = os.environ.get("ENCRYPTION_KEY") or load_dev_vars().get("ENCRYPTION_KEY", "")
    key = base64.b64decode(raw)
    if len(key) != 32:
        raise SystemExit("ENCRYPTION_KEY missing or not 32 bytes (signaling/.dev.vars)")
    return key


def seal(key: bytes, value: object) -> str:
    iv = secrets.token_bytes(12)
    ct = AESGCM(key).encrypt(iv, json.dumps(value, separators=(",", ":")).encode(), None)
    return f"{base64.b64encode(iv).decode()}.{base64.b64encode(ct).decode()}"


def unseal(key: bytes, sealed: str) -> dict:
    iv_b64, ct_b64 = sealed.split(".")
    return json.loads(AESGCM(key).decrypt(base64.b64decode(iv_b64), base64.b64decode(ct_b64), None))


def load_web_session(key: bytes) -> dict:
    state = json.loads(STATE_FILE.read_text())
    entry = state.get("web_session")
    if not entry:
        raise SystemExit(f"No web_session in {STATE_FILE}; save one via the signaling page first.")
    return unseal(key, entry["value"])


def save_web_session(key: bytes, stored: dict) -> None:
    """Read-modify-write that touches only `web_session`, atomically (tmp + rename)."""
    state = json.loads(STATE_FILE.read_text())
    prev = state.get("web_session", {})
    state["web_session"] = {**prev, "value": seal(key, stored)}
    tmp = STATE_FILE.with_name(f"{STATE_FILE.name}.fake-speaker.tmp")
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(state, f, indent=2)
    os.replace(tmp, STATE_FILE)


# --------------------------------------------------------------------------- HTTP/1.1 only

def https_request(method: str, path: str, headers: dict[str, str], body: bytes | None = None):
    """Plain http.client (HTTP/1.1, ALPN http/1.1) — mirrors what the ESP32 will do."""
    ctx = ssl.create_default_context()
    ctx.set_alpn_protocols(["http/1.1"])
    conn = http.client.HTTPSConnection(HOST, 443, context=ctx, timeout=30)
    try:
        conn.request(method, path, body=body, headers=headers)
        resp = conn.getresponse()
        data = resp.read()
        return resp.status, resp.headers, data
    finally:
        conn.close()


def jwt_claims(token: str) -> dict:
    try:
        part = token.split(".")[1]
        return json.loads(base64.urlsafe_b64decode(part + "=" * (-len(part) % 4)))
    except Exception:
        return {}


def read_rotated_cookies(headers) -> dict[str, str] | None:
    values: dict[str, str] = {}
    for line in headers.get_all("Set-Cookie") or []:
        m = re.match(rf"\s*({re.escape(COOKIE_NAME)}(?:\.\d+)?)=([^;]*)", line)
        if m and m.group(2):
            values[m.group(1)] = m.group(2)
    chunks = {k: v for k, v in values.items() if k != COOKIE_NAME}
    if chunks:
        return chunks
    return {COOKIE_NAME: values[COOKIE_NAME]} if COOKIE_NAME in values else None


def exchange_session(key: bytes) -> dict:
    stored = load_web_session(key)
    cookies = stored.get("sessionCookies") or {COOKIE_NAME: stored["sessionToken"]}
    device_id = stored.get("deviceId") or str(uuid.uuid4())
    cookie_header = "; ".join([*(f"{k}={v}" for k, v in cookies.items()), f"oai-did={device_id}"])
    status, headers, body = https_request("GET", "/api/auth/session", {
        "Host": HOST,
        "User-Agent": BROWSER_UA,
        "Accept": "application/json",
        "Accept-Language": "de-DE,de;q=0.9,en;q=0.8",
        "Cookie": cookie_header,
        "oai-device-id": device_id,
        "Origin": BASE,
        "Referer": f"{BASE}/",
    })
    if status != 200:
        cf = headers.get("cf-mitigated")
        raise SystemExit(f"auth/session -> HTTP {status} (cf-mitigated={cf}) body[:300]={body[:300]!r}")
    data = json.loads(body)
    token = data.get("accessToken")
    account_id = (data.get("account") or {}).get("id")
    email = (data.get("user") or {}).get("email")
    if not token or not account_id or jwt_claims(token).get("client_id") != WEB_CLIENT_ID:
        raise SystemExit("auth/session returned no usable web access token (session expired?)")
    rotated = read_rotated_cookies(headers)
    changed = rotated is not None and rotated != cookies
    if rotated is not None or stored.get("deviceId") != device_id:
        save_web_session(key, {**stored, "deviceId": device_id, "sessionCookies": rotated or cookies})
    exp = jwt_claims(token).get("exp", 0)
    return {
        "accessToken": token, "accountId": account_id, "deviceId": device_id, "email": email,
        "rotated": changed, "chunks": len(rotated or cookies),
        "tokenHoursLeft": round((exp - time.time()) / 3600, 1) if exp else None,
    }


def build_session(language: str | None, voice: str) -> dict:
    """Identical to buildChatGPTRealtimeSession() defaults for transport `wm`."""
    sid = str(uuid.uuid4())
    now = dt.datetime.now().astimezone()
    off = now.utcoffset() or dt.timedelta(0)
    tz = os.environ.get("TZ") or _local_tz_name() or "UTC"
    return {
        "conversation_id": None,
        "language_code": language,
        "requested_default_model": "",
        "voice": voice,
        "voice_session_id": sid,
        "voice_status_request_id": sid,
        # JS getTimezoneOffset(): minutes *behind* UTC (Berlin summer = -120)
        "timezone_offset_min": -int(off.total_seconds() // 60),
        "timezone": tz,
        "voice_mode": "wingman",
        "model_slug": "",
        "client_tools": [],
        "history_and_training_disabled": False,
        "conversation_mode": {"kind": "primary_assistant"},
        "model_slug_advanced": "",
        "enable_message_streaming": True,
        "backend_reasoning_effort": "high",
        "chat_mode": "chat",
    }


def _local_tz_name() -> str | None:
    try:
        link = os.readlink("/etc/localtime")
        return link.split("zoneinfo/", 1)[1]
    except Exception:
        return None


def post_offer(auth: dict, sdp: str, session: dict):
    boundary = f"----fakespeaker{secrets.token_hex(12)}"
    parts = []
    for name, value, ctype in (("sdp", sdp, None), ("session", json.dumps(session), None)):
        head = f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n'
        if ctype:
            head += f"Content-Type: {ctype}\r\n"
        parts.append(head.encode() + b"\r\n" + value.encode() + b"\r\n")
    body = b"".join(parts) + f"--{boundary}--\r\n".encode()
    headers = {
        "Host": HOST,
        "User-Agent": BROWSER_UA,
        "Accept-Language": "de-DE,de;q=0.9,en;q=0.8",
        "Authorization": f"Bearer {auth['accessToken']}",
        "chatgpt-account-id": auth["accountId"],
        "Accept": "application/sdp",
        "Content-Type": f"multipart/form-data; boundary={boundary}",
        "Content-Length": str(len(body)),
        "OAI-Language": session.get("language_code") or "en-US",
        "OAI-Device-Id": auth["deviceId"],
        "OAI-Client-Version": CLIENT_VERSION,
        "OAI-Client-Build-Number": CLIENT_BUILD,
        "OAI-Session-Id": str(uuid.uuid4()),
        "X-OpenAI-Target-Path": WM_PATH,
        "X-OpenAI-Target-Route": WM_PATH,
        "Origin": BASE,
        "Referer": f"{BASE}/",
    }
    return https_request("POST", f"{WM_PATH}?dcid=0", headers, body)


# --------------------------------------------------------------------------- media

class SilenceTrack(MediaStreamTrack):
    """20 ms frames of 48 kHz mono silence, paced in real time (what a muted mic sends)."""
    kind = "audio"

    def __init__(self):
        super().__init__()
        self._pts = 0
        self._start: float | None = None

    async def recv(self):
        samples = 960
        if self._start is None:
            self._start = time.time()
        else:
            wait = self._start + self._pts / 48000 - time.time()
            if wait > 0:
                await asyncio.sleep(wait)
        frame = AudioFrame(format="s16", layout="mono", samples=samples)
        for p in frame.planes:
            p.update(bytes(p.buffer_size))
        frame.sample_rate = 48000
        frame.pts = self._pts
        frame.time_base = Fraction(1, 48000)
        self._pts += samples
        return frame


def sdp_ips(sdp: str) -> set[str]:
    ips: set[str] = set()
    for line in sdp.splitlines():
        if line.startswith("c=IN "):
            ips.add(line.split()[2])
        if line.startswith("a=candidate:"):
            f = line.split()
            ips.add(f[4])
            if "raddr" in f:
                ips.add(f[f.index("raddr") + 1])
    return ips


def redact_sdp(sdp: str, local_ips: set[str]) -> str:
    """Redact *our* (client) addresses and ice-pwd. Server addresses, fingerprints, ufrag are kept."""
    out = []
    for line in sdp.splitlines():
        if line.startswith(("a=candidate:", "c=")):
            for ip in sorted(local_ips, key=len, reverse=True):
                if ip not in ("0.0.0.0", "127.0.0.1"):
                    line = line.replace(ip, "<v4>" if "." in ip else "<v6>")
        line = re.sub(r"^(a=ice-pwd:)\S+", r"\1<redacted>", line)
        out.append(line)
    return "\r\n".join(out) + "\r\n"


def fix_answer_for_aiortc(sdp: str) -> str:
    """aiortc workaround (not needed on a real WebRTC stack / esp_peer).

    ChatGPT's answer lists its ICE candidates only under the first (audio) m-line and has no
    a=end-of-candidates. aiortc keys remote candidates per transport and lets the *last* bundled
    m-line win (the data channel, which has none) -> zero remote candidates, ICE never checks.
    Copy the candidates into every m-section and close the candidate list.
    """
    lines = sdp.replace("\r\n", "\n").strip().split("\n")
    cands = [l for l in lines if l.startswith("a=candidate:")]
    out: list[str] = []
    sections: list[list[str]] = [[]]
    for l in lines:
        if l.startswith("m="):
            sections.append([])
        sections[-1].append(l)
    out += sections[0]
    for sec in sections[1:]:
        body = [l for l in sec if not l.startswith(("a=candidate:", "a=end-of-candidates"))]
        out += body + cands + ["a=end-of-candidates"]
    return "\r\n".join(out) + "\r\n"


class Transcript:
    """Decodes ChatGPT's compact JSON-patch stream carried by `chat_message_delta`.

    Observed wire format (payload.delta):
      {"c": 0, "p": "", "o": "add", "v": {"message": {...}}}      new message, slot c
      {"c": 1, "o": "patch", "v": [{"p": "/message/content/parts/0/text", "o": "append", "v": ", schön"}, ...]}
      {"v": [...]}                                                  same slot + same op as before
      {"p": "/message/status", "o": "replace", "v": "finished_successfully"}
    `c` and `o` are sticky (omitted = same as previous delta). Text lives in
    message.content.parts[0] (a string for user text, or {"content_type":"audio_transcription","text":...}).
    """

    def __init__(self):
        self.slots: dict[int, dict] = {}
        self.order: list[int] = []
        self.c = 0
        self.o = "add"
        self.p = ""

    def on_delta(self, delta) -> None:
        if not isinstance(delta, dict):
            return
        if "c" in delta:
            self.c = delta["c"]
        if "o" in delta:
            self.o = delta["o"]
        if "p" in delta:
            self.p = delta["p"]
        self._apply(self.c, self.p if "p" in delta or self.o != "patch" else "", self.o, delta.get("v"))

    def _apply(self, c: int, path: str, op: str, value) -> None:
        if op == "patch":
            for sub in value or []:
                if isinstance(sub, dict):
                    self._apply(c, sub.get("p", ""), sub.get("o", "replace"), sub.get("v"))
            return
        if path == "" and op in ("add", "replace"):
            if c not in self.slots:
                self.order.append(c)
            self.slots[c] = value if isinstance(value, dict) else {}
            return
        doc = self.slots.setdefault(c, {})
        keys = [k.replace("~1", "/").replace("~0", "~") for k in path.strip("/").split("/")]
        parent = doc
        try:
            for k in keys[:-1]:
                parent = parent[int(k)] if isinstance(parent, list) else parent.setdefault(k, {})
            last = keys[-1]
            if isinstance(parent, list):
                last = int(last)
            if op == "append":
                cur = parent[last] if (isinstance(parent, list) or last in parent) else None
                parent[last] = (cur or ("" if isinstance(value, str) else [])) + value
            elif op == "remove":
                if isinstance(parent, list):
                    parent.pop(last)
                else:
                    parent.pop(last, None)
            elif op == "truncate" and isinstance(value, int):
                parent[last] = parent[last][:value]
            else:  # add / replace
                parent[last] = value
        except (KeyError, IndexError, TypeError, ValueError):
            pass

    @staticmethod
    def text_of(msg: dict) -> str:
        parts = (msg.get("content") or {}).get("parts") or []
        out = []
        for part in parts:
            if isinstance(part, str):
                out.append(part)
            elif isinstance(part, dict) and isinstance(part.get("text"), str):
                out.append(part["text"])
        return "".join(out).strip()

    def snapshot(self) -> dict[int, str]:
        return {c: self.text_of((self.slots[c] or {}).get("message") or {}) for c in self.order}

    def final(self) -> list[tuple[str, str]]:
        res = []
        for c in self.order:
            msg = (self.slots[c] or {}).get("message") or {}
            text = self.text_of(msg)
            if text:
                res.append(((msg.get("author") or {}).get("role", "?"), text))
        return res


# --------------------------------------------------------------------------- call

async def run(args) -> int:
    key = _key()
    caps = HERE / "captures"
    caps.mkdir(exist_ok=True)
    tag = args.tag or dt.datetime.now().strftime("%Y%m%d-%H%M%S")

    t = time.perf_counter()
    auth = exchange_session(key)
    t_auth = (time.perf_counter() - t) * 1000
    say(f"auth/session OK in {t_auth:.0f} ms (cookie chunks={auth['chunks']}, rotated={auth['rotated']}, "
        f"token valid ~{auth['tokenHoursLeft']} h)")
    if args.auth_only:
        return 0

    pc = RTCPeerConnection(RTCConfiguration(iceServers=[], bundlePolicy=RTCBundlePolicy.MAX_BUNDLE))
    # --- outgoing media first. aiortc quirk: if the data channel is created before the first
    # transceiver, max-bundle ends up stopping the shared transport when the answer arrives.
    player = None
    if args.mic:
        from aiortc.contrib.media import MediaPlayer
        player = MediaPlayer(f":{args.mic_device or 'default'}", format="avfoundation")
        pc.addTrack(player.audio)
    else:
        pc.addTrack(SilenceTrack())
    if not args.no_video:
        pc.addTransceiver("video", direction="sendonly")
    if args.dc_mode == "negotiated":
        dc = pc.createDataChannel(args.dc_label, negotiated=True, id=0 if args.dc_id is None else args.dc_id)
    else:
        # in-band DCEP open (what esp_peer does). aiortc picks the stream parity from the *ICE*
        # role (controlling -> odd), not the DTLS role, so --dc-id lets us force RFC 8832 parity.
        dc = pc.createDataChannel(args.dc_label, negotiated=False, id=args.dc_id)
    sctp_trace = install_sctp_trace()

    events_f = open(args.log, "w") if args.log else None
    counts: Counter[str] = Counter()
    marks: dict[str, int] = {}
    transcript = Transcript()
    state = {"value": "connecting", "done": asyncio.Event(), "audio_frames": 0, "nonzero_frames": 0,
             "sum_sq": 0.0, "samples": 0, "peak": 0, "lr_differs": 0, "last_frame_at": None,
             "max_gap_ms": 0, "gaps_over_100ms": 0}

    def mark(name: str) -> None:
        if name not in marks:
            marks[name] = ms()
            say(f"** {name}")

    def log_event(direction: str, ev: dict) -> None:
        if events_f and not events_f.closed:
            events_f.write(json.dumps({"t": dt.datetime.now().isoformat(timespec="milliseconds"),
                                       "ms": ms(), "dir": direction, "event": ev}, ensure_ascii=False) + "\n")
            events_f.flush()

    def send(ev: dict) -> None:
        log_event("out", ev)
        dc.send(json.dumps({"type": "data_message", "data": json.dumps(ev)}))

    @pc.on("connectionstatechange")
    async def _pcstate():
        say(f"pc connectionState={pc.connectionState}")
        log_event("local", {"type": "pc_state", "state": pc.connectionState})
        if pc.connectionState == "connected":
            mark("dtls_connected")
        if pc.connectionState in ("failed", "closed"):
            state["done"].set()

    @pc.on("iceconnectionstatechange")
    async def _icestate():
        say(f"ice state={pc.iceConnectionState}")

    @dc.on("open")
    def _open():
        mark("datachannel_open")
        say(f"data channel open: label={dc.label!r} id={dc.id} negotiated={dc.negotiated}")

    @dc.on("close")
    def _close():
        say(f"data channel closed (id={dc.id})")
        log_event("local", {"type": "datachannel_closed", "id": dc.id})
        if args.dc_mode != "dcep" or pc.connectionState != "connected":
            state["done"].set()

    @dc.on("message")
    def _message(raw):
        mark("first_dc_message")
        ev = parse_event(raw)
        if ev is None:
            counts["<unparsed>"] += 1
            log_event("in", {"type": "unparsed", "raw": str(raw)[:400]})
            return
        typ = ev.get("type", "?")
        counts[typ] += 1
        log_event("in", ev)
        p = ev.get("payload") if isinstance(ev.get("payload"), dict) else ev
        if typ == "state_update":
            mark("first_state_update")
            ns = p.get("new_state")
            if ns and ns != state["value"]:
                say(f"state: {state['value']} -> {ns}")
                state["value"] = ns
        elif typ == "chat_message_delta":
            before = transcript.snapshot()
            known = set(transcript.order)
            transcript.on_delta(p.get("delta"))
            for c in transcript.order:
                if c not in known:
                    m = transcript.slots[c].get("message") or {}
                    a = m.get("author") or {}
                    ct = (m.get("content") or {}).get("content_type")
                    say(f"   + message[{c}] {a.get('role')}{':' + a['name'] if a.get('name') else ''} ({ct})")
            if args.verbose:
                for c, txt in transcript.snapshot().items():
                    if txt and txt != before.get(c):
                        role = ((transcript.slots[c].get("message") or {}).get("author") or {}).get("role")
                        say(f"   [{role}] {txt[-140:]}")
        elif typ in ("user_transcription_text", "live_captioning_text"):
            txt = p.get("text") or p.get("transcript")
            if txt and args.verbose:
                say(f"   {typ}: {txt[-120:]}")
        elif typ in ("goodbye", "close_ready", "close_request"):
            say(f"server sent {typ}")
            if typ != "close_request":
                state["done"].set()
        if INTERESTING.search(json.dumps(ev)) and typ not in ("chat_message_delta",):
            say(f"   * {typ}: {json.dumps(p, ensure_ascii=False)[:200]}")

    @pc.on("datachannel")
    def _remote_dc(ch):
        say(f"server opened a data channel: label={ch.label!r} id={ch.id} protocol={ch.protocol!r}")
        log_event("local", {"type": "remote_datachannel", "label": ch.label, "id": ch.id})
        ch.on("message", _message)

    # --- incoming audio
    wav = None
    out_stream = None
    if args.record:
        wav = wave.open(args.record, "wb")
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(48000)
    if args.play:
        import sounddevice as sd
        out_stream = sd.RawOutputStream(samplerate=48000, channels=1, dtype="int16", latency="low")
        out_stream.start()

    @pc.on("track")
    def _track(track):
        say(f"remote track: {track.kind}")
        if track.kind != "audio":
            return

        async def pump():
            first_fmt = True
            while True:
                try:
                    frame = await track.recv()
                except Exception:
                    return
                pcm = frame.to_ndarray()  # s16 interleaved, shape (1, samples*channels)
                ch = len(frame.layout.channels)
                inter = pcm.reshape(-1, ch)
                mono = inter.mean(axis=1).astype(np.int16) if ch > 1 else inter[:, 0]
                if ch > 1 and np.any(inter[:, 0] != inter[:, 1]):
                    state["lr_differs"] += 1
                now = time.perf_counter()
                if state["last_frame_at"] is not None:
                    gap = (now - state["last_frame_at"]) * 1000
                    state["max_gap_ms"] = max(state["max_gap_ms"], round(gap))
                    if gap > 100:
                        state["gaps_over_100ms"] += 1
                state["last_frame_at"] = now
                if first_fmt:
                    first_fmt = False
                    say(f"first audio frame: {frame.format.name} {frame.sample_rate} Hz "
                        f"{frame.layout.name} {frame.samples} samples/frame")
                    mark("first_audio_frame")
                state["audio_frames"] += 1
                peak = int(np.abs(mono.astype(np.int32)).max()) if mono.size else 0
                if peak > 64:
                    state["nonzero_frames"] += 1
                    mark("first_audible_audio_frame")
                state["peak"] = max(state["peak"], peak)
                state["sum_sq"] += float(np.square(mono.astype(np.float64)).sum())
                state["samples"] += mono.size
                b = mono.tobytes()
                if wav:
                    wav.writeframes(b)
                if out_stream:
                    out_stream.write(b)

        asyncio.ensure_future(pump())

    # --- offer
    t = time.perf_counter()
    await pc.setLocalDescription(await pc.createOffer())  # aiortc gathers all candidates here
    offer = pc.localDescription.sdp
    if args.dtls_setup != "actpass":
        # experiment: what if the device insists on a DTLS role (esp_peer may not do actpass)?
        offer = offer.replace("a=setup:actpass", f"a=setup:{args.dtls_setup}")
    say(f"offer ready (ICE gathered) in {(time.perf_counter() - t) * 1000:.0f} ms, {len(offer)} bytes")
    my_ips = sdp_ips(offer)
    (caps / f"{tag}-offer.sdp").write_text(redact_sdp(offer, my_ips))

    session = build_session(args.language, args.voice)
    t = time.perf_counter()
    status, headers, body = await asyncio.to_thread(post_offer, auth, offer, session)
    t_sig = (time.perf_counter() - t) * 1000
    text = body.decode(errors="replace")
    if status not in (200, 201) or not text.lstrip().startswith("v=0"):
        say(f"/realtime/wm FAILED: HTTP {status} ct={headers.get('content-type')} "
            f"cf-mitigated={headers.get('cf-mitigated')} cf-ray={headers.get('cf-ray')}")
        print(text[:2000])
        (caps / f"{tag}-error.txt").write_text(f"HTTP {status}\n{headers}\n\n{text[:4000]}")
        await pc.close()
        return 2
    say(f"/realtime/wm -> HTTP {status} {headers.get('content-type')} in {t_sig:.0f} ms")
    (caps / f"{tag}-answer.sdp").write_text(redact_sdp(text, my_ips))
    (caps / f"{tag}-session.json").write_text(json.dumps(session, indent=2))
    await pc.setRemoteDescription(RTCSessionDescription(sdp=fix_answer_for_aiortc(text), type="answer"))

    # --- wait for data channel, then relay + run
    try:
        await asyncio.wait_for(_until(lambda: dc.readyState == "open" or state["done"].is_set()),
                               args.dcep_ack_timeout if args.dc_mode == "dcep" else 15)
    except asyncio.TimeoutError:
        if args.dc_mode == "dcep" and pc.connectionState == "connected" and dc.readyState != "closed":
            # No DATA_CHANNEL_ACK: the server ignores DCEP. Send on the stream anyway.
            say(f"no DCEP ACK after {args.dcep_ack_timeout:.0f} s -> forcing channel open on stream {dc.id}")
            log_event("local", {"type": "dcep_no_ack", "stream_id": dc.id})
            dc._setReadyState("open")
        else:
            say(f"data channel not usable (state={dc.readyState})")
            state["done"].set()
    if dc.readyState == "open" and args.relay:
        await asyncio.sleep(args.relay_delay)
        send({"type": "relay_message", "payload": {"type": "relay_message", "message": {
            "id": str(uuid.uuid4()), "author": {"role": "user"}, "create_time": time.time(),
            "content": {"content_type": "text", "parts": [args.relay]}, "metadata": {}}}})
        mark("relay_sent")
    try:
        await asyncio.wait_for(state["done"].wait(), args.duration)
    except asyncio.TimeoutError:
        pass
    say("hanging up")
    if player and player.audio:
        player.audio.stop()
    await pc.close()
    if wav:
        wav.close()
    if out_stream:
        out_stream.stop()
        out_stream.close()
    if events_f:
        events_f.close()

    # --- summary
    rms = math.sqrt(state["sum_sq"] / state["samples"]) if state["samples"] else 0.0
    print("\n=== timings (ms since start) ===")
    print(f"auth/session request: {t_auth:.0f}   /realtime/wm request: {t_sig:.0f}")
    for k, v in marks.items():
        print(f"{k:28s} {v}")
    print("\n=== received audio ===")
    print(f"frames={state['audio_frames']} audible_frames={state['nonzero_frames']} "
          f"seconds={state['samples'] / 48000:.1f} rms={rms:.1f} peak={state['peak']} (int16)")
    print(f"frames where L!=R: {state['lr_differs']}  max inter-frame gap: {state['max_gap_ms']} ms  "
          f"gaps>100ms: {state['gaps_over_100ms']}")
    print("\n=== transcript ===")
    for role, txt in transcript.final():
        print(f"[{role}] {txt}")
    print("\n=== SCTP receive trace (stream_id, ppid -> count; 50=DCEP 51=string 53=binary) ===")
    for (sid, ppid, kind), n in sorted(sctp_trace.items()):
        print(f"stream {sid} ppid {ppid} {kind}: {n}")
    print("\n=== event counts ===")
    for k, v in counts.most_common():
        print(f"{v:5d}  {k}")
    return 0


def install_sctp_trace() -> Counter:
    """Count inbound SCTP user messages per (stream id, PPID, DCEP type)."""
    from aiortc import rtcsctptransport as st
    trace: Counter = Counter()
    orig = st.RTCSctpTransport._receive
    if getattr(orig, "_traced", False):
        orig = orig._orig

    async def _receive(self, stream_id, pp_id, data):
        kind = ""
        if pp_id == 50 and data:
            kind = {3: "DCEP_OPEN", 2: "DCEP_ACK"}.get(data[0], f"DCEP_{data[0]}")
        trace[(stream_id, pp_id, kind)] += 1
        return await orig(self, stream_id, pp_id, data)

    _receive._traced = True
    _receive._orig = orig
    st.RTCSctpTransport._receive = _receive
    return trace


INTERESTING = re.compile(r"web\.run|search|brows|source|citation|spawn|handoff|research", re.I)


async def _until(cond, step: float = 0.02):
    while not cond():
        await asyncio.sleep(step)


def parse_event(raw):
    v = raw
    for _ in range(4):
        if isinstance(v, (bytes, bytearray)):
            v = v.decode(errors="replace")
        if isinstance(v, str):
            try:
                v = json.loads(v)
            except ValueError:
                return None
            continue
        if isinstance(v, dict) and v.get("type") == "data_message" and "data" in v:
            v = v["data"]
            continue
        break
    return v if isinstance(v, dict) and isinstance(v.get("type"), str) else None


def main() -> int:
    ap = argparse.ArgumentParser(description="Headless GPT-Live client that mimics the ESP32 speaker.")
    ap.add_argument("--no-video", action="store_true", help="omit the inert sendonly video m-line")
    ap.add_argument("--relay", help="typed relay_message to send after the data channel opens")
    ap.add_argument("--relay-delay", type=float, default=0.5, help="seconds to wait before --relay")
    ap.add_argument("--duration", type=float, default=30, help="call length in seconds")
    mic = ap.add_mutually_exclusive_group()
    mic.add_argument("--mic", dest="mic", action="store_true", help="send the Mac default mic")
    mic.add_argument("--no-mic", dest="mic", action="store_false", help="send silence (default)")
    ap.set_defaults(mic=False)
    ap.add_argument("--mic-device", help="avfoundation audio device index (default: system default)")
    ap.add_argument("--play", action="store_true", help="play received audio on the Mac speakers")
    ap.add_argument("--record", help="write received audio to this WAV (48 kHz mono s16)")
    ap.add_argument("--log", help="write all data-channel events as JSONL")
    ap.add_argument("--language", default="de-DE")
    ap.add_argument("--voice", default="juniper")
    ap.add_argument("--tag", help="prefix for files in captures/ (default: timestamp)")
    ap.add_argument("--dtls-setup", choices=["actpass", "active", "passive"], default="actpass",
                    help="rewrite a=setup in the offer (experiment)")
    ap.add_argument("--dc-mode", choices=["negotiated", "dcep"], default="negotiated",
                    help="negotiated id 0 (browser) or in-band DCEP open (esp_peer)")
    ap.add_argument("--dc-id", type=int, help="force the SCTP stream id of our data channel")
    ap.add_argument("--dcep-ack-timeout", type=float, default=3.0,
                    help="DCEP mode: seconds to wait for DATA_CHANNEL_ACK before sending anyway")
    ap.add_argument("--dc-label", default="", help="data channel label (DCEP mode sends it)")
    ap.add_argument("--auth-only", action="store_true", help="only do the auth/session exchange (+cookie rotation)")
    ap.add_argument("--debug", action="store_true", help="aiortc/aioice debug logging")
    ap.add_argument("-v", "--verbose", action="store_true", help="print streaming transcript updates")
    args = ap.parse_args()
    if args.debug:
        import logging
        logging.basicConfig(level=logging.DEBUG, format="%(relativeCreated)6d %(name)s %(message)s")
    return asyncio.run(run(args))


if __name__ == "__main__":
    sys.exit(main())
