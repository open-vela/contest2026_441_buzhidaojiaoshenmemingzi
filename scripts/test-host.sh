#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Compatibility entry for the current host suite. The old stub-only build
# predates the NuttX Audio implementation and cannot validate its contracts.
set -euo pipefail
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec bash "$repo_dir/tools/agora/run-offline-checks.sh"
