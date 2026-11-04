#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
workspace=$(dirname "$project")
json="$workspace/apps/netutils/cjson/cJSON"
out="$project/out/host/agora"
mkdir -p "$out"
python3 - "$project" "$out/agora-load-test.inc" <<'PY'
from pathlib import Path
import sys
source = (Path(sys.argv[1]) / "app/vision_badge/src/agora_service.c").read_text()
start = source.index("static int load_local(")
end = source.index("static int json_string(", start)
Path(sys.argv[2]).write_text(source[start:end])
PY
temp=$(mktemp -d)
trap 'rm -f "$temp/agora.tmp" "$temp/agora.cfg" "$temp/volume.tmp" "$temp/volume.cfg"; rmdir "$temp"' EXIT
cc -D_DEFAULT_SOURCE -DCONFIG_CONTEST2026_441_WAKE_ASR=1 -DCONFIG_CONTEST2026_441_WAKE_STREAM=1 -DCONFIG_CONTEST2026_441_WAKE_UDP=1 -std=c11 -Wall -Wextra -Werror -g -O1 -no-pie \
  -DAGORA_CONFIG_DIR=\""$temp"\" \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$project/tools/agora/control-test-stubs" \
  -I"$project/tools/xiaozhi/audio-test-stubs" \
  -I"$project/app/vision_badge/include" -I"$json" \
  -I"$out" \
  "$project/tools/agora/test_control.c" "$json/cJSON.c" -lm -o "$out/test_control"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_control"
