#!/usr/bin/env python3
"""Thin HTTP relay for the speaker's two chatgpt.com calls.

chatgpt.com's Cloudflare edge challenges the ESP32's own TLS/TCP client (measured: same public IP,
same headers, the Mac passes and the ESP32 gets 403 cf-mitigated every time). This relay makes the
same requests from a regular OpenSSL client. The speaker keeps all logic and state (cookie jar,
rotation, tokens); the relay stores nothing and only forwards:

  GET  /<token>/api/auth/session   -> https://chatgpt.com/api/auth/session
  POST /<token>/realtime/wm?...    -> https://chatgpt.com/realtime/wm?...
  GET  /<token>/cdn-cgi/trace      -> https://chatgpt.com/cdn-cgi/trace   (probe)

Audio/WebRTC never passes through here. Anything else is 404, so it is not an open proxy.
The token is a path prefix, so the firmware only needs CONFIG_GLS_CHATGPT_BASE_URL changed.

Logs: time, method, path (without token), status, cf-mitigated, sizes. Never headers or bodies.

Usage: RELAY_TOKEN=... python3 relay.py [--host 127.0.0.1] [--port 8790]
"""
import argparse
import hmac
import http.client
import os
import ssl
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

UPSTREAM = "chatgpt.com"
ROUTES = {("GET", "/api/auth/session"), ("POST", "/realtime/wm"), ("GET", "/cdn-cgi/trace")}
# Request headers passed through (everything else, e.g. Host/Origin/Referer/Connection, is set here).
FORWARD = {"accept", "accept-language", "authorization", "content-type", "cookie", "user-agent",
           "oai-device-id", "oai-language", "oai-client-version", "oai-client-build-number",
           "oai-session-id", "x-openai-target-path", "x-openai-target-route", "chatgpt-account-id"}
# Response headers passed back (Set-Cookie may repeat).
RETURN = {"content-type", "set-cookie", "cf-mitigated", "cf-ray", "server", "date", "location"}
MAX_BODY = 256 * 1024
TOKEN = os.environ.get("RELAY_TOKEN", "")
CTX = ssl.create_default_context()


def log(msg):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), msg, flush=True)


class Relay(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "gls-relay"

    def log_message(self, *args):  # silence default access log (it would include the token)
        pass

    def _reply(self, status, body=b"", ctype="text/plain"):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def _handle(self):
        raw_path, _, query = self.path.partition("?")
        parts = raw_path.split("/", 2)  # ['', token, rest]
        if len(parts) < 3 or not TOKEN or not hmac.compare_digest(parts[1], TOKEN):
            log(f"{self.command} <bad token> 404")
            return self._reply(404, b"not found")
        path = "/" + parts[2]
        if (self.command, path) not in ROUTES:
            log(f"{self.command} {path} 404 (not allowed)")
            return self._reply(404, b"not found")
        n = int(self.headers.get("Content-Length") or 0)
        if n > MAX_BODY:
            return self._reply(413, b"too large")
        body = self.rfile.read(n) if n else None

        headers = {k: v for k, v in self.headers.items() if k.lower() in FORWARD}
        headers["Origin"] = "https://chatgpt.com"
        headers["Referer"] = "https://chatgpt.com/"
        headers["Connection"] = "close"
        t0 = time.time()
        try:
            conn = http.client.HTTPSConnection(UPSTREAM, timeout=30, context=CTX)
            conn.request(self.command, path + ("?" + query if query else ""), body=body, headers=headers)
            resp = conn.getresponse()
            data = resp.read()
            out = [(k, v) for k, v in resp.getheaders() if k.lower() in RETURN]
            conn.close()
        except Exception as e:  # upstream unreachable
            log(f"{self.command} {path} 502 upstream error: {type(e).__name__}")
            return self._reply(502, b"upstream error")

        self.send_response(resp.status)
        for k, v in out:
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)
        cf = resp.getheader("cf-mitigated") or "-"
        log(f"{self.command} {path} {resp.status} cf={cf} in={n}B out={len(data)}B {int((time.time()-t0)*1000)}ms")

    def do_GET(self):
        self._handle()

    def do_POST(self):
        self._handle()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8790)
    a = ap.parse_args()
    if len(TOKEN) < 24:
        sys.exit("RELAY_TOKEN must be set (>= 24 chars)")
    log(f"relay listening on {a.host}:{a.port} -> https://{UPSTREAM}")
    ThreadingHTTPServer((a.host, a.port), Relay).serve_forever()


if __name__ == "__main__":
    main()
