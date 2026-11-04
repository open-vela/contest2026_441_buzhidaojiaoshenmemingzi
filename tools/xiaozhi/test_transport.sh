#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
workspace=$(dirname "$root")
lws="$workspace/references/libwebsockets-4.3.1"
build="$workspace/out/host/lws"
out="$workspace/out/host/xiaozhi-transport-test"
test -f "$build/lib/libwebsockets.a"
cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I"$root/tools/xiaozhi/audio-test-stubs" -I"$root/app/vision_badge/include" \
  -I"$lws/include" -I"$build" \
  -I"$workspace/apps/netutils/cjson/cJSON" \
  "$root/tools/xiaozhi/test_transport.c" \
  "$root/app/vision_badge/src/xiaozhi_transport.c" \
  "$root/app/vision_badge/src/xiaozhi_protocol.c" \
  "$workspace/apps/netutils/cjson/cJSON/cJSON.c" \
  "$build/lib/libwebsockets.a" -pthread -ldl -lm -o "$out"
python3 "$root/tools/xiaozhi/test_transport.py" "$out"
