#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile and exercise the actual board renderer against pinned OpenVela LVGL.

Use --config with a resolved candidate AP .config for matching feature flags.
Outputs only software RGB565 frames; no hardware or cloud transport is used.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parents[1]
sys.path.insert(0, str(REPO / "tools"))
from lvgl_dependency import verify


def run(*args):
    subprocess.run(args, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, default=REPO.parent)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--frames", type=Path)
    args = parser.parse_args()
    dependency = verify(args.workspace)
    flags = []
    for line in args.config.read_text().splitlines():
        match = re.fullmatch(r"(CONFIG_LV_\w+)=(.*)", line)
        if match:
            value = "1" if match[2] == "y" else match[2]
            flags.append(f"#define {match[1]} {value}")
    if "#define CONFIG_LV_COLOR_DEPTH 16" not in flags:
        parser.error("requires the resolved RGB565 LVGL candidate config")
    if args.frames:
        args.frames.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="r1-lvgl-") as temporary:
        root = Path(temporary)
        (root / "config.h").write_text("\n".join(flags) + "\n")
        board = REPO / "boards/bk7258/aidk_ai_toy"
        (root / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.16)
project(r1_lvgl_host C CXX ASM)
add_compile_options(-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all)
add_link_options(-fsanitize=address,undefined -no-pie)
add_compile_definitions(LV_CONF_KCONFIG_EXTERNAL_INCLUDE="{root}/config.h")
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)
add_subdirectory("{dependency}" lvgl)
add_executable(test_status "{board}/src/bk7258_aidk_eye_status.c" "{TOOLS}/test_eye_status.c")
target_include_directories(test_status PRIVATE "{board}/include" "{dependency.parent}")
target_link_libraries(test_status PRIVATE lvgl m)
''')
        run("cmake", "-S", str(root), "-B", str(root / "build"), "-G", "Ninja")
        run("cmake", "--build", str(root / "build"), "--parallel", "2")
        command = [str(root / "build/test_status")]
        if args.frames:
            command.append(str(args.frames.resolve()))
        run(*command)


if __name__ == "__main__":
    main()
