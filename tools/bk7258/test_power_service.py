#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual power coordinator, with resource owners/CP/clock mocked; no hardware."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="r1-power-") as directory:
    root = Path(directory)
    headers = {
        "nuttx/config.h": "",
        "nuttx/clock.h": "#include <time.h>\n#define MSEC2TICK(x) (x)\nclock_t clock_systime_ticks(void);\n",
        "nuttx/mutex.h": "typedef int mutex_t;\n#define NXMUTEX_INITIALIZER 0\nint nxmutex_trylock(mutex_t*);\nvoid nxmutex_unlock(mutex_t*);\n",
        "nuttx/signal.h": "int nxsig_usleep(unsigned int);\n",
        "arch/board/board.h": "#include <stdbool.h>\nint bk7258_aidk_eye_power(bool);\n",
        "arch/chip/bk7258_pm.h": "int bk7258_pm_soft_off_request(void);\nint bk7258_pm_soft_off_status(void);\n",
    }
    for name, text in headers.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    binary = root / "test"
    subprocess.run(["cc", "-D_DEFAULT_SOURCE", "-DCONFIG_CONTEST2026_441_POWER=1",
                    "-DCONFIG_BK7258_AP_CORE=1", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-no-pie", "-g", "-O1",
                    "-I" + str(root), "-I" + str(repo / "app/vision_badge/include"),
                    str(repo / "tools/bk7258/test_power_service.c"),
                    str(repo / "app/vision_badge/src/power_service.c"), "-o", str(binary)], check=True)
    for scenario in ("quiet", "off", "lost-ack", "unknown", "voice-timeout", "retained",
                     "camera-fail", "camera-resume-fail", "display-timeout", "resume-fail", "network-busy"):
        subprocess.run([str(binary), scenario], check=True)
