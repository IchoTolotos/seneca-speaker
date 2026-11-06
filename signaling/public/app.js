// Phase 0 browser client. Mirrors login-with-chatgpt's connectChatGPTRealtime()
// (same SDP shape the ESP32 will have to reproduce) and logs every data-channel event.

const $ = (id) => document.getElementById(id);
const INTERESTING = /search|brows|tool|source|citation|url|web|handoff|research|delegat/i;

const admin = $("admin");
admin.value = localStorage.getItem("adminToken") ?? "";
admin.addEventListener("change", () => localStorage.setItem("adminToken", admin.value));

async function api(path, init = {}) {
  const res = await fetch(path, {
    ...init,
    headers: { "content-type": "application/json", authorization: `Bearer ${admin.value}`, ...(init.headers ?? {}) },
  });
  const body = await res.json().catch(() => ({}));
  if (!res.ok) throw Object.assign(new Error(body.message ?? `HTTP ${res.status}`), { body, status: res.status });
  return body;
}

const show = (el, value) => { el.textContent = typeof value === "string" ? value : JSON.stringify(value, null, 2); };

// ---------- status / setup ----------

async function refreshStatus(force = false) {
  try {
    const s = await api(`/api/status${force ? "?refresh" : ""}`);
    show($("status"), s);
    $("overrides").value = JSON.stringify(s.overrides ?? {}, null, 2);
  } catch (e) { show($("status"), e.body ?? String(e)); }
}
$("refreshStatus").onclick = () => refreshStatus();
$("refreshToken").onclick = () => refreshStatus(true);
$("saveSession").onclick = async () => {
  try {
    const first = $("sessionToken").value.trim();
    const second = $("sessionToken1").value.trim();
    const name = "__Secure-next-auth.session-token";
    const sessionToken = second ? JSON.stringify({ [`${name}.0`]: first, [`${name}.1`]: second }) : first;
    const r = await api("/api/setup", { method: "POST", body: JSON.stringify({ sessionToken }) });
    $("sessionToken").value = ""; $("sessionToken1").value = "";
    show($("status"), r);
  } catch (e) { show($("status"), e.body ?? String(e)); }
};
$("clearSession").onclick = async () => show($("status"), await api("/api/setup", { method: "DELETE" }));
$("saveOverrides").onclick = async () => {
  try { show($("sentSession"), await api("/api/overrides", { method: "PUT", body: $("overrides").value })); }
  catch (e) { show($("sentSession"), e.body ?? String(e)); }
};

// ---------- event log ----------

const log = [];
const counts = {};
function addEvent(direction, event) {
  const entry = { t: new Date().toISOString(), direction, event };
  log.push(entry);
  counts[event.type] = (counts[event.type] ?? 0) + 1;
  $("counts").textContent = Object.entries(counts).map(([k, v]) => `${k}:${v}`).join("  ");
  renderEvent(entry);
}
function renderEvent({ t, direction, event }) {
  const raw = JSON.stringify(event);
  const hit = INTERESTING.test(raw);
  if ($("onlyInteresting").checked && !hit) return;
  const div = document.createElement("div");
  div.className = `ev${hit ? " hit" : ""}`;
  div.innerHTML = `<span class="hint">${t.slice(11, 23)} ${direction}</span> <b></b> <span></span>`;
  div.querySelector("b").textContent = event.type;
  div.querySelector("span:last-child").textContent = raw.length > 600 ? `${raw.slice(0, 600)}…` : raw;
  const box = $("events");
  const stick = box.scrollTop + box.clientHeight >= box.scrollHeight - 20;
  box.appendChild(div);
  if (stick) box.scrollTop = box.scrollHeight;
}
$("onlyInteresting").onchange = () => { $("events").innerHTML = ""; log.forEach(renderEvent); };
$("clearLog").onclick = () => { log.length = 0; for (const k in counts) delete counts[k]; $("events").innerHTML = ""; $("counts").textContent = ""; };
$("download").onclick = () => {
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([JSON.stringify(log, null, 2)], { type: "application/json" }));
  a.download = `gpt-live-events-${new Date().toISOString().replace(/[:.]/g, "-")}.json`;
  a.click();
};

// ---------- wire format (same as login-with-chatgpt) ----------

function parseEvent(input) {
  let value = input;
  for (let i = 0; i < 4; i++) {
    if (value instanceof ArrayBuffer) value = new TextDecoder().decode(value);
    if (typeof value === "string") { try { value = JSON.parse(value); } catch { return undefined; } continue; }
    if (value && value.type === "data_message" && "data" in value) { value = value.data; continue; }
    break;
  }
  return value && typeof value.type === "string" ? value : undefined;
}
const encode = (event) => JSON.stringify({ type: "data_message", data: JSON.stringify(event) });
const payloadOf = (e) => (e.payload && typeof e.payload === "object" ? e.payload : e);

// ---------- transcript ----------

let lastCaption;
function addTranscript(kind, text) {
  const box = $("transcript");
  // Assistant captions stream in as growing snapshots; keep updating one line until the user speaks.
  if (kind !== "assistant" || !lastCaption) {
    const p = document.createElement("div");
    p.className = kind;
    box.appendChild(p);
    lastCaption = kind === "assistant" ? p : undefined;
    if (kind === "user") p.textContent = `you: ${text}`;
  }
  if (kind === "assistant") lastCaption.textContent = `gpt: ${text}`;
  box.scrollTop = box.scrollHeight;
}

// ---------- call ----------

let call;
const setState = (s) => { $("state").textContent = s; if (call) call.state = s; };
const setButtons = (live) => {
  $("connect").disabled = live;
  for (const id of ["hangup", "mute", "stopSpeaking", "sendTyped"]) $(id).disabled = !live;
};

function waitFor(target, event, done, ms) {
  return new Promise((resolve, reject) => {
    if (done()) return resolve();
    const t = setTimeout(() => { target.removeEventListener(event, on); reject(new Error(`timeout waiting for ${event}`)); }, ms);
    const on = () => { if (done()) { clearTimeout(t); target.removeEventListener(event, on); resolve(); } };
    target.addEventListener(event, on);
  });
}

$("connect").onclick = async () => {
  const t0 = performance.now();
  $("tConnect").textContent = "…"; $("tAudio").textContent = "…";
  setState("connecting");
  setButtons(true);
  try {
    const stream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: true, noiseSuppression: true, autoGainControl: true } });
    const pc = new RTCPeerConnection({ bundlePolicy: "max-bundle" });
    const dc = pc.createDataChannel("", { negotiated: true, id: 0 });
    const audio = new Audio(); audio.autoplay = true;
    call = { pc, dc, stream, audio, state: "connecting" };

    let gotAudio = false;
    pc.ontrack = (e) => {
      if (e.track.kind !== "audio") return;
      audio.srcObject = e.streams[0] ?? new MediaStream([e.track]);
      audio.play().catch(console.error);
    };
    pc.onconnectionstatechange = () => { addEvent("local", { type: "pc_state", state: pc.connectionState }); if (pc.connectionState === "failed") hangup("webrtc failed"); };
    dc.onmessage = async (m) => {
      const data = m.data instanceof Blob ? await m.data.arrayBuffer() : m.data;
      const ev = parseEvent(data);
      if (!ev) return addEvent("in", { type: "unparsed", raw: String(data).slice(0, 400) });
      addEvent("in", ev);
      const p = payloadOf(ev);
      if (ev.type === "state_update" && p.new_state) {
        setState(p.new_state);
        if (p.new_state === "speaking" && !gotAudio) { gotAudio = true; $("tAudio").textContent = `${Math.round(performance.now() - t0)} ms`; }
      }
      if (ev.type === "user_transcription_text") addTranscript("user", p.text ?? p.transcript ?? "");
      if (ev.type === "live_captioning_text") addTranscript("assistant", p.text ?? p.transcript ?? "");
      if (ev.type === "goodbye" || ev.type === "close_ready") hangup(ev.type);
    };
    dc.onclose = () => { if (call) hangup("data channel closed"); };

    for (const track of stream.getTracks()) pc.addTrack(track, stream);
    if ($("videoLine").checked) pc.addTransceiver("video", { direction: "sendonly" });
    await pc.setLocalDescription(await pc.createOffer());
    await waitFor(pc, "icegatheringstatechange", () => pc.iceGatheringState === "complete", 2500).catch(() => {});

    const res = await api("/api/realtime", {
      method: "POST",
      body: JSON.stringify({
        sdp: pc.localDescription.sdp,
        session: {
          voice: $("voice").value,
          language: $("language").value || null,
          timezone: Intl.DateTimeFormat().resolvedOptions().timeZone,
          timezoneOffsetMinutes: new Date().getTimezoneOffset(),
        },
      }),
    });
    show($("sentSession"), res.sentSession);
    await pc.setRemoteDescription({ type: "answer", sdp: res.sdp });
    await waitFor(dc, "open", () => dc.readyState === "open", 10000);
    $("tConnect").textContent = `${Math.round(performance.now() - t0)} ms`;
    setState("listening");
    startBargeIn(stream);
  } catch (e) {
    addEvent("local", { type: "error", message: e.message, body: e.body });
    hangup("error");
  }
};

function send(event) {
  if (call?.dc.readyState !== "open") return;
  addEvent("out", event);
  call.dc.send(encode(event));
}
const action = (name) => send({ type: "action_request", payload: { action: name } });

function hangup(reason = "user") {
  if (!call) return;
  const c = call; call = undefined;
  c.stopBargeIn?.();
  c.dc.close(); c.pc.close();
  c.stream.getTracks().forEach((t) => t.stop());
  c.audio.srcObject = null;
  addEvent("local", { type: "hangup", reason });
  setState("halted");
  setButtons(false);
}

$("hangup").onclick = () => hangup();
$("stopSpeaking").onclick = () => action("stop_speaking");
$("mute").onclick = () => {
  const track = call?.stream.getAudioTracks()[0];
  if (!track) return;
  track.enabled = !track.enabled;
  $("mute").textContent = track.enabled ? "Mute mic" : "Unmute mic";
};
$("sendTyped").onclick = () => {
  const text = $("typed").value.trim();
  if (!text) return;
  send({ type: "relay_message", payload: { type: "relay_message", message: {
    id: crypto.randomUUID(), author: { role: "user" }, create_time: Date.now() / 1000,
    content: { content_type: "text", parts: [text] }, metadata: {},
  } } });
  addTranscript("user", `(typed) ${text}`);
  $("typed").value = "";
};
$("typed").onkeydown = (e) => { if (e.key === "Enter") $("sendTyped").click(); };

// Local barge-in: interrupt the model when you start talking over it (same thresholds as the SDK).
function startBargeIn(stream) {
  const ctx = new AudioContext();
  const analyser = ctx.createAnalyser();
  analyser.fftSize = 1024;
  ctx.createMediaStreamSource(stream).connect(analyser);
  const buf = new Float32Array(analyser.fftSize);
  let above, last = -Infinity, frame;
  const tick = (now) => {
    analyser.getFloatTimeDomainData(buf);
    let e = 0; for (const s of buf) e += s * s;
    const rms = Math.sqrt(e / buf.length);
    if (call?.state === "speaking" && rms >= 0.045) {
      above ??= now;
      if (now - above >= 120 && now - last >= 800) {
        last = now; above = undefined;
        action("stop_speaking");
        call.audio.muted = true; setTimeout(() => { if (call) call.audio.muted = false; }, 350);
      }
    } else above = undefined;
    frame = requestAnimationFrame(tick);
  };
  frame = requestAnimationFrame(tick);
  call.stopBargeIn = () => { cancelAnimationFrame(frame); ctx.close(); };
}

if (admin.value) refreshStatus();
