// Signaling against ChatGPT's private GPT-Live `/realtime/wm` route, using a
// ChatGPT web session (the `__Secure-next-auth.session-token` cookie).
//
// We use login-with-chatgpt for the token exchange and the default session
// payload, but send the SDP request ourselves so Phase 0 can merge extra
// session fields captured from the real chatgpt.com client.

import {
  CHATGPT_REALTIME_PATHS,
  buildChatGPTRealtimeSession,
  exchangeChatGPTRealtimeWebSession,
  type ChatGPTRealtimeSessionOptions,
} from "../vendor/loginwithchatgpt-core/src/realtime.ts";
import { resolveConfig } from "../vendor/loginwithchatgpt-core/src/config.ts";
import { open, seal } from "./crypto.ts";

/** The subset of Workers KV we use, so the same code runs on Node with a file store. */
export interface Store {
  get(key: string): Promise<string | null>;
  put(key: string, value: string, options?: { expirationTtl?: number }): Promise<void>;
  delete(key: string): Promise<void>;
}

export interface Env {
  STATE: Store;
  ADMIN_TOKEN: string;
  ENCRYPTION_KEY: string;
  /** Optional overrides when ChatGPT's web client version moves on. */
  CHATGPT_CLIENT_VERSION?: string;
  CHATGPT_CLIENT_BUILD?: string;
}

interface StoredWebSession {
  sessionToken: string;
  deviceId?: string;
  sessionCookies?: Record<string, string>;
  email?: string;
  savedAt: string;
}

interface CachedAuth {
  accessToken: string;
  accountId: string;
  deviceId: string;
  email: string;
  exp: number; // unix seconds
}

const KEY_WEB_SESSION = "web_session";
const KEY_AUTH_CACHE = "auth_cache";
export const KEY_OVERRIDES = "session_overrides";
export const KEY_LAST_SIGNAL = "last_signal";
export const KEY_ROTATION_LOG = "rotation_log";

// chatgpt.com's WAF challenges clients that don't look like a browser (curl, local
// workerd) and blocks Cloudflare Workers egress outright (tested 2026-10-24).
// Node's fetch from a residential IP passes. Send a browser User-Agent regardless.
const BROWSER_UA =
  "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15";

const chatgptFetch: typeof fetch = (input, init = {}) => {
  const headers = new Headers(init.headers);
  if (!headers.has("user-agent")) headers.set("user-agent", BROWSER_UA);
  if (!headers.has("accept-language")) headers.set("accept-language", "de-DE,de;q=0.9,en;q=0.8");
  return fetch(input, { ...init, headers });
};

function config(env: Env) {
  return resolveConfig({
    fetch: chatgptFetch,
    realtimeClientVersion: env.CHATGPT_CLIENT_VERSION || undefined,
    realtimeClientBuild: env.CHATGPT_CLIENT_BUILD || undefined,
  });
}

/** Accepts the raw cookie value, or a JSON map of NextAuth cookie chunks. */
export async function saveWebSession(env: Env, input: string): Promise<void> {
  const raw = input.trim();
  const value: StoredWebSession = { sessionToken: raw, savedAt: new Date().toISOString() };
  if (raw.startsWith("{")) {
    const chunks = JSON.parse(raw) as Record<string, string>;
    value.sessionCookies = chunks;
    value.sessionToken = Object.keys(chunks).sort().map((k) => chunks[k]).join("");
  }
  await env.STATE.put(KEY_WEB_SESSION, await seal(env.ENCRYPTION_KEY, value));
  await env.STATE.delete(KEY_AUTH_CACHE);
}

export async function clearWebSession(env: Env): Promise<void> {
  await env.STATE.delete(KEY_WEB_SESSION);
  await env.STATE.delete(KEY_AUTH_CACHE);
}

function jwtExp(token: string): number {
  try {
    const part = token.split(".")[1]!.replace(/-/g, "+").replace(/_/g, "/");
    const json = JSON.parse(atob(part.padEnd(Math.ceil(part.length / 4) * 4, "="))) as { exp?: number };
    return json.exp ?? 0;
  } catch {
    return 0;
  }
}

/** Returns a cached web-client access token, or mints one from the stored web session. */
export async function getAuth(env: Env, { force = false } = {}): Promise<CachedAuth & { expiresAt?: string }> {
  if (!force) {
    const cached = await env.STATE.get(KEY_AUTH_CACHE);
    if (cached) {
      const auth = await open<CachedAuth>(env.ENCRYPTION_KEY, cached);
      if (auth.exp - Date.now() / 1000 > 300) return auth;
    }
  }
  const sealed = await env.STATE.get(KEY_WEB_SESSION);
  if (!sealed) throw new SignalError(412, "no_web_session", "No ChatGPT web session stored yet. Paste it on the setup page.");
  const stored = await open<StoredWebSession>(env.ENCRYPTION_KEY, sealed);

  const live = await exchangeChatGPTRealtimeWebSession({
    config: config(env),
    sessionToken: stored.sessionToken,
    deviceId: stored.deviceId,
    sessionCookies: stored.sessionCookies,
    onSessionUpdate: async (next: { deviceId: string; sessionCookies: Record<string, string> }) => {
      // ChatGPT may rotate the session cookie on every exchange; persist it or we lose the login.
      const updated: StoredWebSession = { ...stored, ...next };
      await env.STATE.put(KEY_WEB_SESSION, await seal(env.ENCRYPTION_KEY, updated));
      // Track (without secrets) whether ChatGPT actually rolls the cookie forward, to learn how
      // often the device must refresh to stay logged in.
      const before = JSON.stringify(stored.sessionCookies ?? { token: stored.sessionToken });
      const log = JSON.parse((await env.STATE.get(KEY_ROTATION_LOG)) ?? "[]") as unknown[];
      log.push({ at: new Date().toISOString(), rotated: before !== JSON.stringify(next.sessionCookies), chunks: Object.keys(next.sessionCookies).length });
      await env.STATE.put(KEY_ROTATION_LOG, JSON.stringify(log.slice(-50)));
    },
  });
  const auth: CachedAuth = {
    accessToken: live.accessToken,
    accountId: live.accountId ?? "",
    deviceId: live.deviceId,
    email: live.email,
    exp: jwtExp(live.accessToken),
  };
  await env.STATE.put(KEY_AUTH_CACHE, await seal(env.ENCRYPTION_KEY, auth));
  return { ...auth, expiresAt: live.expiresAt };
}

export class SignalError extends Error {
  status: number;
  code: string;
  detail?: unknown;
  constructor(status: number, code: string, message: string, detail?: unknown) {
    super(message);
    this.status = status;
    this.code = code;
    this.detail = detail;
  }
}

export interface SignalResult {
  answer: string;
  sentSession: Record<string, unknown>;
}

/** Posts an SDP offer to ChatGPT `/realtime/wm` and returns the SDP answer. */
export async function createCall(
  env: Env,
  sdp: string,
  options: ChatGPTRealtimeSessionOptions,
): Promise<SignalResult> {
  if (!sdp.trim().startsWith("v=0")) throw new SignalError(400, "invalid_sdp", "Body must contain a WebRTC SDP offer.");
  const cfg = config(env);
  const overrides = JSON.parse((await env.STATE.get(KEY_OVERRIDES)) ?? "{}") as Record<string, unknown>;
  const session = { ...buildChatGPTRealtimeSession({ ...options, transport: "wm" }), ...overrides };
  const path = CHATGPT_REALTIME_PATHS.wm;

  const attempt = async (force: boolean) => {
    const auth = await getAuth(env, { force });
    const form = new FormData();
    form.set("sdp", sdp);
    form.set("session", JSON.stringify(session));
    const headers = new Headers({
      Authorization: `Bearer ${auth.accessToken}`,
      Accept: "application/sdp",
      "OAI-Language": String(session.language_code ?? "en-US"),
      "OAI-Device-Id": auth.deviceId,
      "OAI-Client-Version": cfg.realtimeClientVersion,
      "OAI-Client-Build-Number": cfg.realtimeClientBuild,
      "OAI-Session-Id": crypto.randomUUID(),
      "X-OpenAI-Target-Path": path,
      "X-OpenAI-Target-Route": path,
      Origin: cfg.realtimeBaseUrl,
      Referer: `${cfg.realtimeBaseUrl}/`,
    });
    if (auth.accountId) headers.set("chatgpt-account-id", auth.accountId);
    return chatgptFetch(`${cfg.realtimeBaseUrl}${path}?dcid=0`, { method: "POST", headers, body: form });
  };

  let res = await attempt(false);
  if (res.status === 401) res = await attempt(true); // cached token went stale
  const body = await res.text();

  await env.STATE.put(KEY_LAST_SIGNAL, JSON.stringify({
    at: new Date().toISOString(),
    status: res.status,
    cfRay: res.headers.get("cf-ray"),
    server: res.headers.get("server"),
    contentType: res.headers.get("content-type"),
    bodyPreview: body.trimStart().startsWith("v=0") ? "<sdp answer>" : body.slice(0, 800),
  }), { expirationTtl: 7 * 24 * 3600 });

  if (!res.ok || !body.trimStart().startsWith("v=0")) {
    const edge = res.status === 403 && (res.headers.has("cf-mitigated") || /challenge|blocked/i.test(body));
    throw new SignalError(
      res.status === 401 ? 401 : 502,
      edge ? "chatgpt_edge_challenge" : "realtime_request_failed",
      `ChatGPT /realtime/wm answered ${res.status}.`,
      body.slice(0, 2000),
    );
  }
  return { answer: body, sentSession: session };
}
