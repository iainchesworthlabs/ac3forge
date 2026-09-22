import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from append_quality_history import write_recent_window


class RecentHistoryTests(unittest.TestCase):
    def test_writes_whole_recent_commits_for_any_history_stem(self):
        with tempfile.TemporaryDirectory() as tmp:
            history = Path(tmp) / "performance-main.jsonl"
            rows = [
                {"commit": f"c{commit}", "series": series}
                for commit in range(5)
                for series in ("a", "b")
            ]
            history.write_text("".join(json.dumps(row) + "\n" for row in rows))

            with patch("append_quality_history.RECENT_WINDOW_COMMITS", 2):
                write_recent_window(history)

            recent = history.with_suffix(".recent.jsonl")
            kept = [json.loads(line) for line in recent.read_text().splitlines()]
            self.assertEqual({"c3", "c4"}, {row["commit"] for row in kept})
            self.assertEqual(4, len(kept))

    def test_falls_back_to_full_history_inside_window(self):
        with tempfile.TemporaryDirectory() as tmp:
            history = Path(tmp) / "memory-main.jsonl"
            recent = history.with_suffix(".recent.jsonl")
            history.write_text('{"commit":"new"}\n')
            recent.write_text('{"commit":"stale"}\n')

            write_recent_window(history)

            self.assertFalse(recent.exists())

    def test_ignores_malformed_rows_only_in_derived_window(self):
        with tempfile.TemporaryDirectory() as tmp:
            history = Path(tmp) / "kernels-main.jsonl"
            history.write_text(
                '{"commit":"old","series":"a"}\n'
                "not json\n"
                '{"series":"missing commit"}\n'
                '{"commit":"new","series":"a"}\n'
            )

            with patch("append_quality_history.RECENT_WINDOW_COMMITS", 1):
                write_recent_window(history)

            self.assertEqual(
                ['{"commit":"new","series":"a"}'],
                history.with_suffix(".recent.jsonl").read_text().splitlines(),
            )
            self.assertIn("not json", history.read_text())


if __name__ == "__main__":
    unittest.main()
