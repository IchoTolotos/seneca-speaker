# Remote device config

`device-config.json` lets us adjust the speaker when chatgpt.com's web client changes, without
reflashing. The device fetches it at boot (and may refetch daily) from GitHub raw:

```
https://raw.githubusercontent.com/<owner>/<repo>/main/config/device-config.json
```

## The file is optional

The device must work without it. Firmware rules:
1. Compiled-in defaults equal the values in this file. Fetch with a short timeout (5 s); on any
   network/TLS/HTTP error keep the last good copy cached in NVS, or the compiled defaults.
2. Validate before use. **Ignore the whole file** if it isn't a JSON object, is larger than 8 KB,
   or `version` isn't a supported integer (currently `1`; higher versions are ignored entirely).
3. Validate each field on its own and fall back to the default for that field if it is missing or invalid:
   - `chatgpt.base_url`: string starting with `https://`, ≤ 128 chars, no trailing `/`.
   - `client_version`, `client_build`, `web_client_id`, `user_agent`: non-empty printable ASCII,
     no CR/LF, ≤ 256 chars (they go into HTTP headers).
   - `session_overrides`: JSON object (≤ 2 KB serialized), merged shallowly on top of the `/wm`
     session JSON by `sc_session_json_build()`. Never let it set `voice_session_id` /
     `voice_status_request_id`; the device generates those per call.
   - `policy.*`: integers within bounds: `idle_close_s` 0 (never) or 30–86400;
     `rotate_before_cap_s` 10–600; `daily_refresh_s` 3600–604800; `token_refresh_margin_s` 0–604800.
4. Unknown keys are ignored (forward compatible). Only the device reads this file, and it holds no
   secrets. Anything security-relevant (cookies, Wi-Fi) never comes from here.
5. Pin the TLS chain for `raw.githubusercontent.com` with the ESP-IDF certificate bundle.

## Fields

| Field | Meaning | Source |
|---|---|---|
| `version` | Schema version. Bump only for incompatible changes. | |
| `chatgpt.base_url` | Origin for `/api/auth/session` and `/realtime/wm`; also used for `Origin`/`Referer`. | `DEFAULT_REALTIME_BASE_URL` |
| `chatgpt.client_version` | `OAI-Client-Version` header on `/realtime/wm`. | `DEFAULT_REALTIME_CLIENT_VERSION` in `signaling/vendor/loginwithchatgpt-core/src/constants.ts` |
| `chatgpt.client_build` | `OAI-Client-Build-Number` header. | `DEFAULT_REALTIME_CLIENT_BUILD` |
| `chatgpt.web_client_id` | Expected `client_id` claim of the access token from `/api/auth/session` (sanity check via `sc_jwt_get_string_claim`). | `DEFAULT_REALTIME_WEB_CLIENT_ID` |
| `chatgpt.user_agent` | `User-Agent` for every chatgpt.com request (its WAF challenges non-browser clients). | `BROWSER_UA` in `signaling/src/chatgpt.ts` |
| `chatgpt.session_overrides` | Extra/changed fields for the multipart `session` JSON, copied from a real chatgpt.com `/realtime/wm` request (docs/phase0.md step 4). | |
| `policy.idle_close_s` | Hang up after this many seconds without speech (0 = never). | `sc_fsm_config_t.idle_close_s` |
| `policy.rotate_before_cap_s` | Reconnect when the session has less than this left of its ~2 h cap. | `.rotate_before_cap_s` |
| `policy.daily_refresh_s` | Re-auth this often even when idle, so the rotated session cookie keeps rolling. | `.daily_refresh_s` |
| `policy.token_refresh_margin_s` | Re-auth when the access token expires within this window. | `.token_refresh_margin_s` |

When chatgpt.com's web client moves on (calls start failing with 4xx), update `client_version` /
`client_build` from a real browser request (DevTools → Network → `POST /realtime/wm` → request
headers `OAI-Client-Version` / `OAI-Client-Build-Number`), keep `version` at 1, and push.
