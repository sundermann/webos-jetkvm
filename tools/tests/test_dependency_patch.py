"""Dependency source fixes must apply once, survive reconfigure and reject drift."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class PatchTests(unittest.TestCase):
    def test_ignored_subdirectory(self):
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory)
            subprocess.run(["git", "init", "-q", str(parent)], check=True)
            (parent / ".gitignore").write_text("build/\n")
            root = parent / "build/sdk-include"
            root.mkdir(parents=True)
            source = root / "source.c"
            source.write_text("old\n")
            patch = parent / "fix.patch"
            patch.write_text(
                "diff --git a/source.c b/source.c\n--- a/source.c\n+++ b/source.c\n"
                "@@ -1 +1 @@\n-old\n+fixed\n"
            )
            command = [
                "cmake",
                f"-DSOURCE_DIR={root}",
                f"-DPATCH_FILE={patch}",
                "-P",
                str(ROOT / "cmake/apply_source_patch.cmake"),
            ]
            for _ in range(2):
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(source.read_text(), "fixed\n")

    def test_idempotence_and_drift(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            source = root / "source.c"
            source.write_text("old\n")
            patch = root / "fix.patch"
            patch.write_text(
                "diff --git a/source.c b/source.c\n--- a/source.c\n+++ b/source.c\n"
                "@@ -1 +1 @@\n-old\n+fixed\n"
            )
            command = [
                "cmake",
                f"-DSOURCE_DIR={root}",
                f"-DPATCH_FILE={patch}",
                "-P",
                str(ROOT / "cmake/apply_source_patch.cmake"),
            ]
            for _ in range(2):
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(source.read_text(), "fixed\n")
            source.write_text("unexpected upstream change\n")
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(source.read_text(), "unexpected upstream change\n")


if __name__ == "__main__":
    unittest.main()
