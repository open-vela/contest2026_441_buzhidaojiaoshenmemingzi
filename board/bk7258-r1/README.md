# BK7258 R1 板级层

目标硬件为大赛发放的“对话式 AI 开发套件 — 声网 & 博通集成”R1，SoC 为
BK7258。本目录现在既是队伍仓中的板级源码，也是完整 OpenVela 工作区
`vendor/beken/boards/bk7258/bk7258-r1/` 的来源；manifest 会把整个目录链接过去。

## 它和芯片层的边界

- `board/bk7258-r1/` 回答“这块 R1 板如何组合 BK7258”：选用的时钟值、可用
  内存窗口、链接布局、启动后板级初始化、NSH/应用两套配置及镜像打包。
- `porting/nuttx/arch/arm/src/bk7258/` 回答“BK7258 这颗芯片如何运行 NuttX”：
  启动入口、中断控制器、UART、SysTick 和系统堆。当前保存的是候选芯片层的
  中文学习注释镜像，可用同步脚本放回完整工作区。
- `app/vision_badge/` 回答“产品做什么”：采图、识别、音频和反馈流程；它不应
  直接操作 BK7258 寄存器。

因此，这两个底层目录都应该由本队仓维护。板级目录可以直接通过 manifest
链接；芯片目录属于另一个 Git 项目的已跟踪路径，不能整目录链接覆盖，所以用
`scripts/sync-openvela-port.sh` 精确同步本队维护的 11 个文件。

## R1 与公开 bk7258-devkit 的关系

当前结论：**SoC 相同，板级不能视为完全兼容。** 本目录从公开候选实现提取
最小 CPU0/NSH 骨架，再按 R1 原理图和 AIDK 工程建立独立名称；目前能证明的是
构建和打包通过，尚无 R1 实机启动证据。

| 核对项 | 当前 R1 最小目录 | 仍待实机确认 |
| --- | --- | --- |
| 运行核 | CPU0，AIDK 术语为 CP；CPU1/CPU2 暂不启动 | 原厂 bootloader 的实际交接状态 |
| UART0 | 原理图对应 GPIO11 TX、GPIO10 RX | 串口芯片、波特率与启动日志 |
| 时钟 | 按 AIDK CPU0 配置暂取 240 MHz | bootloader 交接后的真实 CPU 时钟 |
| SRAM | 暂沿用公开 bring-up 的保守窗口 | R1 保留区、共享区和可扩展范围 |
| Flash | CPU0 分区物理偏移暂取 `0x11000` | 出厂分区、校准区和恢复流程 |
| 外设 | NSH 配置不启用屏、相机、音频、Wi-Fi、PSRAM 堆 | 器件 ID、引脚、供电、DMA 与驱动 |

公开依赖仍是待合并的 `open-vela/nuttx#332` 和 `open-vela/vendor_beken#2`；固定
提交见 [candidate-prs.xml](candidate-prs.xml)。它们是基线来源，不是官方已合并
能力，也不替代本队 R1 验证。

## 两个 defconfig

- `configs/nsh/defconfig`：当前主线，只编译最小 CPU0 + UART0 + NSH；用于拿到
  板后的第一次启动验证。
- `configs/contest2026_441_vision_badge/defconfig`：下一阶段，在同一 R1 板级
  基础上把 `vision_badge` 编进同一个 NuttX 镜像。它不是单独的应用固件。

两者都是“整机配置清单”。选择不同 defconfig 会得到不同的完整固件；不存在
一个独立板级镜像再动态加载应用镜像的流程。当前先让 NSH 镜像跑通，再切换
应用配置逐项增加能力。

## 当前构建、打包与同步

```bash
cd /home/alientek/openvela/contest2026_441_buzhidaojiaoshenmemingzi

# 只读确认队伍仓与 OpenVela 工作区内容一致
bash scripts/sync-openvela-port.sh --check

# 默认构建最小 NSH
bash scripts/build.sh nsh -j2

# 以后再构建带应用的完整镜像
bash scripts/build.sh app -j2

# 生成包含 Beken bootloader + NuttX CPU0 的待审镜像（不会烧写）
python3 ../vendor/beken/boards/bk7258/bk7258-r1/tools/repack.py \
  --nuttx-bin ../cmake_out/bk7258-r1_nsh/nuttx.bin
```

2026-08-27 已完成 `bk7258-r1:nsh` 构建和 CPU0-only 打包；这只是构建门禁，
不是 R1 启动、NSH 或烧录验证。首次烧写前仍需保存恢复路径、出厂镜像、分区与
校准数据。

## 从哪里开始阅读

按下面顺序读，不必先钻进每个寄存器：

1. `configs/nsh/defconfig`：这次构建选择了什么。
2. `scripts/ld.script`：程序各部分最终放在哪里。
3. `porting/nuttx/.../bk7258_start.c`：bootloader 交接后如何进入 NuttX。
4. `src/bk7258_boardinitialize.c`：调度器启动前的板级钩子。
5. `src/bk7258_appinit.c` 与 `src/bk7258_bringup.c`：内核起来后如何注册板上设备。
6. `app/vision_badge/src/vision_badge_main.c`：输入命令后才进入的应用入口。

`Kconfig` 定义“有哪些选项”；`defconfig` 保存“本次选了哪些”；构建目录里的
`.config` 是依赖展开后的完整结果；`CMakeLists.txt` 决定编译哪些源码；
`ld.script` 决定链接地址。
