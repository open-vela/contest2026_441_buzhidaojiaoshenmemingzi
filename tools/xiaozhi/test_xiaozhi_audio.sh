#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
opus="$repo/../external/opus/opus"
output="$repo/out/host/xiaozhi-audio"
mkdir -p "$output"
# Use the workspace's locked Opus source, not an unrelated system libopus.
cmake -S "$opus" -B "$output/opus" -DOPUS_FIXED_POINT=ON \
  -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF \
  -DOPUS_DISABLE_INTRINSICS=ON -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build "$output/opus" --parallel 2
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -g -I"$opus/include" \
  -I"$repo/app/vision_badge/include" \
  -I"$repo/tools/xiaozhi/audio-test-stubs" \
  "$repo/app/vision_badge/src/xiaozhi_codec.c" \
  "$repo/app/vision_badge/src/xiaozhi_playout.c" \
  "$repo/tests/host/test_xiaozhi_audio.c" "$output/opus/libopus.a" \
  -lm -pthread -o "$output/test-audio"
"$output/test-audio"
