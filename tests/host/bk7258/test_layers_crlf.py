#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Regression test for platform-independent source exception hashes."""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tools/bk7258"))

from _lib import layers  # noqa: E402


def test_lf_and_crlf_have_the_same_source_hash() -> None:
    with tempfile.TemporaryDirectory(prefix="bk7258-layer-hash-") as temporary:
        root = Path(temporary)
        lf = root / "lf.c"
        crlf = root / "crlf.c"
        lf.write_bytes(b"int value = 1;\n")
        crlf.write_bytes(b"int value = 1;\r\n")
        assert layers._sha256(lf) == layers._sha256(crlf)


if __name__ == "__main__":
    test_lf_and_crlf_have_the_same_source_hash()
    print("BK7258_LAYER_CRLF_TEST_PASS")
