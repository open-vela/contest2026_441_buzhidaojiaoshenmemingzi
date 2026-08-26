#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -eu

if test "$#" -ne 1; then
  printf 'Usage: %s /dev/ttyUSB0\n' "$0" >&2
  exit 2
fi

port=$1

if command -v picocom >/dev/null 2>&1; then
  exec picocom --baud 115200 "$port"
fi

if command -v minicom >/dev/null 2>&1; then
  exec minicom -D "$port" -b 115200
fi

printf 'Neither picocom nor minicom is installed.\n' >&2
exit 1
