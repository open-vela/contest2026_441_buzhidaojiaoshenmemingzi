# BK7258 R1 最小 NSH 板级目录

本目录只用于第一阶段目标：在 R1 的物理 CPU0（CP）上启动 openvela，
通过 UART0 进入 nsh。当前不编译 LCD、音频、摄像头、Wi-Fi、PSRAM
系统堆或队伍应用。

源码由比赛仓库 `board/bk7258-r1/` 维护，并通过队伍 manifest 链接到这里。
若工作区不是由 manifest 建立，可运行比赛仓库中的
`scripts/sync-openvela-port.sh --install`；构建前使用 `--check` 核对。

资料核对基线：

- R1 原理图：UART0 下载/日志使用 GPIO11 TX、GPIO10 RX；
- BK7258：三核，CPU0 为 CP，CPU1+CPU2 为 AP；
- AIDK beken_genie：CPU0 分区偏移 0x11000，大小 2856K；
- CPU0 工程配置频率为 240 MHz；
- 当前链接 SRAM 区间沿用公开 BK7258 bring-up 的保守值，须在 R1 实机确认。

构建：

    cd /home/alientek/openvela
    ./build.sh vendor/beken/boards/bk7258/bk7258-r1/configs/nsh/ --cmake -j2

生成 CPU0-only 测试镜像：

    python3 vendor/beken/boards/bk7258/bk7258-r1/tools/repack.py \
      --nuttx-bin cmake_out/bk7258-r1_nsh/nuttx.bin

打包仍需要博通 Bootloader 来完成上电和 XIP 交接，但不会包含或启动
ARMINO 的 CPU1/CPU2 应用。拿到板后先保留可恢复手段，再进行首次烧写。
