"""Offline checks for weekly-window selection and stale Claude data."""
import importlib.util
import json
from pathlib import Path
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1] / "tools"


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


bridge = load("weekly_quota_bridge")
hook = load("claude_statusline")


class WeeklyQuotaTest(unittest.TestCase):
    def test_codex_primary_can_be_weekly(self):
        result = {"rateLimitsByLimitId": {"codex": {
            "primary": {"windowDurationMins": 10080, "usedPercent": 42},
            "secondary": {"windowDurationMins": 300, "usedPercent": 5}}}}
        self.assertEqual(bridge.codex_weekly_remaining(result), 58)

    def test_codex_secondary_can_be_weekly(self):
        result = {"rateLimits": {
            "primary": {"windowDurationMins": 300, "usedPercent": 5},
            "secondary": {"windowDurationMins": 10080, "usedPercent": 37}}}
        self.assertEqual(bridge.codex_weekly_remaining(result), 63)

    def test_claude_uses_seven_day_only(self):
        document = {"rate_limits": {"five_hour": {"used_percentage": 19},
                                    "seven_day": {"used_percentage": 7}}}
        self.assertEqual(hook.weekly_remaining(document), 93)

    def test_claude_stale_cache_is_unknown(self):
        with tempfile.TemporaryDirectory() as folder:
            bridge.CACHE = Path(folder) / "claude.json"
            now = time.time()
            bridge.CACHE.write_text(json.dumps({"remaining": 93, "received_at": now - 3700}))
            self.assertEqual(bridge.read_claude_cache(now), -1)
            bridge.CACHE.write_text(json.dumps({"remaining": 93, "received_at": now,
                                                "resets_at": now - 1}))
            self.assertEqual(bridge.read_claude_cache(now), -1)


if __name__ == "__main__":
    unittest.main()
