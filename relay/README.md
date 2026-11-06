# Home relay and WireGuard endpoint

The relay runs on an always-on Mac at home (the "relay host"). The ESP32 sends only
signaling/auth HTTP requests through WireGuard. Audio still goes directly
from the ESP32 to the upstream WebRTC peer over Wi-Fi.

```text
ESP32 10.77.0.2 -> <your-ddns-host>:51820/UDP
  -> router UPnP -> relay host wireproxy 10.77.0.1:8790
  -> 127.0.0.1:8790 relay.py -> chatgpt.com
```

`wireproxy` is a userspace network stack. It creates no macOS VPN interface or LAN forwarding.
Its only TCP server tunnel forwards port 8790 to the loopback relay. No SOCKS/HTTP proxy is
configured. The authenticated WireGuard peer is allowed source `10.77.0.2/32` only. The relay
also requires its private URL token and accepts only the routes listed in `relay.py`.

## Installed files and services

The relay host's service directory is `~/Library/Application Support/gpt-live-speaker-relay/` (mode 700).
It contains `relay.py`, `run_relay.py`, `renew_mapping.py`, `install_services.py`, `bin/wireproxy`,
`wireproxy.conf`, `wireguard-private.der`, `token`, `upnp.json`, and `logs/`. Private config/key/token files
are mode 600 and must never be committed. A launch agent cannot reliably read `~/Documents`
because of macOS privacy controls.

Three user launch agents live in `~/Library/LaunchAgents/`. Their labels are
`<prefix>.gls-*`; the prefix defaults to `com.gpt-live-speaker` (set `GLS_LABEL_PREFIX` when
installing to keep an existing one):

| Label | Behavior |
| --- | --- |
| `<prefix>.gls-relay` | Loopback HTTP relay on 8790; restarts after exit |
| `<prefix>.gls-wireproxy` | WireGuard UDP 51820; restarts after exit |
| `<prefix>.gls-upnp` | One-shot mapping renewal at login and every 300 seconds |

These start when the relay host's user logs in (use automatic login). They are user
services, not boot daemons. Homebrew Python and `miniupnpc` must remain installed.

`renew_mapping.py` maps UDP 51820 to the relay host's current IPv4 address on the configured
interface with a requested 3600s lease. It refuses to replace another application's mapping. It
uses the router's direct UPnP description URL because macOS blocks multicast discovery for the
background job. Both are router/host-specific and live in `upnp.json` in the service directory:

```json
{"interface": "en1", "description_url": "http://<router-ip>:<port>/<path>.xml"}
```

Find the URL with `upnpc -m <interface> -l` on the relay host. A router reboot has not been
deliberately tested.

For script updates, copy the files into the service directory and run there:

```sh
/opt/homebrew/bin/python3 install_services.py
```

The installer retains the existing keys, token and WireGuard configuration. It deliberately
refuses to run from a checkout or to start without the private files.

## WireGuard configuration

Installed binary: upstream `windtf/wireproxy` v1.1.3, Darwin arm64 release. Archive SHA-256:
`28d34342c48c309b628d9c06ab4efc05b82fba49821f360605c02139c02547a5`.

The private `wireproxy.conf` has this shape:

```ini
[Interface]
Address = 10.77.0.1/32
ListenPort = 51820
PrivateKey = <relay host private key>
MTU = 1280

[Peer]
PublicKey = <speaker public key>
PresharedKey = <shared PSK>
AllowedIPs = 10.77.0.2/32

[TCPServerTunnel]
ListenPort = 8790
Target = 127.0.0.1:8790
```

The speaker's gitignored `firmware/sdkconfig` sets its address to `10.77.0.2`, allowed peer
address to `10.77.0.1`, endpoint to the DDNS name on UDP 51820, the relay host's public key, and base URL
to `http://10.77.0.1:8790/<private relay token>`. Preserve all credentials when editing it.

The firmware reserves **two allowed-address slots** using `CONFIG_WIREGUARD_MAX_SRC_IPS`.
The library otherwise defaults to one and consumes it with the device's own address; adding
the peer address then fails. The wrapper checks that failure, retries asynchronous DNS without
losing the interface, and serializes the raw lwIP API calls onto the TCP/IP thread. Relay HTTP
requests fail closed while the interface is unavailable. Other traffic keeps using Wi-Fi.

## Diagnostics

On the relay host, use `launchctl print gui/$(id -u)/<prefix>.gls-wireproxy` (or `gls-relay`,
`gls-upnp`) and the corresponding files in `logs/`. `gls-upnp` is normally stopped between runs;
its last exit code should be zero. The relay's logs omit credentials, headers and bodies.

wireproxy's diagnostic HTTP server listens only on `127.0.0.1:8791`. **Its `/metrics` output
includes private keys.** Never print, paste, or save the full output. Filter only counters:

```sh
curl -fsS http://127.0.0.1:8791/metrics |
  rg '^(last_handshake_time_sec|tx_bytes|rx_bytes|allowed_ip)='
```

`/readyz` is not a connectivity test here: no `CheckAlive` is configured, so it always returns
an empty object. Confirm an actual relay HTTP request plus a recent handshake instead.

To recover an expired/deleted UPnP mapping immediately:

```sh
launchctl kickstart gui/$(id -u)/<prefix>.gls-upnp
```

## Verification (2026-10-28)

- Upstream release archive checksum matched before installation; server configuration test passed.
- Independent userspace WireGuard client, using a temporary diagnostic peer over the public
  DDNS endpoint, reached `/api/auth/session`: HTTP 200 JSON, with no Cloudflare challenge.
  The temporary peer and its private key were removed afterward.
- Automatic background UPnP renewal succeeded, including recovery after deliberately deleting
  only the speaker's mapping. Refusal to overwrite another application's mapping was checked.
- Relay listener is bound to `127.0.0.1:8790` only.
- Both long-running agents restarted automatically after their processes received SIGTERM.
- With the speaker left running, a second WireGuard service restart at 18:15:10 recovered
  automatically: the server accepted a new speaker handshake at 18:17:33 and a further
  authenticated keepalive at 18:17:58. Recovery took about 2 minutes 23 seconds.
- Corrected ESP32 firmware was flashed after a physical USB/power reconnect. Its handshake
  succeeded, `/cdn-cgi/trace` and the anonymous `/api/auth/session` probe returned HTTP 200,
  and the dummy `/realtime/wm` request returned HTTP 400 JSON (`invalid_offer`), all without
  a Cloudflare challenge. These verify transport and API reachability, not an authenticated call.
- The speaker currently reports `needs_login` because no ChatGPT session is stored. A live
  voice call still requires sign-in.

For a permanent port-forward fallback, forward UDP 51820 to the relay host and reserve its LAN
address. Then unload `gls-upnp`; do not let it manage a manually owned mapping.
