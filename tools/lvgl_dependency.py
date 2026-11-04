#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Restore or verify the optional, exact OpenVela LVGL dependency."""
import argparse
import json
from pathlib import Path
import subprocess

from restore_workspace import checkout


def verify(workspace):
    lock = json.loads((Path(__file__).parent / "bk7258/lvgl.json").read_text())
    source = workspace / lock["path"]
    if not (source / ".git").exists():
        raise RuntimeError("LVGL missing: run tools/lvgl_dependency.py --restore")
    def git(*args):
        return subprocess.check_output(["git", "-C", str(source), *args], text=True).strip()
    if git("rev-parse", "HEAD") != lock["commit"] or git("rev-parse", "HEAD^{tree}") != lock["tree"]:
        raise RuntimeError("LVGL commit/tree differs from the lock")
    if git("status", "--porcelain", "--untracked-files=all"):
        raise RuntimeError("LVGL has local changes; refusing unrecorded dependency inputs")
    if not (source / lock["license_file"]).is_file():
        raise RuntimeError("LVGL license missing")
    print("LVGL_VERIFIED " + lock["commit"] + " " + lock["tree"])
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--restore", action="store_true")
    args = parser.parse_args()
    if args.restore:
        lock = json.loads((Path(__file__).parent / "bk7258/lvgl.json").read_text())
        checkout(args.workspace, lock["path"], lock["url"], lock["commit"], lock["tree"])
    verify(args.workspace)


if __name__ == "__main__":
    main()
