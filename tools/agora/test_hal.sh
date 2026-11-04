#!/usr/bin/env bash
set -euo pipefail
project=$(cd "$(dirname "$0")/../.." && pwd)
workspace=$(dirname "$project")
sdk="$workspace/vendor/beken/bk_avdk_smp/ap/components/bk_thirdparty/agora-iot-sdk"
out="$project/out/host/agora"
mkdir -p "$out"
gcc -D_GNU_SOURCE -DVISION_AGORA_HOST_TEST -std=c11 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1 -no-pie \
  -I"$sdk/hal/aosl/platform/include" -I"$sdk/hal/aosl/platform/src/bk7258/config" \
  "$project/chips/bk7258/ap/agora/agora_nuttx_hal.c" \
  "$project/chips/bk7258/ap/agora/agora_nuttx_socket.c" "$project/tools/agora/test_hal.c" \
  -pthread -o "$out/test_hal"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_hal"
# Exercise the production scheduler attribute branch, without Linux RT access.
gcc -D_GNU_SOURCE -DVISION_AGORA_HOST_TEST -DVISION_AGORA_SCHED_TEST \
  -Dpthread_attr_setinheritsched=r1_test_attr_inherit \
  -Dpthread_attr_setschedpolicy=r1_test_attr_policy \
  -Dpthread_attr_setschedparam=r1_test_attr_priority \
  -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1 -no-pie \
  -I"$sdk/hal/aosl/platform/include" -I"$sdk/hal/aosl/platform/src/bk7258/config" \
  "$project/chips/bk7258/ap/agora/agora_nuttx_hal.c" \
  "$project/chips/bk7258/ap/agora/agora_nuttx_socket.c" "$project/tools/agora/test_hal.c" \
  -pthread -o "$out/test_hal_sched"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_hal_sched"
gcc -D_GNU_SOURCE -DVISION_AGORA_HOST_TEST -DVISION_AGORA_NET_TEST \
  -Dwrite=r1_test_write -Dsendto=r1_test_sendto -std=c11 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1 -no-pie \
  -I"$sdk/hal/aosl/platform/include" -I"$sdk/hal/aosl/platform/src/bk7258/config" \
  "$project/chips/bk7258/ap/agora/agora_nuttx_hal.c" \
  "$project/chips/bk7258/ap/agora/agora_nuttx_socket.c" "$project/tools/agora/test_hal.c" \
  -pthread -o "$out/test_hal_net"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_hal_net"
gcc -D_GNU_SOURCE -DVISION_AGORA_HOST_TEST -DVISION_AGORA_CONNECT_TEST \
  -Dselect=r1_test_select -DAGORA_CONNECT_MS=80 -std=c11 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1 -no-pie \
  -I"$sdk/hal/aosl/platform/include" -I"$sdk/hal/aosl/platform/src/bk7258/config" \
  "$project/chips/bk7258/ap/agora/agora_nuttx_hal.c" \
  "$project/chips/bk7258/ap/agora/agora_nuttx_socket.c" "$project/tools/agora/test_hal.c" \
  -pthread -o "$out/test_hal_connect"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_hal_connect"
