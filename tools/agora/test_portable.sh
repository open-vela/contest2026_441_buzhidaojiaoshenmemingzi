#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
project=$(cd "$(dirname "$0")/../.." && pwd)
workspace=${R1_TEST_WORKSPACE:-$(dirname "$project")}
sdk="$workspace/vendor/beken/bk_avdk_smp/ap/components/bk_thirdparty/agora-iot-sdk"
out="$project/out/host/agora-portable"
mkdir -p "$out"
test "$(git -C "$sdk" rev-parse HEAD)" = cb080de1655d579c7593ecf504c440997c4c137b
# Use the same portable list as the actual firmware target. No copied SDK.
mapfile -t portable < <(python3 - "$project/chips/bk7258/ap/agora/agora.cmake" <<'PY'
import re, sys
text = open(sys.argv[1], encoding='utf-8').read()
block = re.search(r'set\(portable\s+(.*?)\)', text, re.S).group(1)
for path in block.split():
    print(path)
PY
)
objects=()
flags=(-D_GNU_SOURCE -DVISION_AGORA_HOST_TEST -U__linux__
       -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all
       -fno-omit-frame-pointer -include sys/types.h
       -I"$sdk/hal/aosl/include" -I"$sdk/hal/aosl/platform/include"
       -I"$sdk/hal/aosl/platform/src/bk7258/config")
for src in "${portable[@]}"; do
  obj="$out/${src//\//_}.o"
  gcc "${flags[@]}" -c "$sdk/hal/aosl/$src" -o "$obj"
  objects+=("$obj")
done
gcc "${flags[@]}" -Wall -Wextra -Werror \
  "$project/chips/bk7258/ap/agora/agora_nuttx_hal.c" \
  "$project/chips/bk7258/ap/agora/agora_nuttx_socket.c" \
  "$project/tools/agora/test_portable.c" "${objects[@]}" \
  -pthread -no-pie -o "$out/test_portable"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 timeout 20s "$out/test_portable"
