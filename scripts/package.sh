#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -eu

repo_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
workspace_dir=$(dirname "$repo_dir")
profile=${1:-nsh}

if test "$#" -gt 1; then
  printf 'Usage: %s [nsh|app]\n' "$0" >&2
  exit 2
fi

case "$profile" in
  nsh)
    build_dir="$workspace_dir/cmake_out/bk7258-r1_nsh"
    ;;
  app)
    build_dir="$workspace_dir/cmake_out/bk7258-r1_contest2026_441_vision_badge"
    ;;
  -h|--help)
    printf 'Usage: %s [nsh|app]\n' "$0"
    exit 0
    ;;
  *)
    printf 'Unsupported profile: %s (use nsh or app).\n' "$profile" >&2
    exit 2
    ;;
esac

nuttx_bin="$build_dir/nuttx.bin"
repack="$repo_dir/board/bk7258-r1/tools/repack.py"
publish_dir="$workspace_dir/out/bk7258-r1/$profile"
work_dir="$publish_dir/repack-work"
image_name=bk7258-r1-openvela-all-app.bin
manifest_name=bk7258-r1-openvela-manifest.json
checksum_name=bk7258-r1-openvela-all-app.sha256

if ! test -f "$nuttx_bin"; then
  printf 'Missing %s; run scripts/build.sh %s first.\n' "$nuttx_bin" "$profile" >&2
  exit 1
fi

mkdir -p "$publish_dir"
python3 "$repack" --nuttx-bin "$nuttx_bin" --output-dir "$work_dir"

cp "$work_dir/all-app-openvela.bin" "$publish_dir/$image_name.tmp"
mv "$publish_dir/$image_name.tmp" "$publish_dir/$image_name"
cp "$work_dir/manifest.json" "$publish_dir/$manifest_name.tmp"
mv "$publish_dir/$manifest_name.tmp" "$publish_dir/$manifest_name"

(
  cd "$publish_dir"
  sha256sum "$image_name" > "$checksum_name.tmp"
)
mv "$publish_dir/$checksum_name.tmp" "$publish_dir/$checksum_name"

printf 'Published fixed-name firmware artifacts:\n'
printf '  image:    %s/%s\n' "$publish_dir" "$image_name"
printf '  manifest: %s/%s\n' "$publish_dir" "$manifest_name"
printf '  sha256:   %s/%s\n' "$publish_dir" "$checksum_name"
