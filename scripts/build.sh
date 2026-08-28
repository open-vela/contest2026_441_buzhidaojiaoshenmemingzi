#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -eu

repo_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
workspace_dir=$(dirname "$repo_dir")

profile=${1:-nsh}
case "$profile" in
  nsh)
    board_config=vendor/beken/boards/bk7258/bk7258-r1/configs/nsh
    if test "$#" -gt 0; then shift; fi
    ;;
  app)
    board_config=vendor/beken/boards/bk7258/bk7258-r1/configs/contest2026_441_vision_badge
    shift
    ;;
  -h|--help)
    printf 'Usage: %s [nsh|app] [openvela build arguments]\n' "$0"
    exit 0
    ;;
  *)
    board_config=vendor/beken/boards/bk7258/bk7258-r1/configs/nsh
    ;;
esac

if ! test -x "$workspace_dir/build.sh"; then
  printf 'Missing %s/build.sh; initialize and sync the full repo workspace first.\n' "$workspace_dir" >&2
  exit 1
fi

if ! test -f "$workspace_dir/$board_config/defconfig"; then
  printf 'Missing linked board config: %s/%s/defconfig\n' "$workspace_dir" "$board_config" >&2
  printf 'Run repo sync so the team manifest linkfile is created.\n' >&2
  exit 1
fi

if ! test -f "$workspace_dir/vendor/beken/boards/bk7258/bk7258-r1/CMakeLists.txt"; then
  printf 'Missing the team-maintained BK7258 R1 board directory.\n' >&2
  printf 'Run repo sync or scripts/sync-openvela-port.sh --install first.\n' >&2
  exit 1
fi

"$repo_dir/scripts/sync-openvela-port.sh" --check "$workspace_dir"

cd "$workspace_dir"
exec ./build.sh "$board_config/" --cmake "$@"
