#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -eu

usage() {
  cat <<'EOF'
Usage: scripts/sync-openvela-port.sh [--check|--install|--capture] [workspace]

  --check    compare the team repository with the full OpenVela workspace
  --install  copy the reviewed team files into the full workspace
  --capture  copy intentional workspace edits back into the team repository

The workspace defaults to the parent directory of this team repository.
EOF
}

mode=${1:---check}
case "$mode" in
  --check|--install|--capture) ;;
  -h|--help)
    usage
    exit 0
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
workspace_dir=${2:-$(dirname "$repo_dir")}

board_repo="$repo_dir/board/bk7258-r1"
board_workspace="$workspace_dir/vendor/beken/boards/bk7258/bk7258-r1"
chip_repo="$repo_dir/porting/nuttx/arch/arm/src/bk7258"
chip_workspace="$workspace_dir/nuttx/arch/arm/src/bk7258"

board_files='
.gitignore
CMakeLists.txt
Kconfig
README_zh-cn.md
configs/nsh/defconfig
configs/contest2026_441_vision_badge/defconfig
include/board.h
scripts/ld.script
src/CMakeLists.txt
src/Make.defs
src/bk7258-r1.h
src/bk7258_appinit.c
src/bk7258_boardinitialize.c
src/bk7258_bringup.c
tools/repack.py
'

chip_files='
CMakeLists.txt
Make.defs
bk7258_allocateheap.c
bk7258_gpio.c
bk7258_gpio.h
bk7258_irq.c
bk7258_lowputc.c
bk7258_lowputc.h
bk7258_serial.c
bk7258_start.c
bk7258_start.h
bk7258_timerisr.c
chip.h
hardware/bk7258_memorymap.h
hardware/bk7258_gpio.h
hardware/bk7258_uart.h
'

failed=0

sync_file() {
  source_root=$1
  target_root=$2
  relative=$3
  source_file="$source_root/$relative"
  target_file="$target_root/$relative"

  if ! test -f "$source_file"; then
    printf '[miss] source %s\n' "$source_file" >&2
    failed=1
    return
  fi

  if test -e "$target_file" && test "$source_file" -ef "$target_file"; then
    printf '[link] %s\n' "$target_file"
    return
  fi

  case "$mode" in
    --check)
      if test -f "$target_file" && cmp -s "$source_file" "$target_file"; then
        printf '[same] %s\n' "$relative"
      elif test -f "$target_file"; then
        printf '[diff] %s\n' "$target_file" >&2
        failed=1
      else
        printf '[miss] target %s\n' "$target_file" >&2
        failed=1
      fi
      ;;
    --install|--capture)
      mkdir -p "$(dirname "$target_file")"
      cp -p "$source_file" "$target_file"
      printf '[copy] %s -> %s\n' "$source_file" "$target_file"
      ;;
  esac
}

if test "$mode" = --capture; then
  board_from=$board_workspace
  board_to=$board_repo
  chip_from=$chip_workspace
  chip_to=$chip_repo
else
  board_from=$board_repo
  board_to=$board_workspace
  chip_from=$chip_repo
  chip_to=$chip_workspace
fi

for relative in $board_files; do
  sync_file "$board_from" "$board_to" "$relative"
done

for relative in $chip_files; do
  sync_file "$chip_from" "$chip_to" "$relative"
done

if test "$failed" -ne 0; then
  printf '\nOpenVela port sources are not synchronized.\n' >&2
  exit 1
fi

printf '\nOpenVela port sources are synchronized (%s).\n' "$mode"
