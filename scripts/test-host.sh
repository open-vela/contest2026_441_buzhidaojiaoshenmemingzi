#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

# Native host tests only; these are not firmware or hardware acceptance tests.
set -euo pipefail

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output_dir="$repo_dir/build/host"
host_cc=${HOST_CC:-gcc}
mkdir -p "$output_dir"
cd "$repo_dir"

flags=(-std=c11 -Og -g -Wall -Wextra -Werror -I app/vision_badge/include)
services=(app/vision_badge/src/{workflow,camera_service,vision_service,audio_service,feedback_service}.c)

"$host_cc" --version
"$host_cc" "${flags[@]}" app/vision_badge/src/vision_badge_main.c \
  "${services[@]}" -o "$output_dir/vision_badge-host"
"$host_cc" "${flags[@]}" tests/service_contracts.c \
  "${services[@]}" -o "$output_dir/service-contracts"

"$output_dir/vision_badge-host" selftest
"$output_dir/service-contracts"

status=0
"$output_dir/vision_badge-host" run test > "$output_dir/run-negative.log" 2>&1 || status=$?
if [ "$status" -ne 1 ] ||
   ! grep -q '^vision_badge: stage=capture error=-' "$output_dir/run-negative.log"; then
  cat "$output_dir/run-negative.log" >&2
  printf 'Unexpected CLI negative-test result: exit=%s\n' "$status" >&2
  exit 1
fi

printf 'Host checks passed; outputs: %s\n' "$output_dir"
printf 'No hardware, firmware execution, or AI capability was validated.\n'
