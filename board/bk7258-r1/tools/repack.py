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
EXPECTED_IMAGE_BYTES = CPU0_FLASH_OFFSET + CPU0_PARTITION_KIB * 1024

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


def run_command(command: list[str], cwd: Path, label: str) -> None:
    """Run one official Beken packaging step and surface its full output."""
    env = os.environ.copy()
    env.setdefault("TERM", "xterm")
    result = subprocess.run(
        command,
        cwd=cwd,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.stdout:
        print(result.stdout.rstrip())
    if result.returncode != 0:
        fail(f"{label} exited with status {result.returncode}")


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
        "--partition-csv",
        type=Path,
        help="override the BK7258 R1/AIDK partition definition",
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

    # ── 解析路径 ──────────────────────────────────────────────────
    # 把命令行参数中的路径展开（处理 ~ 等符号）、转绝对路径。

    nuttx_bin = args.nuttx_bin.expanduser().resolve()        # 用户编译出来的 nuttx.bin
    aidk_root = args.aidk_root.expanduser().resolve()         # Beken 官方 SDK 根目录
    idk_root = aidk_root / "bk_avdk/bk_idk"                  # IDK 子目录
    bootloader = (
        args.bootloader.expanduser().resolve()
        if args.bootloader
        else idk_root
        / "components/bk_libs/bk7258/bootloader/normal_bootloader/bootloader.bin"
    )                                                         # bootloader.bin 路径
    packager = idk_root / "tools/env_tools/beken_packager/beken_packager"  # Beken 打包器
    image_generator = idk_root / "tools/env_tools/beken_packager/cmake_Gen_image"
    image_generator_config = idk_root / "tools/env_tools/beken_packager/config.json"
    partition_generator = (
        idk_root / "tools/build_tools/part_table_tools/gen_bk7256partitions.py"
    )
    partition_csv = (
        args.partition_csv.expanduser().resolve()
        if args.partition_csv
        else aidk_root / "projects/beken_genie/config/bk7258/bk7258_partitions.csv"
    )
    output_dir = args.output_dir.expanduser().resolve()       # 输出目录

    # ── 步骤 1：校验输入文件是否存在 ──────────────────────────────
    # 如果 nuttx.bin 或 bootloader.bin 不存在，直接报错退出。

    for label, path in (
        ("NuttX binary", nuttx_bin),
        ("BK7258 bootloader", bootloader),
        ("BK7258 partition CSV", partition_csv),
        ("Beken partition generator", partition_generator),
        ("Beken image generator", image_generator),
        ("Beken image generator config", image_generator_config),
    ):
        if not path.is_file():
            fail(f"{label} not found: {path}")

    # ── 步骤 2：校验文件大小是否超过分区限制 ──────────────────────
    # nuttx.bin 不能超过 CPU0_MAX_RAW_BYTES（约 2.69MB，因为 34/32 CRC 编码）
    # bootloader.bin 不能超过 68KB

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

    # ── 步骤 3：准备工作目录，拷贝输入文件 ────────────────────────
    # 清空旧的输出目录，重新创建，然后准备 nuttx 和注入分区表后的 bootloader。

    if output_dir.exists():
        shutil.rmtree(output_dir)                              # 删除旧目录
    output_dir.mkdir(parents=True)                             # 创建新目录

    work_bootloader = output_dir / "bootloader.bin"            # 工作目录下的 bootloader
    work_app = output_dir / "app.bin"                          # 工作目录下的 app（nuttx 重命名）
    shutil.copy2(nuttx_bin, work_app)                          # 拷贝 nuttx.bin → app.bin

    # 官方 AIDK 不会直接使用原始 bootloader.bin。它先根据项目分区 CSV 生成
    # partition_bootloader.json，再用 cmake_Gen_image 将分区表注入 bootloader。
    # R1 使用 beken_genie 的 8 MiB 布局；虽然 M1 阶段不打包 CPU1/CPU2，仍保留
    # 官方完整分区描述，避免 Bootloader 与后续多核阶段使用不同的 Flash 布局。
    generated_partition_json = output_dir / "partition_bk7256_ota_a_new.json"
    partition_json = output_dir / "partition_bootloader.json"
    run_command(
        [
            sys.executable,
            str(partition_generator),
            str(partition_csv),
            f"--to-json={output_dir / 'partition-placeholder.json'}",
            "--flash-size=8MB",
            "--smode",
            "--smode-inseq=1,1,2,0,0,0",
        ],
        output_dir,
        "Beken partition generator",
    )
    if not generated_partition_json.is_file():
        fail(f"partition generator did not create: {generated_partition_json}")
    generated_partition_json.replace(partition_json)

    partition_data = json.loads(partition_json.read_text(encoding="utf-8"))
    partitions = {
        item["name"]: item for item in partition_data.get("part_table", [])
    }
    boot_part = partitions.get("bootloader", {})
    app_part = partitions.get("app", {})
    if (
        int(boot_part.get("offset", "-1"), 0) != 0
        or boot_part.get("len", "").upper() != "68K"
        or int(app_part.get("offset", "-1"), 0) != CPU0_FLASH_OFFSET
        or app_part.get("len", "").upper() != f"{CPU0_PARTITION_KIB}K"
    ):
        fail("generated partition table does not match the R1 CPU0 layout")

    run_command(
        [
            str(image_generator),
            "genfile",
            "-injsonfile",
            str(image_generator_config),
            "-infile",
            str(bootloader),
            "-outfile",
            str(work_bootloader),
            "-genjson",
            str(partition_json),
        ],
        output_dir,
        "Beken bootloader partition injection",
    )
    if not work_bootloader.is_file():
        fail(f"partition-injected bootloader was not created: {work_bootloader}")
    if work_bootloader.stat().st_size > BOOTLOADER_PARTITION_KIB * 1024:
        fail(
            "partition-injected bootloader exceeds its 68 KiB partition: "
            f"{work_bootloader.stat().st_size} bytes"
        )

    # ── 步骤 4：生成 configuration.json（Beken 打包器的输入）─────
    # 告诉 Beken 打包器：哪些文件放哪个分区、分区大小、起始地址。

    # "FreeRTOS" 是打包器的历史遗留字段，不代表 CPU0 用 FreeRTOS 系统。
    # 它只是告诉打包器按 FreeRTOS 镜像格式打包。
    package_config = {
        "magic": "FreeRTOS",
        "version": "0.1",
        "count": 2,                                           # 2 个 section：bootloader + app
        "section": [
            {
                "firmware": "bootloader.bin",
                "version": "2M.1220",
                "partition": "bootloader",
                "start_addr": "0x00000000",                   # bootloader 从 Flash 0x00000 开始
                "size": f"{BOOTLOADER_PARTITION_KIB}K",       # 68KB
            },
            {
                "firmware": "app.bin",
                "version": "2M.1220",
                "partition": "app",
                "start_addr": f"0x{CPU0_FLASH_OFFSET:08x}",   # nuttx 从 Flash 0x11000 开始
                "size": f"{CPU0_PARTITION_KIB}K",              # 2856KB
            },
        ],
    }

    config_path = output_dir / "configuration.json"
    config_path.write_text(
        json.dumps(package_config, indent=4) + "\n", encoding="utf-8"
    )

    # ── 生成 manifest.json（元数据，方便调试和记录）───────────────

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
            "packed_bytes": work_bootloader.stat().st_size,
            "packed_sha256": sha256(work_bootloader),
        },
        "partition_table": {
            "source": str(partition_csv),
            "generated": str(partition_json),
            "sha256": sha256(partition_json),
        },
        "cpu1_cpu2_included": False,                           # M1 阶段不包含 CPU1/CPU2 镜像
    }

    manifest_path = output_dir / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=4) + "\n", encoding="utf-8"
    )

    print(f"[repack] workspace : {workspace_root()}")
    print(f"[repack] NuttX CPU0: {nuttx_bin}")
    print(f"[repack] bootloader: {bootloader}")
    print(f"[repack] partitions: {partition_csv}")
    print(
        f"[repack] CPU0 size : {nuttx_bin.stat().st_size} / "
        f"{CPU0_MAX_RAW_BYTES} raw bytes"
    )
    print("[repack] CPU1/CPU2: omitted; NuttX will not start the AP cores")

    if args.prepare_only:                                     # --prepare-only 模式：只准备不打包
        print(f"[repack] prepared: {output_dir}")
        return

    # ── 步骤 5：调用 Beken 官方打包器 ────────────────────────────
    # beken_packager 读取 configuration.json，按分区表拼接 bootloader + app，
    # 加上 CRC(34/32) 校验码，输出 all_*.bin。

    if not packager.is_file():
        fail(f"Beken packager not found: {packager}")

    run_command(
        [str(packager), str(config_path)],
        output_dir,
        "Beken packager",
    )

    # ── 步骤 6：重命名输出文件为 all-app-openvela.bin ────────────
    # 打包器输出文件名不确定（如 all_20240827_143022.bin），
    # 这里找到唯一的 all_*.bin 文件，重命名为固定名称。

    generated_images = sorted(output_dir.glob("all_*.bin"))
    if len(generated_images) != 1:
        names = ", ".join(path.name for path in generated_images) or "none"
        fail(f"expected one all_*.bin from packager, found: {names}")

    generated = generated_images[0]

    final_image = output_dir / "all-app-openvela.bin"          # 最终镜像文件名
    generated.replace(final_image)                             # 重命名

    if final_image.stat().st_size != EXPECTED_IMAGE_BYTES:
        fail(
            f"unexpected final image size {final_image.stat().st_size}; "
            f"expected {EXPECTED_IMAGE_BYTES} bytes (end of CPU0 partition)"
        )

    # ── 更新 manifest.json，加入输出文件信息 ────────────────────

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
