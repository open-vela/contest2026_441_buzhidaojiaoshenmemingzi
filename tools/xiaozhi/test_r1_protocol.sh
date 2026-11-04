#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

r1_repo=$(cd "$(dirname "$0")/../.." && pwd)
r1_cjson="$r1_repo/../apps/netutils/cjson/cJSON"
r1_output="$r1_repo/out/host/xiaozhi"
mkdir -p "$r1_output"

gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$r1_repo/app/vision_badge/include" -I"$r1_cjson" \
  "$r1_repo/app/vision_badge/src/xiaozhi_protocol.c" \
  "$r1_repo/tests/host/test_xiaozhi_protocol.c" "$r1_cjson/cJSON.c" \
  -lm -o "$r1_output/test-xiaozhi-protocol"
"$r1_output/test-xiaozhi-protocol"
