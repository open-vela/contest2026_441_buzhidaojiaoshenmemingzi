#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
import subprocess
import tempfile
repo=Path(__file__).resolve().parents[2]
json=repo.parent/'apps/netutils/cjson/cJSON'
with tempfile.TemporaryDirectory(prefix='r1-wake-stream-') as directory:
    root=Path(directory)
    for name,text in {'nuttx/config.h':'',
        'arch/chip/bk7258_psram.h':'#include <stddef.h>\nvoid *bk7258_psram_malloc(size_t);\nvoid bk7258_psram_free(void*);\n'}.items():
        p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
    binary=root/'test'
    subprocess.run(['cc','-D_DEFAULT_SOURCE','-DCONFIG_CONTEST2026_441_WAKE_ASR=1','-DCONFIG_CONTEST2026_441_WAKE_STREAM=1',
        '-std=c11','-Wall','-Wextra','-Werror','-g','-O1','-no-pie','-fsanitize=address,undefined',
        '-fno-sanitize-recover=all','-I'+str(root),'-I'+str(repo/'app/vision_badge/include'),
        '-I'+str(repo/'tools/agora/control-test-stubs'),'-I'+str(json),
        str(repo/'tools/agora/test_wake_stream.c'),str(json/'cJSON.c'),'-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=20)
