#!/usr/bin/env python3
"""Renew only the speaker WireGuard UDP mapping; run every 300s with launchd."""
import ipaddress
import re
import subprocess
import sys
import time

import json
import os
from pathlib import Path

UPNPC = "/opt/homebrew/bin/upnpc"
PORT = 51820
DESCRIPTION = "gls-wireguard"
LEASE = 3600
# Site-specific settings live next to this script in upnp.json (not in git):
#   {"interface": "en1", "description_url": "http://<router>:<port>/<path>.xml"}
# A direct description URL avoids macOS blocking multicast discovery for launch agents;
# find it with `upnpc -m <interface> -l`. Environment variables override the file.
_CONF_FILE = Path(__file__).resolve().parent / "upnp.json"
_CONF = json.loads(_CONF_FILE.read_text()) if _CONF_FILE.is_file() else {}
INTERFACE = os.environ.get("GLS_UPNP_INTERFACE", _CONF.get("interface", "en0"))
DESCRIPTION_URL = os.environ.get("GLS_UPNP_DESCRIPTION_URL", _CONF.get("description_url", ""))


def run(args):
    p = subprocess.run(args, capture_output=True, text=True, timeout=45)
    if p.returncode:
        raise RuntimeError(p.stdout.strip() + " " + p.stderr.strip())
    return p.stdout


def renew():
    if not DESCRIPTION_URL:
        raise RuntimeError(f"No router description URL: create {_CONF_FILE} (see relay/README.md)")
    address = run(["/usr/sbin/ipconfig", "getifaddr", INTERFACE]).strip()
    ipaddress.IPv4Address(address)
    listing = run([UPNPC, "-u", DESCRIPTION_URL, "-l"])
    # Do not replace a mapping created by another application, even if its port matches.
    for line in listing.splitlines():
        m = re.match(r"\s*\d+\s+UDP\s+51820->([^: ]+):(\d+)\s+'([^']*)'", line)
        if m and (m[1], m[2], m[3]) != (address, str(PORT), DESCRIPTION):
            raise RuntimeError("UDP 51820 is already mapped to a different target; left unchanged")
    output = run([UPNPC, "-u", DESCRIPTION_URL, "-e", DESCRIPTION, "-a",
                  address, str(PORT), str(PORT), "UDP", str(LEASE)])
    # Some miniupnpc failures return zero; require its confirmed mapping response.
    if not re.search(r"external .*:51820 UDP is redirected to internal "
                     + re.escape(address) + r":51820\b", output):
        raise RuntimeError("Router did not confirm the UDP mapping: " + output.strip())
    print(time.strftime("%Y-%m-%d %H:%M:%S"),
          f"UDP {PORT} -> {address}:{PORT} renewed (requested lease {LEASE}s)", flush=True)


if __name__ == "__main__":
    try:
        renew()
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as exc:
        print(time.strftime("%Y-%m-%d %H:%M:%S"), "mapping renewal failed:", exc,
              file=sys.stderr, flush=True)
        sys.exit(1)
