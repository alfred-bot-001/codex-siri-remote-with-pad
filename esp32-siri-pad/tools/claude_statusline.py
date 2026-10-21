"""Claude Code statusLine hook: save only the seven-day remaining percentage.

Claude Code supplies one JSON document on stdin. This hook deliberately does not
save its conversation, paths, tokens or credentials.
"""
import json
import os
from pathlib import Path
import sys
import time

CACHE = Path.home() / "Library/Application Support/SiriVoicePad/claude-weekly.json"
HEALTH = CACHE.with_name("claude-statusline-health.json")


def weekly_remaining(document):
    value = document.get("rate_limits", {}).get("seven_day", {}).get("used_percentage")
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not 0 <= value <= 100:
        return None
    return round(100 - value)


def main():
    try:
        document = json.load(sys.stdin)
        weekly = document.get("rate_limits", {}).get("seven_day")
        HEALTH.parent.mkdir(parents=True, exist_ok=True)
        HEALTH.write_text(json.dumps({"seen_at": time.time(),
                                      "has_rate_limits": "rate_limits" in document,
                                      "has_seven_day": isinstance(weekly, dict)}))
        left = weekly_remaining(document)
        if left is None:
            print("Claude Code")
            return
        reset = document.get("rate_limits", {}).get("seven_day", {}).get("resets_at")
        record = {"remaining": left, "received_at": time.time(), "resets_at": reset}
        CACHE.parent.mkdir(parents=True, exist_ok=True)
        temp = CACHE.with_suffix(".tmp")
        temp.write_text(json.dumps(record, ensure_ascii=False))
        os.replace(temp, CACHE)
        print(f"Claude Code · 每周剩余 {left}%")
    except (OSError, ValueError, TypeError, KeyError):
        print("Claude Code")


if __name__ == "__main__":
    main()
