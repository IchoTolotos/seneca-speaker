#!/usr/bin/env python3
"""Load the private relay token without placing it in process arguments."""
import os
from pathlib import Path
import sys

directory = Path(__file__).resolve().parent
os.environ["RELAY_TOKEN"] = (directory / "token").read_text().strip()
os.execv(sys.executable, [sys.executable, str(directory / "relay.py"),
                        "--host", "127.0.0.1", "--port", "8790"])
