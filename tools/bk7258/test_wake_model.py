#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Real locked model/frontend/TFLM. Synthetic PCM is not speech accuracy proof."""
from pathlib import Path
import argparse, subprocess, tempfile
repo=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--workspace',type=Path,default=repo.parent)
p.add_argument('--reference',action='store_true',help='Build the original portable TFLM kernels for comparison')
args=p.parse_args()
out=repo/('out/host/wake-model-reference' if args.reference else 'out/host/wake-model');out.mkdir(parents=True,exist_ok=True)
subprocess.run(['python3',str(repo/'tools/wake_dependency.py'),'--workspace',str(args.workspace),
                '--emit-model',str(out/'model.c')],check=True)
(out/'stubs/arch/chip').mkdir(parents=True,exist_ok=True)
(out/'stubs/arch/chip/bk7258_psram.h').write_text('#include <stddef.h>\n#ifdef __cplusplus\nextern "C" {\n#endif\nvoid *bk7258_psram_malloc(size_t);\nvoid bk7258_psram_free(void*);\n#ifdef __cplusplus\n}\n#endif\n')
cmake='''cmake_minimum_required(VERSION 3.16)
project(r1_wake_host C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(R1_WAKE_WORKSPACE "@WORKSPACE@")
include("@REPO@/tools/bk7258/wake_sources.cmake")
add_executable(test_wake ${R1_WAKE_CORE_SOURCES} model.c
  "@REPO@/app/vision_badge/src/wake_model.cc" "@REPO@/app/vision_badge/src/wake_policy.c"
  "@REPO@/tools/bk7258/test_wake_model.cc")
target_include_directories(test_wake PRIVATE ${R1_WAKE_INCLUDE_DIRS} stubs "@REPO@/app/vision_badge/include")
target_compile_definitions(test_wake PRIVATE ${R1_WAKE_COMPILE_DEFINITIONS})
target_compile_options(test_wake PRIVATE -g -O1 -fno-omit-frame-pointer -ffunction-sections -fdata-sections
  -fsanitize=address,undefined -fno-sanitize-recover=all
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions> $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
target_link_options(test_wake PRIVATE -fsanitize=address,undefined -no-pie -Wl,--gc-sections)
target_link_libraries(test_wake PRIVATE m)
'''.replace('@REPO@',str(repo)).replace('@WORKSPACE@',str(args.workspace))
(out/'CMakeLists.txt').write_text(cmake)
subprocess.run(['cmake','-S',str(out),'-B',str(out/'build'),'-G','Ninja',
                '-DR1_WAKE_USE_CMSIS_NN='+('OFF' if args.reference else 'ON')],check=True)
subprocess.run(['cmake','--build',str(out/'build'),'--parallel','2'],check=True)
subprocess.run([str(out/'build/test_wake')],check=True)
