#!/usr/bin/env python3
"""Build a BK7258 R1 CPU0-only image containing the openvela NSH binary.

The image contains the Beken normal bootloader and the NuttX CPU0 image.
It deliberately omits ARMINO CPU1/CPU2 application images.  NuttX milestone
1 does not release those cores.
"""

###############################################################################
# 文件角色：打包工具（把 nuttx.bin 打包成 R1 板子可烧录的镜像）
#
# 通俗理解：编译出来的 nuttx.bin 只是一个"裸程序"，不能直接烧进芯片。
# 需要和 bootloader 拼在一起，加上分区表信息和 CRC 校验，才能被 bootloader
# 正确识别和加载。这个脚本就是做这件事的。
#
# 打包流程：
#   1. 找到 nuttx.bin 和 bootloader.bin
#   2. 校验文件大小是否超过分区限制
#   3. 生成 configuration.json（告诉 Beken 打包器：哪个文件放哪个分区）
#   4. 调用 Beken 官方的打包器（beken_packager）
#   5. 输出 all-app-openvela.bin → 这就是可以烧录的最终镜像
#
# 分区配置（与 bk_package.json 的分区表一致）：
#   ┌────────────┬──────────┬──────────┬────────────────────────┐
#   │ 分区        │ 物理偏移  │ 大小      │ 内容                   │
#   ├────────────┼──────────┼──────────┼────────────────────────┤
#   │ bootloader │ 0x00000  │ 68KB     │ 原厂闭源 bootloader     │
#   │ app (CPU0) │ 0x11000  │ 2856KB   │ nuttx.bin（我们的程序）  │
#   └────────────┴──────────┴──────────┴────────────────────────┘
#
# ⚠️ CPU0 分区限制：2856KB 物理 = 2856 * 1024 * 32/34 = ~2.69MB 有效数据
#   因为 Beken Flash 控制器每 32 字节数据加 2 字节 CRC(34/32)，实际有效数据
#   只有 32/34 = 94.1%。
#
# 使用方法：
#   python tools/repack.py --nuttx-bin cmake_out/bk7258-r1_nsh/nuttx.bin
#
# 参考：scripts/ld.script（链接脚本，Flash 起点 0x02010000）
#       include/board.h（BOARD_FLASH_BASE = 0x02000000）
###############################################################################

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


# ── 分区参数 ────────────────────────────────────────────────────────────────

CPU0_FLASH_OFFSET = 0x11000      # CPU0 app 分区的物理 Flash 偏移（68KB 之后）
CPU0_PARTITION_KIB = 2856        # CPU0 app 分区大小：2856 KiB
BOOTLOADER_PARTITION_KIB = 68    # bootloader 分区大小：68 KiB

# Beken Flash 控制器使用 34/32 CRC 编码：每 32 字节有效数据占用 34 字节物理空间。
# 所以有效数据上限 = 分区大小 * 32/34
CPU0_MAX_RAW_BYTES = CPU0_PARTITION_KIB * 1024 * 32 // 34


def fail(message: str) -> None:
    print(f"[repack] error: {message}", file=sys.stderr)
    raise SystemExit(1)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def workspace_root() -> Path:
    """Locate the full OpenVela checkout even when this board is linked."""
    script = Path(__file__).resolve()
    for candidate in script.parents:
        if (
            (candidate / "build.sh").is_file()
            and (candidate / "nuttx").is_dir()
            and (candidate / "vendor").is_dir()
        ):
            return candidate
    fail(f"cannot locate OpenVela workspace above: {script}")


def parse_args() -> argparse.Namespace:
    root = workspace_root()
    aidk = root / "beken_reference/sdk/bk_aidk"

    parser = argparse.ArgumentParser(
        description="Pack openvela NuttX as the BK7258 R1 CPU0 image"
    )
    parser.add_argument(
        "--nuttx-bin",
        type=Path,
        default=root / "cmake_out/bk7258-r1_nsh/nuttx.bin",
        help="explicit NuttX raw binary (default: R1 NSH build output)",
    )
    parser.add_argument(
        "--aidk-root",
        type=Path,
        default=aidk,
        help="bk_aidk checkout used only for the bootloader and packager",
    )
    parser.add_argument(
        "--bootloader",
        type=Path,
        help="override the BK7258 normal bootloader binary",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(__file__).resolve().parent / "bk_repack_work",
        help="generated files directory",
    )
    parser.add_argument(
        "--prepare-only",
        action="store_true",
        help="validate and prepare inputs/configuration without invoking packager",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    nuttx_bin = args.nuttx_bin.expanduser().resolve()
    aidk_root = args.aidk_root.expanduser().resolve()
    idk_root = aidk_root / "bk_avdk/bk_idk"
    bootloader = (
        args.bootloader.expanduser().resolve()
        if args.bootloader
        else idk_root
        / "components/bk_libs/bk7258/bootloader/normal_bootloader/bootloader.bin"
    )
    packager = idk_root / "tools/env_tools/beken_packager/beken_packager"
    output_dir = args.output_dir.expanduser().resolve()

    # ── 步骤 1：校验输入文件是否存在 ──────────────────────────────────────

    for label, path in (
        ("NuttX binary", nuttx_bin),
        ("BK7258 bootloader", bootloader),
    ):
        if not path.is_file():
            fail(f"{label} not found: {path}")

    # ── 步骤 2：校验文件大小是否超过分区限制 ──────────────────────────────

    if nuttx_bin.stat().st_size > CPU0_MAX_RAW_BYTES:
        fail(
            f"NuttX is {nuttx_bin.stat().st_size} bytes, exceeding the "
            f"CPU0 raw payload limit {CPU0_MAX_RAW_BYTES} bytes"
        )

    if bootloader.stat().st_size > BOOTLOADER_PARTITION_KIB * 1024:
        fail(
            f"bootloader is larger than {BOOTLOADER_PARTITION_KIB} KiB: "
            f"{bootloader.stat().st_size} bytes"
        )

    # ── 步骤 3：准备工作目录，拷贝输入文件 ────────────────────────────────

    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True)

    work_bootloader = output_dir / "bootloader.bin"
    work_app = output_dir / "app.bin"
    shutil.copy2(bootloader, work_bootloader)
    shutil.copy2(nuttx_bin, work_app)      # nuttx.bin 重命名为 app.bin

    # ── 步骤 4：生成 configuration.json（Beken 打包器的输入）─────────────
    # 告诉打包器：bootloader 放 0x00000，nuttx/app 放 0x11000

    # FreeRTOS is the packager's legacy image magic, not an assertion that
    # the CPU0 payload uses FreeRTOS.
    package_config = {
        "magic": "FreeRTOS",
        "version": "0.1",
        "count": 2,
        "section": [
            {
                "firmware": "bootloader.bin",
                "version": "2M.1220",
                "partition": "bootloader",
                "start_addr": "0x00000000",
                "size": f"{BOOTLOADER_PARTITION_KIB}K",
            },
            {
                "firmware": "app.bin",
                "version": "2M.1220",
                "partition": "app",
                "start_addr": f"0x{CPU0_FLASH_OFFSET:08x}",
                "size": f"{CPU0_PARTITION_KIB}K",
            },
        ],
    }

    config_path = output_dir / "configuration.json"
    config_path.write_text(
        json.dumps(package_config, indent=4) + "\n", encoding="utf-8"
    )

    manifest = {
        "mode": "bk7258-r1-openvela-cpu0-only",
        "cpu0": {
            "source": str(nuttx_bin),
            "bytes": nuttx_bin.stat().st_size,
            "sha256": sha256(nuttx_bin),
            "flash_offset": f"0x{CPU0_FLASH_OFFSET:x}",
            "partition_kib": CPU0_PARTITION_KIB,
            "max_raw_bytes": CPU0_MAX_RAW_BYTES,
        },
        "bootloader": {
            "source": str(bootloader),
            "bytes": bootloader.stat().st_size,
            "sha256": sha256(bootloader),
        },
        "cpu1_cpu2_included": False,
    }

    manifest_path = output_dir / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=4) + "\n", encoding="utf-8"
    )

    print(f"[repack] workspace : {workspace_root()}")
    print(f"[repack] NuttX CPU0: {nuttx_bin}")
    print(f"[repack] bootloader: {bootloader}")
    print(
        f"[repack] CPU0 size : {nuttx_bin.stat().st_size} / "
        f"{CPU0_MAX_RAW_BYTES} raw bytes"
    )
    print("[repack] CPU1/CPU2: omitted; NuttX will not start the AP cores")

    if args.prepare_only:
        print(f"[repack] prepared: {output_dir}")
        return

    # ── 步骤 5：调用 Beken 官方打包器 ────────────────────────────────────
    # beken_packager 读取 configuration.json，按分区表拼接 bootloader + app，
    # 加上 CRC(34/32) 校验码，输出 all_*.bin。

    if not packager.is_file():
        fail(f"Beken packager not found: {packager}")

    env = os.environ.copy()
    env.setdefault("TERM", "xterm")
    result = subprocess.run(
        [str(packager), str(config_path)],
        cwd=output_dir,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.stdout:
        print(result.stdout.rstrip())
    if result.returncode != 0:
        fail(f"Beken packager exited with status {result.returncode}")

    # ── 步骤 6：重命名输出文件为 all-app-openvela.bin ────────────────────

    generated_images = sorted(output_dir.glob("all_*.bin"))
    if len(generated_images) != 1:
        names = ", ".join(path.name for path in generated_images) or "none"
        fail(f"expected one all_*.bin from packager, found: {names}")

    generated = generated_images[0]

    final_image = output_dir / "all-app-openvela.bin"
    generated.replace(final_image)
    manifest["output"] = {
        "path": str(final_image),
        "bytes": final_image.stat().st_size,
        "sha256": sha256(final_image),
    }
    manifest_path.write_text(
        json.dumps(manifest, indent=4) + "\n", encoding="utf-8"
    )

    print(f"[repack] image     : {final_image}")
    print(f"[repack] sha256    : {manifest['output']['sha256']}")
    print("[repack] this command only packs; it does not flash or erase the board")


if __name__ == "__main__":
    main()
