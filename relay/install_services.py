#!/usr/bin/env python3
"""Install/reload the relay host's user agents after copying this directory to Application Support.

Requires private token, wireproxy.conf, upnp.json and bin/wireproxy already provisioned there.
Does not generate or replace credentials. Run as the relay host's logged-in user, without sudo.
Launch-agent labels are <prefix>.gls-<name>; set GLS_LABEL_PREFIX to keep an existing prefix.
"""
import os
from pathlib import Path
import plistlib
import subprocess
import sys

PREFIX = os.environ.get("GLS_LABEL_PREFIX", "com.gpt-live-speaker")


def install():
    directory = Path(__file__).resolve().parent
    expected = Path.home() / "Library/Application Support/gpt-live-speaker-relay"
    if directory != expected:
        sys.exit(f"Copy relay scripts to {expected} before running this installer")
    for filename in ("token", "wireproxy.conf", "bin/wireproxy", "relay.py",
                     "run_relay.py", "renew_mapping.py", "upnp.json"):
        if not (directory / filename).is_file():
            sys.exit(f"Required file missing: {filename}")
    directory.chmod(0o700)
    for filename in ("token", "wireproxy.conf", "upnp.json"):
        (directory / filename).chmod(0o600)
    logs = directory / "logs"
    logs.mkdir(exist_ok=True)
    agents = Path.home() / "Library/LaunchAgents"
    agents.mkdir(exist_ok=True)
    domain = f"gui/{os.getuid()}"
    services = {
        "relay": ([sys.executable, str(directory / "run_relay.py")], {"KeepAlive": True}),
        "wireproxy": ([str(directory / "bin/wireproxy"), "-c", str(directory / "wireproxy.conf"),
                       "-i", "127.0.0.1:8791"], {"KeepAlive": True}),
        "upnp": ([sys.executable, str(directory / "renew_mapping.py")], {"StartInterval": 300}),
    }
    for name, (args, options) in services.items():
        label = f"{PREFIX}.gls-{name}"
        output = str(logs / f"{name}.log")
        plist = {"Label": label, "ProgramArguments": args, "RunAtLoad": True,
                 "ThrottleInterval": 10, "StandardOutPath": output,
                 "StandardErrorPath": output, **options}
        destination = agents / f"{label}.plist"
        destination.write_bytes(plistlib.dumps(plist))
        subprocess.run(["launchctl", "bootout", f"{domain}/{label}"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["launchctl", "bootstrap", domain, str(destination)], check=True)
        print(f"Installed {label}")


if __name__ == "__main__":
    install()
