"""Regression tests for check_test_wiring.py itself, on a synthetic repo
layout - no dependency on this repo's actual Makefile/tests/ contents."""
from pathlib import Path
import tempfile
import unittest

import check_test_wiring as ctw


class CheckTestWiringTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "tests").mkdir()

    def write_makefile(self, test_targets_body: str):
        (self.root / "Makefile").write_text(
            "CC ?= gcc\n\n"
            "TEST_TARGETS = " + test_targets_body + "\n\n"
            ".PHONY: all test $(TEST_TARGETS)\n"
            "test: $(TEST_TARGETS)\n"
        )

    def write_test_file(self, name: str):
        (self.root / "tests" / f"{name}.c").write_text("int main(void){return 0;}\n")

    def test_fully_wired_passes(self):
        self.write_test_file("test_foo")
        self.write_test_file("test_bar")
        self.write_makefile("test_foo \\\n               test_bar")
        self.assertEqual(ctw.check(self.root), [])

    def test_missing_entry_is_reported(self):
        self.write_test_file("test_foo")
        self.write_test_file("test_forgotten")
        self.write_makefile("test_foo")
        self.assertEqual(ctw.check(self.root), ["test_forgotten"])

    def test_multiple_missing_are_sorted(self):
        self.write_test_file("test_zzz")
        self.write_test_file("test_aaa")
        self.write_makefile("")
        self.assertEqual(ctw.check(self.root), ["test_aaa", "test_zzz"])

    def test_expected_unwired_is_not_reported(self):
        # Uses this script's real EXPECTED_UNWIRED set, not a synthetic one -
        # this is intentionally coupled to it, so renaming/removing an entry
        # there without updating this test (or the file it protects) fails
        # loudly instead of silently losing coverage of the exception list.
        for name in ctw.EXPECTED_UNWIRED:
            self.write_test_file(name)
        self.write_makefile("")
        self.assertEqual(ctw.check(self.root), [])

    def test_wired_via_multiline_continuation(self):
        self.write_test_file("test_a")
        self.write_test_file("test_b")
        self.write_test_file("test_c")
        self.write_makefile("test_a \\\n               test_b \\\n               test_c")
        self.assertEqual(ctw.check(self.root), [])

    def test_no_test_targets_block_raises(self):
        (self.root / "Makefile").write_text("CC ?= gcc\n")
        with self.assertRaises(ValueError):
            ctw.check(self.root)


if __name__ == "__main__":
    unittest.main()
