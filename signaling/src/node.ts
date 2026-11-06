// Node entry point (Node 24+, runs the .ts files directly). This is the one that
// works: chatgpt.com accepts Node's fetch from a home connection.
//
//   node src/node.ts            → http://localhost:8787
//   HOST=0.0.0.0 node src/node.ts  (to reach it from the ESP32 on the LAN later)

import { createServer } from "node:http";
import { mkdir, readFile, rename, writeFile } from "node:fs/promises";
import { homedir } from "node:os";
import { dirname, extname, join, normalize } from "node:path";
import { fileURLToPath } from "node:url";
import { handleApi } from "./app.ts";
import type { Env, Store } from "./chatgpt.ts";

const root = dirname(dirname(fileURLToPath(import.meta.url)));
const publicDir = join(root, "public");
const stateFile = process.env.STATE_FILE ?? join(homedir(), ".gpt-live-speaker", "state.json");

/** Same .dev.vars file wrangler reads, so both runtimes share one secrets file. */
async function loadDevVars(): Promise<Record<string, string>> {
  const text = await readFile(join(root, ".dev.vars"), "utf8").catch(() => "");
  const vars: Record<string, string> = {};
  for (const line of text.split("\n")) {
    const m = line.match(/^\s*([A-Z0-9_]+)\s*=\s*(.*)\s*$/);
    if (m) vars[m[1]!] = m[2]!.replace(/^"(.*)"$/, "$1");
  }
  return vars;
}

/** A tiny JSON-file store with Workers-KV-like TTLs. Values are already encrypted where it matters. */
function fileStore(path: string): Store {
  type Entry = { value: string; expiresAt?: number };
  let cache: Record<string, Entry> | undefined;
  let writing = Promise.resolve();
  const load = async () => {
    cache ??= JSON.parse(await readFile(path, "utf8").catch(() => "{}")) as Record<string, Entry>;
    return cache;
  };
  const persist = () => {
    writing = writing.then(async () => {
      await mkdir(dirname(path), { recursive: true, mode: 0o700 });
      await writeFile(`${path}.tmp`, JSON.stringify(cache, null, 2), { mode: 0o600 });
      await rename(`${path}.tmp`, path);
    });
    return writing;
  };
  return {
    async get(key) {
      const entry = (await load())[key];
      if (!entry) return null;
      if (entry.expiresAt && entry.expiresAt < Date.now()) return null;
      return entry.value;
    },
    async put(key, value, options) {
      (await load())[key] = { value, expiresAt: options?.expirationTtl ? Date.now() + options.expirationTtl * 1000 : undefined };
      await persist();
    },
    async delete(key) {
      delete (await load())[key];
      await persist();
    },
  };
}

const MIME: Record<string, string> = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
};

const vars = { ...(await loadDevVars()), ...process.env } as Record<string, string | undefined>;
const env: Env = {
  STATE: fileStore(stateFile),
  ADMIN_TOKEN: vars.ADMIN_TOKEN ?? "",
  ENCRYPTION_KEY: vars.ENCRYPTION_KEY ?? "",
  CHATGPT_CLIENT_VERSION: vars.CHATGPT_CLIENT_VERSION,
  CHATGPT_CLIENT_BUILD: vars.CHATGPT_CLIENT_BUILD,
};

const port = Number(process.env.PORT ?? 8787);
const host = process.env.HOST ?? "127.0.0.1";

createServer(async (req, res) => {
  try {
    const url = new URL(req.url ?? "/", `http://${req.headers.host ?? "localhost"}`);
    if (url.pathname.startsWith("/api/")) {
      const chunks: Buffer[] = [];
      for await (const chunk of req) chunks.push(chunk as Buffer);
      const request = new Request(url, {
        method: req.method,
        headers: Object.entries(req.headers).flatMap(([k, v]) => (v === undefined ? [] : [[k, String(v)]])) as [string, string][],
        body: req.method === "GET" || req.method === "HEAD" ? undefined : Buffer.concat(chunks),
      });
      const started = Date.now();
      const response = await handleApi(request, env);
      res.writeHead(response.status, Object.fromEntries(response.headers));
      res.end(Buffer.from(await response.arrayBuffer()));
      console.log(`${req.method} ${url.pathname} → ${response.status} (${Date.now() - started} ms)`);
      return;
    }
    const rel = normalize(url.pathname === "/" ? "/index.html" : url.pathname).replace(/^(\.\.[/\\])+/, "");
    const body = await readFile(join(publicDir, rel)).catch(() => undefined);
    if (!body) { res.writeHead(404).end("not found"); return; }
    res.writeHead(200, { "content-type": MIME[extname(rel)] ?? "application/octet-stream", "cache-control": "no-store" });
    res.end(body);
  } catch (error) {
    console.error(error);
    res.writeHead(500).end("internal error");
  }
}).listen(port, host, () => {
  console.log(`gpt-live-speaker signaling on http://${host === "0.0.0.0" ? "localhost" : host}:${port}  (state: ${stateFile})`);
  if (!env.ADMIN_TOKEN || !env.ENCRYPTION_KEY) console.warn("ADMIN_TOKEN / ENCRYPTION_KEY missing — see README.");
});
