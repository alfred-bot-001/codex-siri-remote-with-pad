"""Install this Mac's weekly-quota bridge and Claude Code statusLine hook.

Only the statusLine field is added to Claude's user settings. A dated backup is
saved before that file changes. The LaunchAgent survives login and USB changes.
"""
import json
import os
from pathlib import Path
import plistlib
import shlex
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
PYTHON = ROOT / ".venv/bin/python"
SETTINGS = Path.home() / ".claude/settings.json"
LABEL = "local.siri-voice-pad.weekly-quota"
AGENT = Path.home() / "Library/LaunchAgents" / (LABEL + ".plist")
LOGS = Path.home() / "Library/Logs/SiriVoicePad"


def install_statusline():
    settings = json.loads(SETTINGS.read_text()) if SETTINGS.exists() else {}
    if settings.get("statusLine"):
        raise SystemExit("An existing Claude statusLine is configured; integrate the cache hook manually.")
    command = f"/usr/bin/python3 {shlex.quote(str(ROOT / 'tools/claude_statusline.py'))}"
    settings["statusLine"] = {"type": "command", "command": command}
    SETTINGS.parent.mkdir(parents=True, exist_ok=True)
    if SETTINGS.exists():
        backup = SETTINGS.with_name(SETTINGS.name + f".before-siri-pad-{int(time.time())}")
        shutil.copy2(SETTINGS, backup)
        print(f"Claude settings backup: {backup}")
    temp = SETTINGS.with_suffix(".tmp")
    temp.write_text(json.dumps(settings, ensure_ascii=False, indent=2) + "\n")
    os.replace(temp, SETTINGS)


def install_agent():
    if not PYTHON.is_file():
        raise SystemExit("Create esp32-siri-pad/.venv and install pyserial first.")
    LOGS.mkdir(parents=True, exist_ok=True)
    AGENT.parent.mkdir(parents=True, exist_ok=True)
    definition = {
        "Label": LABEL,
        "ProgramArguments": [str(PYTHON), str(ROOT / "tools/weekly_quota_bridge.py")],
        "WorkingDirectory": str(ROOT),
        "RunAtLoad": True,
        "KeepAlive": True,
        "ThrottleInterval": 10,
        "StandardOutPath": str(LOGS / "weekly-quota.log"),
        "StandardErrorPath": str(LOGS / "weekly-quota.err.log"),
    }
    AGENT.write_bytes(plistlib.dumps(definition))
    subprocess.run(["launchctl", "bootstrap", f"gui/{os.getuid()}", str(AGENT)], check=True)
    print(f"LaunchAgent installed: {AGENT}")


if __name__ == "__main__":
    install_statusline()
    install_agent()
