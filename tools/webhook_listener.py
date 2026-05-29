#!/usr/bin/env python3
"""STYRBAR -> PC action listener.

The ESP32-C6 gateway POSTs button events to this listener:
    POST /event   {"device":"0x1234","event":"BRIGHT"}

Map each event name to a shell command in ACTIONS below. Run:
    python3 tools/webhook_listener.py
Listens on 0.0.0.0:8899 (matches WEBHOOK_URL in main/webhook.c).
"""
import json
import shlex
import subprocess
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = 8899

# event name (as emitted by the firmware) -> shell command to run.
# Edit these to do whatever you want. Empty/missing = ignored.
ACTIONS = {
    "BRIGHT":     "echo BRIGHT pressed",       # e.g. "cd ~/proj && ./deploy.sh"
    "DIM":        "echo DIM pressed",
    "BRIGHT HLD": "",
    "DIM HLD":    "",
    "BRT REL":    "",
    "ARR LEFT":   "echo previous",
    "ARR RIGHT":  "echo next",
    "ARR LEFT HLD":  "",
    "ARR RIGHT HLD": "",
    "ARR REL":    "",
}


def run_action(event: str, device: str):
    cmd = ACTIONS.get(event, "")
    if not cmd:
        print(f"[skip] {device} {event!r} (no action mapped)")
        return
    print(f"[run ] {device} {event!r} -> {cmd}")
    try:
        subprocess.Popen(cmd, shell=True)
    except Exception as e:  # noqa: BLE001
        print(f"[err ] {e}")


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length else b"{}"
        try:
            data = json.loads(raw or b"{}")
        except json.JSONDecodeError:
            data = {}
        event = str(data.get("event", "")).strip()
        device = str(data.get("device", "?"))
        run_action(event, device)
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(b'{"ok":true}')

    def log_message(self, *_):  # silence default access log
        pass


if __name__ == "__main__":
    print(f"STYRBAR listener on http://0.0.0.0:{PORT}/event")
    print("Mapped events:", ", ".join(k for k, v in ACTIONS.items() if v))
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
