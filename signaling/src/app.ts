// Signaling API: stores the ChatGPT web session and swaps SDP offers for
// GPT-Live answers. Audio never passes through here. Runtime-agnostic; see
// node.ts (works against chatgpt.com) and worker.ts (blocked by OpenAI's WAF).

import { ChatGPTAuthError } from "../vendor/loginwithchatgpt-core/src/errors.ts";
import { parseChatGPTRealtimeSessionOptions } from "../vendor/loginwithchatgpt-core/src/realtime.ts";
import { safeEqual } from "./crypto.ts";
import {
  KEY_LAST_SIGNAL,
  KEY_OVERRIDES,
  KEY_ROTATION_LOG,
  SignalError,
  clearWebSession,
  createCall,
  getAuth,
  saveWebSession,
  type Env,
} from "./chatgpt.ts";

const json = (body: unknown, status = 200) =>
  new Response(JSON.stringify(body, null, 2), {
    status,
    headers: { "content-type": "application/json", "cache-control": "no-store" },
  });

function authorized(request: Request, env: Env): boolean {
  const header = request.headers.get("authorization") ?? "";
  const token = header.startsWith("Bearer ") ? header.slice(7) : "";
  return Boolean(env.ADMIN_TOKEN) && safeEqual(token, env.ADMIN_TOKEN);
}

function errorResponse(error: unknown): Response {
  if (error instanceof SignalError) {
    return json({ error: error.code, message: error.message, detail: error.detail }, error.status);
  }
  if (error instanceof ChatGPTAuthError) {
    return json({ error: error.code, message: error.message }, error.status ?? 502);
  }
  if (error instanceof TypeError) return json({ error: "bad_request", message: error.message }, 400);
  console.error(error);
  return json({ error: "internal", message: String(error) }, 500);
}

async function egress(): Promise<Record<string, string>> {
  // Where do our outbound requests to chatgpt.com come from? (Mac IP under `wrangler dev`, Cloudflare when deployed.)
  const text = await (await fetch("https://cloudflare.com/cdn-cgi/trace")).text();
  const out: Record<string, string> = {};
  for (const line of text.split("\n")) {
    const [k, v] = line.split("=");
    if (k && v && ["ip", "colo", "loc", "warp"].includes(k)) out[k] = v;
  }
  return out;
}

/** Handles `/api/*`. Static files are served by the runtime entry point. */
export async function handleApi(request: Request, env: Env): Promise<Response> {
  const url = new URL(request.url);
  if (!env.ADMIN_TOKEN || !env.ENCRYPTION_KEY) {
    return json({ error: "misconfigured", message: "Set ADMIN_TOKEN and ENCRYPTION_KEY (see README)." }, 500);
  }
  if (!authorized(request, env)) return json({ error: "unauthorized" }, 401);

  try {
    const route = `${request.method} ${url.pathname}`;
    switch (route) {
      case "GET /api/status": {
        const [lastSignal, overrides, out, rotations] = await Promise.all([
          env.STATE.get(KEY_LAST_SIGNAL).then((v) => (v ? JSON.parse(v) : null)),
          env.STATE.get(KEY_OVERRIDES).then((v) => (v ? JSON.parse(v) : {})),
          egress().catch((e) => ({ error: String(e) })),
          env.STATE.get(KEY_ROTATION_LOG).then((v) => (v ? JSON.parse(v) : [])),
        ]);
        let chatgpt: unknown;
        try {
          const auth = await getAuth(env, { force: url.searchParams.has("refresh") });
          chatgpt = { ok: true, email: auth.email, accountId: auth.accountId, accessTokenExpires: new Date(auth.exp * 1000).toISOString(), webSessionExpires: auth.expiresAt };
        } catch (e) {
          const r = errorResponse(e);
          chatgpt = { ok: false, status: r.status, ...(await r.json() as object) };
        }
        return json({ chatgpt, egress: out, lastSignal, overrides, rotations });
      }
      case "POST /api/setup": {
        const { sessionToken } = await request.json() as { sessionToken?: string };
        if (!sessionToken || sessionToken.length < 100) throw new TypeError("Paste the full __Secure-next-auth.session-token value.");
        await saveWebSession(env, sessionToken);
        const auth = await getAuth(env, { force: true });
        return json({ ok: true, email: auth.email });
      }
      case "DELETE /api/setup":
        await clearWebSession(env);
        return json({ ok: true });
      case "PUT /api/overrides": {
        const body = await request.json();
        if (typeof body !== "object" || body === null || Array.isArray(body)) throw new TypeError("Overrides must be a JSON object.");
        await env.STATE.put(KEY_OVERRIDES, JSON.stringify(body));
        return json({ ok: true, overrides: body });
      }
      case "POST /api/realtime": {
        const { sdp, session } = await request.json() as { sdp?: string; session?: unknown };
        const options = parseChatGPTRealtimeSessionOptions(session ?? {});
        const result = await createCall(env, sdp ?? "", options);
        return json({ sdp: result.answer, sentSession: result.sentSession });
      }
      default:
        return json({ error: "not_found" }, 404);
    }
  } catch (error) {
    return errorResponse(error);
  }
}
