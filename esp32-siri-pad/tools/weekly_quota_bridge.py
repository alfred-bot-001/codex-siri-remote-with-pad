"""Send live Codex and Claude Code weekly limits to Siri Voice Pad over USB CDC.

The keyboard and microphone remain standard USB devices. This host helper sends
only two numbers over the Pad's existing diagnostics interface: Q <codex> <claude>.
Unknown or stale values are -1 and render as -- on the Pad.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import threading
import time

from serial import Serial, SerialException
from serial.tools import list_ports

SERIAL = "1020BA4658B4-PAD1"
CACHE = Path.home() / "Library/Application Support/SiriVoicePad/claude-weekly.json"
CODEX_CLI = "/Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex"


def codex_weekly_remaining(result):
    by_id = result.get("rateLimitsByLimitId") or {}
    bucket = by_id.get("codex") or result.get("rateLimits") or {}
    for name in ("primary", "secondary"):
        window = bucket.get(name) or {}
        if window.get("windowDurationMins") != 10080:
            continue
        used = window.get("usedPercent")
        if isinstance(used, bool) or not isinstance(used, (int, float)) or not 0 <= used <= 100:
            continue
        return round(100 - used)
    return -1


def read_codex(codex_path):
    process = subprocess.Popen([codex_path, "app-server"], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                               text=True, bufsize=1)
    lines = queue.Queue()
    reader = threading.Thread(target=lambda: [lines.put(line) for line in process.stdout], daemon=True)
    reader.start()
    requests = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "clientInfo": {"name": "siri-voice-pad", "version": "1.0"}, "capabilities": {}}},
        {"jsonrpc": "2.0", "method": "initialized"},
        {"jsonrpc": "2.0", "id": 2, "method": "account/rateLimits/read"},
    ]
    try:
        for request in requests:
            process.stdin.write(json.dumps(request) + "\n")
        process.stdin.flush()
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                response = json.loads(lines.get(timeout=min(1, deadline - time.monotonic())))
            except queue.Empty:
                continue
            if response.get("id") == 2:
                return codex_weekly_remaining(response.get("result") or {})
        return -1
    finally:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()


def read_claude_cache(now=None):
    now = time.time() if now is None else now
    try:
        record = json.loads(CACHE.read_text())
        left = record["remaining"]
        received = record["received_at"]
        if isinstance(left, bool) or not isinstance(left, int) or not 0 <= left <= 100:
            return -1
        if not isinstance(received, (int, float)) or not 0 <= now - received < 3600:
            return -1
        reset = record.get("resets_at")
        if isinstance(reset, (int, float)) and now >= reset:
            return -1
        if isinstance(reset, str) and now >= datetime.fromisoformat(reset.replace("Z", "+00:00")).timestamp():
            return -1
        return left
    except (OSError, ValueError, TypeError, KeyError):
        return -1


def find_pad_port():
    for port in list_ports.comports():
        if port.vid == 0xCAFE and port.pid == 0x4015 and port.serial_number == SERIAL:
            return port.device
    return None


def run(codex_path, port_override, interval):
    connection = None
    current_port = None
    codex = -1
    next_codex = 0
    last_sent = (-2, -2)
    last_send_at = 0
    while True:
        now = time.monotonic()
        if now >= next_codex:
            try:
                codex = read_codex(codex_path)
            except (OSError, ValueError, KeyError) as exc:
                codex = -1
                print(f"Codex rate-limit read failed: {exc}", file=sys.stderr, flush=True)
            next_codex = now + interval
        claude = read_claude_cache()
        if connection is None:
            port = port_override or find_pad_port()
            if port:
                try:
                    connection = Serial(port=None, baudrate=115200, timeout=0, write_timeout=2)
                    connection.dtr = False
                    connection.rts = False
                    connection.port = port
                    connection.open()
                    connection.dtr = True
                    current_port = port
                    last_sent = (-2, -2)
                    print(f"Pad connected: {port}", flush=True)
                except (OSError, SerialException):
                    connection = None
        if connection:
            try:
                connection.read_all()  # Drain STAT/log diagnostics so CDC never backs up.
                values = (codex, claude)
                if values != last_sent or now - last_send_at >= 60:
                    connection.write(f"Q {codex} {claude}\n".encode("ascii"))
                    connection.flush()
                    last_sent, last_send_at = values, now
                    print(f"Weekly remaining: Codex {codex}, Claude {claude}", flush=True)
            except (OSError, SerialException):
                connection.close()
                connection = None
                print(f"Pad disconnected: {current_port}", flush=True)
        time.sleep(5)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", help="Explicit Pad CDC port; default discovers the verified device")
    parser.add_argument("--codex", default=CODEX_CLI, help="Codex CLI executable")
    parser.add_argument("--interval", type=int, default=120, help="Codex query interval in seconds")
    args = parser.parse_args()
    if not Path(args.codex).is_file() and not shutil.which(args.codex):
        parser.error("Codex CLI not found")
    run(args.codex, args.port, max(args.interval, 30))


if __name__ == "__main__":
    main()
