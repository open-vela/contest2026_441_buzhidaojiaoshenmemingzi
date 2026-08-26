# BK7258 R1 队伍配置

目标硬件为大赛发放的“对话式 AI 开发套件 — 声网 & 博通集成”R1，
SoC 为 BK7258。本目录只保存队伍应用配置和接入说明，不复制尚未合并的
芯片层或板级 BSP。

## R1 与 bk7258-devkit 是否匹配

当前结论：**芯片型号一致，板级完全兼容尚未证实。可以用来学习和编译，不能
因此直接烧录 R1。** 官方硬件列表把 BK7258 DevKit 与声网套件分别列出，
PR #2 的作者称其目标为 AIDK AI toy board；这些信息不足以确定 PCB 修订一致。

| 核对项 | 当前候选 devkit 的依据 | R1 仍需核对 |
| --- | --- | --- |
| SoC / 运行核 | PR #332 为 BK7258 CPU0 启动实现 | 套件说明确认 BK7258；实物芯片修订和原厂多核固件分工未核对 |
| UART / 时钟 | `bk7258_lowputc.c` 使用 UART0、GPIO11/10、26 MHz 时钟假设 | PDF 只标出 USB 转 UART / UART 扩展，未确认芯片型号、网络名和引脚 |
| SRAM / Flash | `ld.script` 给当前镜像分配 336 KiB SRAM、1280 KiB Flash XIP 窗口 | 不等于全部物理容量；需 R1 分区表、bootloader 和保留区证明兼容 |
| PSRAM | PR 包含 PSRAM 实验；当前 `up_allocate_heap()` 只提供 SRAM 堆 | 实际型号/容量、初始化、DMA 可达性、是否加入系统堆 |
| 屏幕 / 摄像头 | 候选有 GC9D01 实验代码，不能据此确认 R1 屏幕型号 | PDF 标出双屏/单屏接口及 DVP，仍缺控制器/传感器型号、接线和时序 |
| 音频 / 电源 / 按键 | 候选有双麦实验代码 | R1 原理图、器件型号、供电使能与 GPIO；按键功能说明不能代替引脚表 |
| 镜像打包 / 恢复 | 候选工具依赖原厂 bootloader、app1、分区和线性 CRC | 获取匹配 R1 出厂版本的镜像、校准区保护和恢复步骤后再评审烧录方案 |

证据来源：[官方硬件列表](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/hardware_porting/supported_hardware.md)、
[套件 PDF](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/attachment/声网开发套件使用说明.pdf)
第 5 页板卡示意及第 10 页资料入口、下列固定 PR 源码。
PDF 指向的硬件资料为 [Beken AIDK v2.0.1](https://docs.bekencorp.com/arminodoc/bk_aidk/bk7258/zh_CN/v2.0.1/hw-reference/index.html)，
它比通用 BK7258 EVB 资料更贴近目标套件，但仍须对照实物修订。

如果只差引脚/器件组合，优先在队伍仓新增独立 R1 板级支持并映射到工作区；
只有芯片层行为确实不同才修改 `nuttx/arch/arm/src/bk7258/`。
不能仅把 `bk7258-devkit` 改名为 `bk7258-r1` 就宣称完成适配。

## 上游依赖

当前（2026-08-26）大赛分支尚未包含完整 BK7258 支持。公开实现分为两个
仍在审核中的 PR：

- `open-vela/nuttx#332`：CPU0 启动、UART0、中断、SysTick、堆和 NSH；
- `open-vela/vendor_beken#2`：`bk7258-devkit` 板级目录、16 MB PSRAM、
  GC9D01 显示、双麦音频实验驱动、链接及打包工具。

二者不是已发布接口，提交前必须重新核对 PR 状态和最新提交。R1 到板后还要
对照原理图验证 DVP 摄像头、双麦、双屏、按键、振动、NFC、SD NAND 和电源引脚，
不能仅凭外观相似直接认定与公开 DevKit 完全一致。

## 映射关系

队伍 manifest 仅把应用配置链接到预期上游目录：

```text
board/bk7258-r1/configs/contest2026_441_vision_badge
  -> vendor/beken/boards/bk7258/bk7258-devkit/configs/contest2026_441_vision_badge
```

当上游 `bk7258-devkit` BSP 可用后，该配置可通过以下入口构建：

```bash
./build.sh vendor/beken/boards/bk7258/bk7258-devkit/configs/contest2026_441_vision_badge/ --cmake
```

当前配置继承公开 PR #2 的最小 NSH 内存布局并启用 `vision_badge`。摄像头、
网络和完整音频驱动尚未进入配置。2026-08-26 在固定候选 PR 的 Ubuntu VM
工作区完成了 NSH 与队伍应用两套固件构建，见[构建验证](../../docs/progress/构建验证-20260826.md)。
这只验证了编译和链接，不代表已经通过 R1 启动、驱动或应用实机验收。

配套候选提交见 [candidate-prs.xml](candidate-prs.xml)。这是供新实验工作区
审阅的 local manifest 模板，不会由队伍主 manifest 自动包含。当前 VM 已有
`.repo/local_manifests/441-bk7258.xml` 固定相同候选并保留队伍快照，不要重复安装。

## 学习时对应的源码入口

下列路径相对于完整工作区 `/home/alientek/openvela`，不是相对于本队仓。

| 层级 | 文件或目录 | 回答的问题 |
| --- | --- | --- |
| 芯片启动 | `nuttx/arch/arm/src/bk7258/bk7258_start.c` | bootloader 交接后如何设置 CPU 状态、初始化 C 运行环境并进入内核 |
| 芯片串口 | `nuttx/arch/arm/src/bk7258/bk7258_lowputc.c`、`bk7258_serial.c` | 早期字符输出与完整串口驱动分别在哪里 |
| 芯片内存 | `nuttx/arch/arm/src/bk7258/bk7258_allocateheap.c` | `malloc` 使用的初始系统堆从哪里来 |
| 通用内核 | `nuttx/sched/init/nx_start.c`、`nx_bringup.c` | 调度、系统初始化和初始应用如何建立 |
| 板级配置 | `vendor/beken/boards/bk7258/bk7258-devkit/configs/nsh/defconfig` | 选择哪块板、哪些功能和哪个初始应用 |
| 链接布局 | `vendor/beken/boards/bk7258/bk7258-devkit/scripts/ld.script` | 指令、已初始化变量、零初始化变量放在哪里 |
| 板级初始化 | `vendor/beken/boards/bk7258/bk7258-devkit/src/` 下的 `bk7258_boardinitialize.c`、`bk7258_appinit.c`、`bk7258_bringup.c` | 早期板级钩子和内核建立后的外设初始化如何分工 |
| 命令行 | `apps/system/nsh/nsh_main.c`、`apps/nshlib/` | `nsh>` 如何启动、解析并执行命令 |
| 队伍应用 | `contest2026_441_buzhidaojiaoshenmemingzi/app/vision_badge/src/` | `vision_badge_main.c` 处理命令，`workflow.c` 调用采集/识别/反馈接口 |

`Kconfig` 定义有哪些配置项及依赖；`defconfig` 保存所选配置的精简基线；
输出目录的 `.config` 是解析默认值/依赖后的完整配置；`CMakeLists.txt` 决定
如何编译和链接源码；`ld.script` 决定最终地址。它们不是同一种配置文件。

当前 CPU0 镜像的源码调用关系（不是 R1 实测启动日志）：

```text
原厂 bootloader 交接 → __start → bk7258_cstart
  → 早期 UART / 看门狗处理 / 清 .bss / 拷贝 .data
  → arm_boardinitialize（当前是空钩子）→ nx_start
  → 系统 bring-up 中 board_late_initialize → bk7258_bringup（当前主要挂载 /proc）
  → 启动配置指定的 nsh_main
  → 输入 vision_badge 命令后才进入队伍应用入口
```

当前 `ld.script` 的 SRAM 区间为 `[0x28010000, 0x28064000)`，共 336 KiB；
其中还要容纳 `.data`、`.bss`、idle 栈和系统堆。任务栈及应用动态缓冲会继续
占用运行期内存。Flash 的 `0x02010000` 是 CPU XIP 映射地址，不能当成直接
传给烧录器的文件偏移。没有验证 PSRAM 入堆前，不能把其标称容量算成 `malloc`
已经可用的空间。

任何 Wi-Fi 密码、API Key、Token 或私钥都不得写入 defconfig。
