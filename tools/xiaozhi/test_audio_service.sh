#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
testdir=$(mktemp -d)
trap 'rm -f "$testdir/audio-test"; rmdir "$testdir"' EXIT
for profile in legacy product; do
defines=
if [ "$profile" = product ]; then defines=-DCONFIG_CONTEST2026_441_POWER=1; fi
cc -std=c11 -D_DEFAULT_SOURCE $defines -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I"$root/tools/xiaozhi/audio-test-stubs" \
  -I"$root/app/vision_badge/include" \
  "$root/tools/xiaozhi/test_audio_service.c" -pthread -o "$testdir/audio-test"
"$testdir/audio-test"
done
