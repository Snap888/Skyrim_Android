#!/usr/bin/env python3
"""Regression checks for Skyrim's Android Win32-to-host path mapper."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "runtime/kernel32.c"


class RuntimePathNormalizationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = SOURCE.read_text(encoding="utf-8")

    def test_duplicate_data_prefix_is_retried_only_for_existing_game_assets(self):
        self.assertIn('root == g_game_dir && !strncasecmp(rel, "DATA\\\\DATA\\\\", 10)', self.text)
        self.assertIn('snprintf(alternate, sizeof alternate, "%s/%s", root, rel + 5);', self.text)
        self.assertIn("if (resolve_ci(alternate))", self.text)
        self.assertIn('normalized duplicate DATA prefix %s -> %s', self.text)

    def test_unresolved_assets_are_not_rewritten(self):
        block = self.text.split('if (!found && root == g_game_dir', 1)[1].split(
            'if (strcasestr(tmp, "\\\\sound\\\\voice\\\\")', 1
        )[0]
        self.assertIn("if (resolve_ci(alternate))", block)
        self.assertIn("found = 1;", block)
        self.assertNotIn("else", block)
        self.assertIn("never rewrite a real", block)


if __name__ == "__main__":
    unittest.main()
