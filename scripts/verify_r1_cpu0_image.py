#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Verify the structure and CPU0 vectors of an R1 all-app image."""

from __future__ import annotations

import argparse
from pathlib import Path
import struct


APP_OFFSET = 0x11000
SRAM_START = 0x28010000
SRAM_END = 0x28064000
FLASH_START = 0x02010000
FLASH_END = 0x02150000


def read(path: Path) -> bytes:
    try:
        return path.read_bytes()
    except OSError as exc:
        raise SystemExit(f"cannot read {path}: {exc}") from exc


def decode_beken_crc(data: bytes) -> bytes:
    if len(data) % 34 != 0:
        raise SystemExit(
            f"encoded partition size {len(data)} is not a multiple of 34"
        )

    return b"".join(data[index : index + 32] for index in range(0, len(data), 34))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nuttx-bin", type=Path, required=True)
    parser.add_argument("--encoded-app", type=Path, required=True)
    parser.add_argument("--final-image", type=Path, required=True)
    parser.add_argument("--packed-bootloader", type=Path, required=True)
    parser.add_argument("--source-bootloader", type=Path, required=True)
    args = parser.parse_args()

    raw = read(args.nuttx_bin)
    encoded_app = read(args.encoded_app)
    final = read(args.final_image)
    packed_boot = read(args.packed_bootloader)
    source_boot = read(args.source_bootloader)

    if len(raw) < 8:
        raise SystemExit("nuttx.bin is too small to contain an ARM vector table")

    encoded_boot = final[:APP_OFFSET]
    decoded_boot = decode_beken_crc(encoded_boot)
    decoded_app = decode_beken_crc(encoded_app)
    initial_sp, reset = struct.unpack_from("<II", raw)

    checks = {
        "source_bootloader_is_prefix": packed_boot.startswith(source_boot),
        "partition_data_appended": len(packed_boot) > len(source_boot),
        "decoded_bootloader_matches_injected": decoded_boot.startswith(packed_boot),
        "encoded_app_at_0x11000": (
            final[APP_OFFSET : APP_OFFSET + len(encoded_app)] == encoded_app
        ),
        "decoded_app_matches_nuttx": decoded_app[: len(raw)] == raw,
        "final_size_matches_partitions": len(final) == APP_OFFSET + len(encoded_app),
        "initial_sp_in_r1_sram": SRAM_START <= initial_sp <= SRAM_END,
        "reset_vector_in_cpu0_flash": FLASH_START <= (reset & ~1) < FLASH_END,
        "reset_vector_is_thumb": bool(reset & 1),
    }

    print(f"vectors: initial_sp=0x{initial_sp:08x} reset=0x{reset:08x}")
    for name, passed in checks.items():
        print(f"{name}={passed}")

    if not all(checks.values()):
        failed = ", ".join(name for name, passed in checks.items() if not passed)
        raise SystemExit(f"R1 image verification failed: {failed}")

    print("R1 CPU0 all-app image verification passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
