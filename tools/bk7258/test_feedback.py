#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise actual workflow and R1 feedback mapping; display queue is mocked."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="r1-feedback-") as temporary:
    root = Path(temporary)
    board = root / "arch/board"
    board.mkdir(parents=True)
    (board / "board.h").write_bytes((repo / "boards/bk7258/aidk_ai_toy/include/board.h").read_bytes())
    (board / "bk7258_board_config.h").write_text("")
    (root / "bk7258_board.h").write_text("")
    binary = root / "test_feedback"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-no-pie",
                    "-DCONFIG_BK7258_AIDK_EYE_LVGL_STATUS=1", "-DCONFIG_BK7258_AIDK_DUAL_LCD=1",
                    "-I" + str(root), "-I" + str(repo / "app/vision_badge/include"),
                    str(repo / "tools/bk7258/test_feedback.c"),
                    str(repo / "app/vision_badge/src/workflow.c"),
                    str(repo / "app/vision_badge/src/feedback_service.c"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
