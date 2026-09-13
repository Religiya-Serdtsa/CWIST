#!/usr/bin/env python3
"""Fail CI when a tests/test_*.c file has no Makefile TEST_TARGETS entry.

Motivation: multiple PRs in a row added a new tests/test_*.c file without
a matching Makefile build rule or TEST_TARGETS entry - `make test` never
ran them, so a green CI check on those PRs never actually covered the new
test. A repo-wide sweep on 2026-09-13 found 12 such files accumulated over
time (most had simply been forgotten; test_quic_flow_control turned out to
be genuinely incomplete - see EXPECTED_UNWIRED below). This script is the
gate that should have caught each of those the first time.

Usage: python3 scripts/ci/check_test_wiring.py [repo_root]
Exit 0 (all test_*.c files are wired in, or explicitly and knowingly not)
Exit 1 (found a test_*.c file with no TEST_TARGETS entry and no listed reason)
"""
from __future__ import annotations
import re
import sys
from pathlib import Path

# Files intentionally left out of `make test`, with why - not a place to
# silence this gate, a place to record a real, checked reason. Anything
# not listed here must be wired in or deleted.
EXPECTED_UNWIRED = {
    "test_quic_flow_control": (
        "links against quic_stream_fc_init()/quic_flow_control_*() which "
        "are not implemented anywhere in the tree yet (undefined reference "
        "at link time) - the test file is ahead of the feature, not "
        "forgotten. Wire it in once cwist/net/quic_flow_control.h has a "
        "real implementation to link against."
    ),
}


def parse_test_targets(makefile_text: str) -> set[str]:
    """Every identifier in the TEST_TARGETS = ... \\ ... continuation block."""
    m = re.search(r"^TEST_TARGETS\s*=\s*(.*?)(?=^\S|\Z)", makefile_text, re.M | re.S)
    if not m:
        raise ValueError("Could not find a TEST_TARGETS = ... block in the Makefile")
    body = m.group(1)
    return set(re.findall(r"test_[A-Za-z0-9_]+", body))


def find_test_files(tests_dir: Path) -> set[str]:
    return {p.stem for p in tests_dir.glob("test_*.c")}


def check(root: Path) -> list[str]:
    makefile_text = (root / "Makefile").read_text()
    wired = parse_test_targets(makefile_text)
    on_disk = find_test_files(root / "tests")
    missing = sorted(on_disk - wired - set(EXPECTED_UNWIRED))
    return missing


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
    missing = check(root)
    if missing:
        print("The following tests/test_*.c files have no TEST_TARGETS entry "
              "in the Makefile - make test never runs them:")
        for name in missing:
            print(f"  - {name}.c")
        print()
        print("Either add a build rule + TEST_TARGETS entry (see any recent "
              "one in the Makefile for the pattern), or if it's genuinely not "
              "runnable yet, add it to EXPECTED_UNWIRED in this script with a "
              "real reason (not just to silence this check).")
        return 1
    print(f"OK: every tests/test_*.c file is wired into TEST_TARGETS "
          f"(or listed in EXPECTED_UNWIRED with a reason).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
