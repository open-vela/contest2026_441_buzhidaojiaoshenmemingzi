#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Check whether a BK7258 R1 OpenVela AP image fits the CP/AP contract."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import struct
import subprocess
import tempfile


AP_FLASH_START = 0x02150000
DEFAULT_AP_FLASH_SIZE = 0x00110000
SPINLOCK_START = 0x28000000
SPINLOCK_END = 0x28010000
AP_RAM_START = 0x28010000
AP_RAM_END = 0x28064000
CP_RAM_START = 0x28064000
CP_RAM_END = 0x2809F800
SWAP_START = 0x2809F800
SWAP_END = 0x280A0000
MAX_SPINLOCK_FOOTPRINT = 0x1000


@dataclass(frozen=True)
class LoadSegment:
    vstart: int
    vend: int
    pstart: int
    pend: int
    filesz: int
    memsz: int


def run(tool: str, *args: object) -> str:
    command = [tool, *(str(arg) for arg in args)]
    try:
        return subprocess.run(
            command,
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        ).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit(f"cannot run {' '.join(command)}: {exc}") from exc


def parse_entry(header: str) -> int:
    match = re.search(r"Entry point address:\s*(0x[0-9a-fA-F]+)", header)
    if not match:
        raise SystemExit("cannot find the ELF entry point")
    return int(match.group(1), 16)


def parse_load_segments(program_headers: str) -> list[LoadSegment]:
    segments: list[LoadSegment] = []
    number = r"(?:0x)?[0-9a-fA-F]+"
    pattern = re.compile(
        rf"^\s*LOAD\s+{number}\s+"
        rf"({number})\s+({number})\s+"
        rf"({number})\s+({number})\s+",
        re.MULTILINE,
    )
    for match in pattern.finditer(program_headers):
        vstart = int(match.group(1), 16)
        pstart = int(match.group(2), 16)
        filesz = int(match.group(3), 16)
        memsz = int(match.group(4), 16)
        segments.append(
            LoadSegment(
                vstart=vstart,
                vend=vstart + memsz,
                pstart=pstart,
                pend=pstart + filesz,
                filesz=filesz,
                memsz=memsz,
            )
        )
    if not segments:
        raise SystemExit("cannot find any ELF LOAD segment")
    declared = len(re.findall(r"^\s*LOAD\b", program_headers, re.MULTILINE))
    if len(segments) != declared:
        raise SystemExit(
            f"parsed {len(segments)} of {declared} ELF LOAD segments"
        )
    return segments


def parse_symbols(symbol_table: str) -> dict[str, int]:
    symbols: dict[str, int] = {}
    pattern = re.compile(
        r"^([0-9a-fA-F]+)\s+[A-Za-z?]\s+(\S+)$", re.MULTILINE
    )
    for match in pattern.finditer(symbol_table):
        symbols[match.group(2)] = int(match.group(1), 16)
    return symbols


def containing_region(start: int, end: int, flash_end: int) -> str | None:
    # NuttX keeps scheduler/IRQ spinlock state in the dedicated shared SRAM
    # window, so a small SPINLOCK LOAD segment is expected. Any address gap
    # not named below is deliberately rejected.

    regions = {
        "AP_FLASH": (AP_FLASH_START, flash_end),
        "SPINLOCK": (SPINLOCK_START, SPINLOCK_END),
        "AP_RAM": (AP_RAM_START, AP_RAM_END),
    }
    for name, (region_start, region_end) in regions.items():
        if region_start <= start and end <= region_end:
            return name
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--readelf", default="arm-none-eabi-readelf")
    parser.add_argument("--nm", default="arm-none-eabi-nm")
    parser.add_argument("--objcopy", default="arm-none-eabi-objcopy")
    parser.add_argument(
        "--flash-size",
        type=lambda value: int(value, 0),
        default=DEFAULT_AP_FLASH_SIZE,
        help=(
            "CRC-decoded raw AP XIP capacity, not the encoded physical "
            "partition size; default: 0x110000"
        ),
    )
    args = parser.parse_args()
    flash_end = AP_FLASH_START + args.flash_size

    try:
        raw = args.bin.read_bytes()
    except OSError as exc:
        raise SystemExit(f"cannot read {args.bin}: {exc}") from exc
    if len(raw) < 16:
        raise SystemExit("AP binary is too small to contain the first ARM vectors")

    with tempfile.TemporaryDirectory(prefix="verify-r1-ap-") as tempdir:
        rebuilt_path = Path(tempdir) / "from-elf.bin"
        run(args.objcopy, "-O", "binary", args.elf, rebuilt_path)
        rebuilt = rebuilt_path.read_bytes()

    header = run(args.readelf, "-h", args.elf)
    attributes = run(args.readelf, "-A", args.elf)
    program_headers = run(args.readelf, "-W", "-l", args.elf)
    symbols = parse_symbols(run(args.nm, "-n", args.elf))
    entry = parse_entry(header)
    initial_sp, reset, nmi, hardfault = struct.unpack_from("<IIII", raw)
    segments = parse_load_segments(program_headers)

    machine_arm = bool(re.search(r"Machine:\s+ARM\b", header))
    elf32 = bool(re.search(r"Class:\s+ELF32\b", header))
    little_endian = "2's complement, little endian" in header
    executable = bool(re.search(r"Type:\s+EXEC\b", header))
    cpu_v8m = "Tag_CPU_arch: v8-M.mainline" in attributes
    hard_float = "Tag_ABI_VFP_args: VFP registers" in attributes
    fpv5 = "Tag_FP_arch: FPv5" in attributes
    segment_regions = [
        containing_region(segment.vstart, segment.vend, flash_end)
        for segment in segments
    ]
    heap_start_name = next(
        (name for name in ("_sheap", "__heap_start") if name in symbols), None
    )
    heap_end_name = next(
        (name for name in ("_eheap", "__heap_end") if name in symbols), None
    )
    runtime_limits = {
        name: symbols[name]
        for name in (heap_start_name, heap_end_name, "_estack",
                     "_bk7258_cpu2_probe_stack_top")
        if name is not None and name in symbols
    }
    heap_start = symbols.get(heap_start_name) if heap_start_name else None
    heap_end = symbols.get(heap_end_name) if heap_end_name else None
    load_vma_ranges = sorted(
        (segment.vstart, segment.vend)
        for segment in segments
        if segment.memsz > 0
    )

    checks = {
        "elf_matches_raw_binary": rebuilt == raw,
        "elf_class_is_32_bit": elf32,
        "elf_is_little_endian": little_endian,
        "elf_type_is_executable": executable,
        "elf_machine_is_arm": machine_arm,
        "elf_entry_is_vector_base_or_reset": (
            entry == AP_FLASH_START or (entry & ~1) == (reset & ~1)
        ),
        "cpu_arch_is_v8m_mainline": cpu_v8m,
        "float_abi_uses_vfp_registers": hard_float,
        "fp_arch_is_fpv5": fpv5,
        "raw_fits_ap_flash_partition": len(raw) <= args.flash_size,
        "initial_sp_in_ap_ram": AP_RAM_START <= initial_sp <= AP_RAM_END,
        "reset_vector_in_ap_flash": AP_FLASH_START <= (reset & ~1) < flash_end,
        "reset_vector_is_thumb": bool(reset & 1),
        "nmi_vector_in_ap_flash": AP_FLASH_START <= (nmi & ~1) < flash_end,
        "nmi_vector_is_thumb": bool(nmi & 1),
        "hardfault_vector_in_ap_flash": (
            AP_FLASH_START <= (hardfault & ~1) < flash_end
        ),
        "hardfault_vector_is_thumb": bool(hardfault & 1),
        "all_load_segments_in_ap_regions": all(segment_regions),
        "load_segment_vmas_do_not_overlap": all(
            previous_end <= current_start
            for (_, previous_end), (current_start, _) in zip(
                load_vma_ranges, load_vma_ranges[1:]
            )
        ),
        "load_payloads_in_ap_flash": all(
            segment.filesz == 0
            or AP_FLASH_START <= segment.pstart <= segment.pend <= flash_end
            for segment in segments
        ),
        "spinlock_footprint_is_bounded": sum(
            segment.memsz
            for segment, region in zip(segments, segment_regions)
            if region == "SPINLOCK"
        ) <= MAX_SPINLOCK_FOOTPRINT,
        "no_load_segment_in_cp_ram": all(
            segment.vend <= CP_RAM_START or segment.vstart >= CP_RAM_END
            for segment in segments
        ),
        "no_load_segment_in_swap": all(
            segment.vend <= SWAP_START or segment.vstart >= SWAP_END
            for segment in segments
        ),
        "heap_start_symbol_present": heap_start is not None,
        "heap_end_symbol_present": heap_end is not None,
        "runtime_heap_and_stack_in_ap_ram": all(
            AP_RAM_START <= address <= AP_RAM_END
            for address in runtime_limits.values()
        ),
        "runtime_heap_order_is_valid": (
            heap_start is not None
            and heap_end is not None
            and heap_start < heap_end
        ),
    }

    print(
        f"vectors: initial_sp=0x{initial_sp:08x} "
        f"reset=0x{reset:08x} nmi=0x{nmi:08x} "
        f"hardfault=0x{hardfault:08x} entry=0x{entry:08x}"
    )
    print(f"raw_size=0x{len(raw):x} ap_raw_capacity=0x{args.flash_size:x}")
    for segment, region in zip(segments, segment_regions):
        print(
            f"LOAD VMA=0x{segment.vstart:08x}-0x{segment.vend:08x} "
            f"LMA=0x{segment.pstart:08x}-0x{segment.pend:08x} "
            f"filesz=0x{segment.filesz:x} memsz=0x{segment.memsz:x} "
            f"region={region or 'INVALID'}"
        )
    for name, address in runtime_limits.items():
        print(f"symbol {name}=0x{address:08x}")
    for name, passed in checks.items():
        print(f"{name}={passed}")

    if not all(checks.values()):
        failed = ", ".join(name for name, passed in checks.items() if not passed)
        raise SystemExit(f"R1 AP image verification failed: {failed}")

    print("R1 AP image verification passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
