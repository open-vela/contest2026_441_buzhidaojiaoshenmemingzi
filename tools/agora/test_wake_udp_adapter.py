#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile the actual adapter and the workspace's existing mbedTLS sources."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
crypto = repo.parent / "apps/crypto/mbedtls/mbedtls"
cjson = repo.parent / "apps/netutils/cjson/cJSON"
with tempfile.TemporaryDirectory(prefix="r1-wake-udp-") as directory:
    root = Path(directory)
    for name, text in {
        "nuttx/config.h": "",
        "udp-mbedtls.h": "#define MBEDTLS_MD_C\n#define MBEDTLS_SHA256_C\n",
    }.items():
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
    binary = root / "test"
    subprocess.run([
        "cc", "-D_DEFAULT_SOURCE", "-DCONFIG_CONTEST2026_441_WAKE_ASR=1",
        '-DMBEDTLS_CONFIG_FILE="udp-mbedtls.h"', "-std=c11", "-Wall", "-Wextra",
        "-Werror", "-g", "-O1", "-no-pie", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-I" + str(root),
        "-I" + str(repo / "app/vision_badge/include"),
        "-I" + str(repo / "tools/agora/control-test-stubs"),
        "-I" + str(cjson), "-I" + str(crypto / "include"),
        str(repo / "tools/agora/test_wake_udp.c"), str(cjson / "cJSON.c"),
        *[str(crypto / "library" / (name + ".c"))
          for name in ("md", "sha256", "platform_util", "constant_time")],
        "-lm", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
