# Phase 0: prove subscription-backed GPT-Live signaling

Goal: before the hardware matters, prove that we can open a full-duplex GPT-Live session through
our own signaling server using your ChatGPT web session, and that it researches in the background
the way chatgpt.com voice does.

## Findings so far (2026-10-24)

| Test | Result |
|---|---|
| `chatgpt.com` from **Cloudflare Workers** (deployed, FRA) | ❌ **Hard-blocked.** Every endpoint, even `/backend-api/me`, returns OpenAI's 403 "blocked" page. Not a solvable challenge. |
| `chatgpt.com` from **local `wrangler dev` (workerd)** and **curl**, home IP | ❌ `cf-mitigated: challenge` (bot challenge; fingerprint-based) |
| **HTTP/1.1** clients from home IP (curl `--http1.1`, Python urllib), even without a User-Agent | ✅ Reachable. The challenge targets curl's/workerd's **HTTP/2** fingerprint. Good sign for the ESP32 (HTTP/1.1, mbedTLS); still to verify on the device. |
| `chatgpt.com` from **Node 24 `fetch`**, home IP | ✅ Real API responses (200 / 401), no challenge |
| `chatgpt.com` from **GitHub Actions (Azure, US datacenter)**, Node `fetch` | ✅ Reachable. So datacenter IPs are fine; the block is specific to Workers (likely the unremovable `CF-Worker` header). |
| Token exchange with a real session (chunked `.0`/`.1` cookie) | ✅ |
| `/realtime/wm` call + full-duplex audio (Safari) | ✅ connect ≈ 4.9 s |
| Background research in voice | ✅ `web.run` searches ("Searched 11 websites", 18 s) on `gpt-6-thinking` while still talking; `spawn_update` events bracket the background work |
| Limits reported by `usage_update` | audio ≈ 15 h remaining, **session cap ≈ 2 h** (7170 s) |

**Consequence:** the Worker can't make the two chatgpt.com calls (mint token, post SDP) itself,
but anything that isn't a Worker can: home machine, or a datacenter/serverless function. Options:
keep the Worker as the device's front door and hand those two calls to (a) a stateless
serverless relay elsewhere (Lambda / Vercel / Cloud Run), or (b) a home machine via a Cloudflare Tunnel.
Audio goes directly from the device to ChatGPT either way.

## Steps

### 1. Start the server
```sh
cd signaling && npm start      # http://localhost:8787
```
Open it in **Chrome or Safari**, paste `ADMIN_TOKEN` from `signaling/.dev.vars` into *Admin token*.

### 2. Give it your ChatGPT web session
1. Log in at chatgpt.com in the same browser.
2. Safari: Develop → Show Web Inspector → Storage → Cookies → `https://chatgpt.com`.
   Chrome: DevTools → Application → Cookies.
3. Copy the value of `__Secure-next-auth.session-token`.
   If there are `…session-token.0` and `.1`, paste JSON instead:
   `{"__Secure-next-auth.session-token.0":"…","__Secure-next-auth.session-token.1":"…"}`
4. Paste it under *ChatGPT web session* → **Save session**. It should show your email.

The value is AES-GCM-encrypted into `~/.gpt-live-speaker/state.json` and never returned to the page.
Treat it like a password: it is your logged-in ChatGPT session.

### 3. Talk
**Connect**, allow the microphone, talk. Check:
- [ ] Session connects; note *Connect* ms and *First audio* ms.
- [ ] Full duplex: talk over it, and it stops and listens.
- [ ] German works (`de-DE`), and switching voices works.
- [ ] **Background research**: ask something current ("Was sind die Nachrichten heute in Berlin?").
      Does it keep talking while it looks things up? Watch section 3 for highlighted
      search / tool / source events. **Download log** afterwards.
- [ ] Typed message (relay) gets a spoken answer.
- [ ] Leave it idle 5–10 min: does the session stay open?

### 4. If research does NOT happen
Compare our request with the real one:
1. On chatgpt.com, open DevTools → Network, start a voice conversation.
2. Find the `POST /realtime/wm` request → Payload → the `session` form field (JSON).
3. Compare with *Last sent* on our page. Copy fields that differ (not ids) into *Overrides* → Save → reconnect.
4. Also note request headers `OAI-Client-Version` / `OAI-Client-Build-Number`; if they differ a lot,
   put them in `.dev.vars` as `CHATGPT_CLIENT_VERSION` / `CHATGPT_CLIENT_BUILD` and restart.

### 5. Longevity
- **Force token refresh** a day later. Does the stored session still mint tokens? (The cookie rotates; we persist it.)

## Notes for the firmware (Phase 2)
- The SDP offer must contain: one audio m-line (Opus), an **inert `sendonly` video m-line**
  (the handshake needs it; toggle *inert video m-line* to test without), and an SCTP data channel
  negotiated with id 0. `bundlePolicy: max-bundle`.
- Data-channel messages are wrapped: `{"type":"data_message","data":"<json string>"}`.
- Control: `action_request` with `stop_speaking`, `start_listening`, …; state arrives as `state_update`.
