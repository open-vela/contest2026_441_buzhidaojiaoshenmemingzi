#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="$project/out/host/agora"
mkdir -p "$out"
python3 - "$project" "$out/mimo-network-test.inc" <<'PY'
from pathlib import Path
import sys
source = (Path(sys.argv[1]) / "app/vision_badge/src/mimo_http.c").read_text()
start = source.index("int mimo_http_prepare_network(void)")
end = source.index("/* ========================================================================", start)
Path(sys.argv[2]).write_text(source[start:end])
def function(signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{": depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0: return source[start:pos + 1] + "\n"
    raise RuntimeError(signature)
constants = "\n".join(line for line in source.splitlines()
                      if line.startswith("#define MIMO_PROVISION_"))
functions = ["static int mimo_prepare_storage(", "int mimo_http_save_provision(",
             "static FILE *mimo_open_credential_file(", "static uint32_t mimo_provision_u32(",
             "static uint32_t mimo_provision_crc32(const uint8_t *data, size_t size)\n{",
             "static int mimo_decode_provision_record(",
             "static int mimo_decode_network_credentials(",
             "static int mimo_open_credentials(", "static int mimo_open_provision_record("]
Path(sys.argv[2]).with_name("mimo-provision-test.inc").write_text(
    constants + "\nstatic uint32_t mimo_provision_crc32(const uint8_t *, size_t);\n" +
    "\n".join(function(signature) for signature in functions))
PY
cc -D_DEFAULT_SOURCE -std=c11 -Wall -Wextra -Werror -g -O1 -no-pie \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$out" "$project/tools/agora/test_network.c" -o "$out/test_network"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_network"
temp=$(mktemp -d)
trap 'rm -f "$temp/wifi.tmp" "$temp/wifi.bin"; rmdir "$temp"' EXIT
cc -D_DEFAULT_SOURCE -DVISION_BADGE_PROVISION_TEST -std=c11 -Wall -Wextra -Werror \
  -g -O1 -no-pie -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$out" "$project/tools/agora/test_network.c" -o "$out/test_provision"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_provision" "$temp"
