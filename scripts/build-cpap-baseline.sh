#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
workspace_dir=$(dirname "$repo_dir")
sdk_dir=${BK_AVDK_SMP_DIR:-"$workspace_dir/beken_reference/sdk/bk_avdk_smp"}
container_image=${BK_AVDK_BUILD_IMAGE:-bekencorp/armino-idk:1.5}
expected_revision=${BK_AVDK_SMP_REVISION:-d2ded037798530175e5dc5cde6fa1878f5d5ef35}
project_dir="$sdk_dir/projects/app_ab"
package_dir="$project_dir/build/bk7258/app_ab/package"

if ! command -v docker >/dev/null 2>&1; then
  printf 'docker is required to build the official BK7258 CP/AP baseline.\n' >&2
  exit 1
fi

if ! test -d "$sdk_dir/.git"; then
  printf 'BK AVDK SMP was not found at %s\n' "$sdk_dir" >&2
  printf 'Set BK_AVDK_SMP_DIR to the release/v3.1.1 checkout.\n' >&2
  exit 1
fi

actual_revision=$(git -C "$sdk_dir" rev-parse HEAD)
if test "$actual_revision" != "$expected_revision"; then
  printf 'SDK revision mismatch.\nexpected: %s\nactual:   %s\n' \
    "$expected_revision" "$actual_revision" >&2
  exit 1
fi

run_sdk_make() {
  docker run --rm \
    -v "$sdk_dir:/armino" \
    -w /armino \
    -u "$(id -u):$(id -g)" \
    "$container_image" \
    make -C projects/app_ab "$@" SDK_DIR=/armino
}

if test "${1:-}" = "--clean"; then
  run_sdk_make clean
elif test "$#" -ne 0; then
  printf 'usage: %s [--clean]\n' "$0" >&2
  exit 2
fi

run_sdk_make bk7258

printf '\nCP/AP baseline outputs:\n'
sha256sum \
  "$package_dir/all-app.bin" \
  "$package_dir/app_ab_crc.rbl" \
  "$project_dir/build/bk7258/app_ab/bk7258/app.elf" \
  "$project_dir/build/bk7258/app_ab/bk7258_ap/app.elf"
