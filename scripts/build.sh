#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
workspace_dir=$(dirname "$repo_dir")
board_config=vendor/beken/boards/bk7258/bk7258-devkit/configs/contest2026_441_vision_badge

if ! test -x "$workspace_dir/build.sh"; then
  printf 'Missing %s/build.sh; initialize and sync the full repo workspace first.\n' "$workspace_dir" >&2
  exit 1
fi

if ! test -f "$workspace_dir/$board_config/defconfig"; then
  printf 'Missing linked board config: %s/%s/defconfig\n' "$workspace_dir" "$board_config" >&2
  printf 'Run repo sync so the team manifest linkfile is created.\n' >&2
  exit 1
fi

if ! test -f "$workspace_dir/vendor/beken/boards/bk7258/bk7258-devkit/CMakeLists.txt"; then
  printf 'Missing BK7258 board support. The public BSP is still awaiting upstream merge.\n' >&2
  printf 'See docs/BK7258移植调研.md before selecting a reviewed revision.\n' >&2
  exit 1
fi

cd "$workspace_dir"
exec ./build.sh "$board_config/" --cmake "$@"
