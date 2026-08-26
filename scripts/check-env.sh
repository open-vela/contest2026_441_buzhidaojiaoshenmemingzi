#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
workspace_dir=$(dirname "$repo_dir")
failed=0

check_command() {
  if command -v "$1" >/dev/null 2>&1; then
    printf '[ok]   %s\n' "$1"
  else
    printf '[miss] %s\n' "$1"
    failed=1
  fi
}

check_command git
check_command repo
check_command python3

if command -v arm-none-eabi-gcc >/dev/null 2>&1 ||
   test -x "$workspace_dir/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin/arm-none-eabi-gcc"; then
  printf '[ok]   arm-none-eabi-gcc\n'
else
  printf '[miss] arm-none-eabi-gcc\n'
  failed=1
fi

for path in \
  "$workspace_dir/build.sh" \
  "$workspace_dir/nuttx" \
  "$workspace_dir/vendor/beken/boards/bk7258/bk7258-devkit" \
  "$workspace_dir/packages/demos/contest2026_441_vision_badge"; do
  if test -e "$path"; then
    printf '[ok]   %s\n' "$path"
  else
    printf '[miss] %s\n' "$path"
    failed=1
  fi
done

if test "$failed" -ne 0; then
  printf '\nEnvironment is incomplete. Run repo init/repo sync from README first.\n' >&2
  exit 1
fi

printf '\nBK7258 openvela workspace is ready for an experimental build.\n'
