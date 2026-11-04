#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Restore the bounded R1 build dependencies in Linux, without deleting files.

This prepares sources and links, not a successful firmware build. Build/SDK
generation remains owned by tools/bk7258/bk7258.py. The resolved revisions of
previously unlocked dependencies are saved for repeat runs.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET


BASE_PATHS = (
    "nuttx", "apps", "build", "external", "frameworks", "packages",
    "packages/demos", "vendor/openvela", "apps/crypto/mbedtls/mbedtls",
    "apps/netutils/cjson/cJSON", "external/opus/opus",
    "nuttx/openamp/libmetal", "nuttx/openamp/open-amp",
    "nuttx/fs/littlefs/littlefs",
)
PINNED = {
    "nuttx": "76354c637858ecb0aa4601629327acb6f44a26bb",
    "apps": "dcc6a95c3b323e533c98fde8fb209f99e24f0fdd",
    "nuttx/openamp/libmetal": "fbd291502a1b4113febd0b6bc87e919247fc7b4d",
    "nuttx/openamp/open-amp": "8608b94c947e525dadd781ce6f169ed49b0f2a2b",
    "nuttx/fs/littlefs/littlefs": "b97ad1f437c3fa3128ca2d70b8317c1207c90ac9",
}


def run(*args, capture=False):
    result = subprocess.run(args, check=True, text=True,
                            stdout=subprocess.PIPE if capture else None)
    return result.stdout.strip() if capture else None


def safe_path(root, name):
    relative = Path(name)
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError(f"invalid relative path: {name}")
    target = root / relative
    target.resolve().relative_to(root.resolve())
    return target


def checkout(root, name, url, revision, expected_tree=None):
    dest = safe_path(root, name)
    if not (dest / ".git").exists():
        # An empty mount point or parent repository's untracked directory is OK;
        # existing nonempty non-Git content is never replaced.
        if dest.exists() and any(dest.iterdir()):
            raise RuntimeError(f"non-Git directory is not empty: {dest}")
        dest.parent.mkdir(parents=True, exist_ok=True)
        run("git", "init", "-q", str(dest))
        run("git", "-C", str(dest), "remote", "add", "origin", url)
    actual_url = run("git", "-C", str(dest), "remote", "get-url", "origin",
                     capture=True)
    if actual_url != url:
        raise RuntimeError(f"unexpected origin: {dest}")
    if run("git", "-C", str(dest), "status", "--porcelain", "--untracked-files=no",
           capture=True):
        raise RuntimeError(f"tracked local modifications: {dest}")
    head = subprocess.run(["git", "-C", str(dest), "rev-parse", "--verify", "HEAD"],
                          text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if head.returncode == 0:
        if head.stdout.strip() != revision:
            raise RuntimeError(f"existing checkout differs from lock: {dest}")
    else:
        try:
            run("git", "-C", str(dest), "fetch", "--depth=1", "origin", revision)
        except subprocess.CalledProcessError:
            if expected_tree is None:
                raise
            # Some mirrors rewrote history while retaining the exact source
            # tree. An alternate commit is accepted ONLY with byte-identical
            # locked Git tree, not because the branch name looks compatible.
            run("git", "-C", str(dest), "fetch", "--depth=1", "origin",
                "refs/heads/dev-ai-contest-2026")
            observed_tree = run("git", "-C", str(dest), "rev-parse", "FETCH_HEAD^{tree}",
                                capture=True)
            if observed_tree != expected_tree:
                raise RuntimeError(f"unavailable revision, alternate tree differs: {name}")
            print(f"IDENTICAL_TREE_RECOVERY {name} requested={revision}", flush=True)
        run("git", "-C", str(dest), "switch", "--detach", "FETCH_HEAD")
    commit = run("git", "-C", str(dest), "rev-parse", "HEAD", capture=True)
    tree = run("git", "-C", str(dest), "rev-parse", "HEAD^{tree}", capture=True)
    if expected_tree is not None and tree != expected_tree:
        raise RuntimeError(f"locked dependency tree differs: {name}")
    print(f"SOURCE_READY {name} {commit}", flush=True)
    return {"url": url, "commit": commit, "tree": tree}


def link(dest, source):
    if dest.is_symlink():
        if dest.resolve() != source.resolve():
            raise RuntimeError(f"wrong existing link: {dest}")
        return
    if dest.exists():
        raise RuntimeError(f"refusing to replace existing path: {dest}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.symlink_to(os.path.relpath(source, dest.parent))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--with-sdk", action="store_true")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("run on the Ubuntu VM, not Windows")
    repository = Path(__file__).resolve().parents[1]
    root = args.workspace.resolve()
    root.mkdir(parents=True, exist_ok=True)
    if repository.parent != root:
        parser.error("place the team repository directly under the selected workspace")
    lock_path = root / ".workspace" / "restore-lock.json"
    lock_path.parent.mkdir(exist_ok=True)
    seed_lock = repository / "config/restored-workspace.lock.json"
    lock = json.loads(lock_path.read_text()) if lock_path.exists() else (
        json.loads(seed_lock.read_text()) if seed_lock.exists() else {})
    dependency_rows = json.loads((repository / "nuttx/dependencies.lock.json").read_text())["projects"]
    locked_trees = {row["path"]: row["tree"] for row in dependency_rows}
    base = ET.parse(repository / "openvela.xml").getroot()
    default = base.find("default").get("revision")
    projects = {item.get("path"): item for item in base.findall("project")}
    paths = list(BASE_PATHS)
    if args.with_sdk:
        paths.append("vendor/beken/bk_avdk_smp")
        projects[paths[-1]] = ET.parse(
            repository / "contest2026_441_buzhidaojiaoshenmemingzi.xml"
        ).getroot().find("project[@path='vendor/beken/bk_avdk_smp']")
    for name in paths:
        item = projects[name]
        url = "https://github.com/" + (item.get("name") if
               item.get("remote") == "git" else "open-vela/" + item.get("name")) + ".git"
        revision = lock.get(name, {}).get("commit") or PINNED.get(name)
        if revision is None:
            ref = item.get("revision") or default
            if len(ref) == 40:
                revision = ref
            else:
                resolved = run("git", "ls-remote", url, "refs/heads/" + ref,
                               capture=True)
                if not resolved:
                    raise RuntimeError(f"branch not found: {url} {ref}")
                revision = resolved.split()[0]
        expected_tree = locked_trees.get(name) or lock.get(name, {}).get("tree")
        lock[name] = checkout(root, name, url, revision, expected_tree)
        lock_path.write_text(json.dumps(lock, indent=2) + "\n")
        for item_link in item.findall("linkfile"):
            source = safe_path(root / name, item_link.get("src"))
            if not source.exists():
                raise RuntimeError(f"missing manifest link source: {source}")
            link(safe_path(root, item_link.get("dest")), source)
    team_manifest = ET.parse(repository / "contest2026_441_buzhidaojiaoshenmemingzi.xml")
    for item in team_manifest.findall(".//linkfile"):
        source = safe_path(repository, item.get("src"))
        if not source.exists():
            raise RuntimeError(f"missing link source: {source}")
        link(safe_path(root, item.get("dest")), source)
    (repository / "prebuilt").mkdir(exist_ok=True)
    link(root / "vendor/beken/prebuilt", repository / "prebuilt")
    print("RESTORE_SOURCE_READY; SDK, toolchain and firmware build remain separate gates.")


if __name__ == "__main__":
    main()
