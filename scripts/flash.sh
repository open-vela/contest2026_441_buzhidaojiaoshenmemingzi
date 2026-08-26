#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -eu

if test "$#" -ne 3 || test "$3" != "--confirm"; then
  printf 'Usage: %s /dev/ttyUSB0 path/to/all-app-nuttx.bin --confirm\n' "$0" >&2
  exit 2
fi

port=$1
image=$2

case "$port" in
  /dev/ttyUSB[0-9]*) port_number=${port#/dev/ttyUSB} ;;
  [0-9]*) port_number=$port ;;
  *)
    printf 'Unsupported port: %s (use /dev/ttyUSB<N> or <N>).\n' "$port" >&2
    exit 2
    ;;
esac

case "$port_number" in
  ''|*[!0-9]*)
    printf 'Invalid Beken port number derived from: %s\n' "$port" >&2
    exit 2
    ;;
esac

if ! test -f "$image"; then
  printf 'Missing packed BK7258 image: %s\n' "$image" >&2
  exit 1
fi

if ! command -v bk_loader >/dev/null 2>&1; then
  printf 'Missing bk_loader. Install the Beken flashing tool documented by the BSP.\n' >&2
  exit 1
fi

printf 'Flashing will overwrite the selected device: %s\n' "$port"
printf 'Image: %s\n' "$image"
exec bk_loader download -p "$port_number" -b 1500000 -i "$image"
