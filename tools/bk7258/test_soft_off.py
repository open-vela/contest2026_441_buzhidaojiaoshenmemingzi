#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual CP soft-off, direct and signed guards; reset/SDK/Flash mocked."""
from pathlib import Path
import shutil
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="r1-soft-off-") as directory:
    root = Path(directory)
    shutil.copytree(repo / "chips/bk7258/include", root / "arch/chip")
    headers = {
        "nuttx/config.h": "#define OK 0\n#define CONFIG_BK7258_PM_SOFT_OFF 1\n#define DEBUGASSERT(c) assert(c)\n",
        "nuttx/compiler.h": "#define FAR\n#define noreturn_function __attribute__((noreturn))\n",
        "nuttx/arch.h": "#include <stdbool.h>\nvoid up_mdelay(unsigned int);\nbool up_interrupt_context(void);\n",
        "nuttx/mutex.h": "typedef int mutex_t;\n#define NXMUTEX_INITIALIZER 0\nint nxmutex_lock(mutex_t*);\nint nxmutex_timedlock(mutex_t*,unsigned int);\nint nxmutex_unlock(mutex_t*);\n",
        "nuttx/sched.h": "#include <sys/types.h>\npid_t nxsched_gettid(void);\n",
        "nuttx/irq.h": "typedef unsigned int irqstate_t;\nirqstate_t up_irq_save(void);\n",
        "nuttx/clock.h": "#include <time.h>\n#define MSEC2TICK(x) (x)\n#define TICK2MSEC(x) (x)\nclock_t clock_systime_ticks(void);\n",
        "nuttx/signal.h": "#include <unistd.h>\nint nxsig_usleep(useconds_t);\n",
        "nuttx/wqueue.h": "struct work_s { int unused; };\n#define LPWORK 1\nint work_queue(int,struct work_s*,void (*)(void*),void*,unsigned int);\n",
        "driver/gpio.h": "#define BK_OK 0\n#define GPIO_INT_TYPE_LOW_LEVEL 0\n#define GPIO_INT_TYPE_HIGH_LEVEL 1\nint bk_gpio_ana_register_wakeup_source(unsigned int,int);\n",
    }
    for name, text in headers.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    for signed in (False, True):
        binary = root / ("signed" if signed else "direct")
        flags = ["-DCONFIG_BK7258_MCUBOOT_IMAGE=1", "-DCONFIG_BK7258_OTA=1"] if signed else []
        subprocess.run(["cc", "-D_DEFAULT_SOURCE", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-no-pie", "-g", "-O1",
                        *flags, "-I" + str(root), "-I" + str(repo),
                        str(repo / "tools/bk7258/test_soft_off.c"), "-o", str(binary)], check=True)
        for scenario in ("storage-barrier", "unready", "request", "queue-fail", "storage-busy", "held-key", "bad-pin",
                         "gpio-fail", "wake-fail", "sleep-return", "trial", "release-delay",
                         "wake-hold", "wake-short", "wake-gpio-hold", "wake-fast-waits",
                         "wake-late-release", "wake-released", "wake-read-fail", "wake-slow-waits"):
            subprocess.run([str(binary), scenario], check=True)
