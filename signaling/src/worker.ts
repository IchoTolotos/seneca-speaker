// Cloudflare Worker entry point. Kept for the later public front door (pairing,
// device auth, routing to the home server). Calling chatgpt.com from here is
// currently blocked by OpenAI's WAF, so /api/realtime only works under node.ts.

import { handleApi } from "./app.ts";
import type { Env } from "./chatgpt.ts";

interface WorkerEnv extends Omit<Env, "STATE"> {
  STATE: KVNamespace;
  ASSETS: Fetcher;
}

export default {
  async fetch(request, env): Promise<Response> {
    const url = new URL(request.url);
    if (!url.pathname.startsWith("/api/")) return env.ASSETS.fetch(request);
    return handleApi(request, env);
  },
} satisfies ExportedHandler<WorkerEnv>;
