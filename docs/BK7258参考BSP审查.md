# BK7258 R1 竞赛仓库源码审查报告（2026-09-16）

## 1. 审查目的与结论

本报告为 R1 的 BSP 整合提供源码依据，重点检查 CPU0、AP、CPU2 的启动、内存布局、中断、系统节拍、板级初始化、RPTUN、构建和镜像生成。审查以实际源码、配置、链接脚本和构建脚本为准，不以 README 中的功能声明代替源码证据。

结论如下：

1. **主参考应固定为 `contest2026_135_yongwangzhiqian` 的提交 `7079493e73159e00c1b03e60bee6ee69845751cd`。** 该版本已经形成 CPU0 NuttX、AP 上的双核 NuttX、共享内存、Mailbox、RPTUN、生命周期监督、板级初始化及构建打包的一套完整实现。
2. **上述代码必须按耦合单元移植，不能只摘一两个函数。** 尤其是内存布局、AP 生命周期、SMP/IPI、RPTUN 共享 ABI、两侧配置和链接脚本必须保持一致。
3. `contest2026_264_VelaSight` 当前源码是 **CPU0 Armino + 物理 CPU1 上的单核 NuttX**。它没有启用 AP SMP，也没有启用 RPTUN，不能作为“CPU0 和 AP 都运行 OpenVela”的主 BSP；它可作为 Wi-Fi、LCD、Camera、Audio、SDIO 和 Mailbox 外设驱动的辅助对照。
4. `open-vela/nuttx` PR #332 的分支仅完成 CPU0 NSH/UART0 的最小移植，而且 PR 仍未合入。它适合作为 CPU0 启动顺序的独立对照，不应覆盖 135 仓库中更完整的多核实现。
5. 135 仓库有真实 AIDK 板验证记录，但不能据此宣称全部功能已经完成。现有证据覆盖 AP/CPU2/RPMsg、双 framebuffer、基础音频采集、运动传感器请求、NFC 无卡路径、看门狗恢复等有限项目；Wi-Fi、Camera、音频质量、深睡和全部异常恢复仍需在本项目硬件上重新验证。

## 2. 固定参考版本与许可证证据

| 参考来源 | 固定提交 | 提交时间 | 许可证和来源证据 | 本次定位 |
|---|---|---|---|---|
| `open-vela/contest2026_135_yongwangzhiqian`，分支 `dev-ai-contest-2026` | `7079493e73159e00c1b03e60bee6ee69845751cd` | 2026-09-16 18:58:35 +0800 | 根目录 `LICENSE` 为 Apache-2.0；`SOURCE_PROVENANCE.md` 记录第三方来源；manifest 固定 Beken SDK | 全 OpenVela 多核 BSP 主参考 |
| `Embracecactus/bk_avdk_smp` | `cb080de1655d579c7593ecf504c440997c4c137b` | 由 135 manifest 固定 | 该提交根 `LICENSE` 为 Apache-2.0 | Beken SDK 固定基线 |
| `open-vela/contest2026_264_VelaSightsuixingAIzhinengyanjing`，分支 `dev-ai-contest-2026` | `ba87d984a1e08bbcd5c09734700a3d4574739158` | 2026-09-16 10:30:33 +0800 | 根 `LICENSE` 为 Apache-2.0；`NOTICE` 另列 Beken overlay 和 GNU Unifont 等来源 | 成熟外设驱动对照，不作全 OpenVela 主 BSP |
| `open-vela/nuttx` PR #332 源分支 | `8dbe907a8461c3b6b5ceddf3c0fcf7a690df1ffd` | 2026-08-01 17:21:07 +0800 | NuttX 根 `LICENSE` 为 Apache-2.0 | CPU0-only NSH/UART0 最小移植对照 |

135 仓库的 `contest2026_135_yongwangzhiqian.xml` 明确固定：

- Beken SDK 仓库：`Embracecactus/bk_avdk_smp`
- Beken SDK 提交：`cb080de1655d579c7593ecf504c440997c4c137b`
- 路径：`vendor/beken/bk_avdk_smp`
- 分支来源：`refs/heads/openvela/v3.1.1.9`

### 许可证注意事项

- 根目录 Apache-2.0 不能自动覆盖仓库中的所有第三方组件。移植文件时应保留原文件头，并同步保留对应的 `LICENSE`、`NOTICE` 和来源记录。
- `SOURCE_PROVENANCE.md` 中列出的 NuttX、Beken SDK、Bluetooth、FFmpeg、UnQLite 等组件可能有不同许可证。最终作品若实际包含这些组件，应逐项保留其许可证义务。
- 264 仓库的 `NOTICE` 明确区分 Beken SDK overlay 和 GNU Unifont。若只参考实现而不复制字体/资源，不应把无关资源带入本项目。
- 不应复制任何 API Key、证书、设备凭据、用户数据或产品应用配置。

## 3. 135 主参考的真实系统结构

### 3.1 CPU 编号和操作系统位置

135 的实现中：

- 物理 CPU0：运行 CP 侧 NuttX/OpenVela，负责最早启动、系统控制以及启动 AP。
- 物理 CPU1：运行 AP 侧 NuttX/OpenVela，是 AP SMP 的逻辑 CPU0。
- 物理 CPU2：作为 AP NuttX 的第二个调度 CPU，是 AP SMP 的逻辑 CPU1。

因此，“AP 双核”指物理 CPU1 和 CPU2 共同运行一个 SMP NuttX 实例；不是三套互相独立的操作系统。

### 3.2 CPU0 复位和启动

关键文件与函数：

| 层次 | 文件 | 关键实现 |
|---|---|---|
| 向量表 | `chips/bk7258/cp/bk7258_vectors.c` | `bk7258_reset_entry()` 先初始化栈，再进入 `__start()`；80 项向量表中含 BK7258 特殊槽位 |
| 启动入口 | `chips/bk7258/cp/bk7258_start.c` | `__start()` 设置 VTOR、关闭 Bootloader 看门狗、启用 FPU、复制 `.data`、清零 `.bss`、初始化时钟/早期串口，最后进入 `nx_start()` |
| 中断 | `chips/bk7258/common/bk7258_irq.c` | `up_irqinitialize()` 关闭外部中断、设置 VTOR、复制 RAM 向量并修复特殊槽位 |
| SysTick | `chips/bk7258/common/bk7258_timerisr.c` | `up_timer_initialize()` 以 32 kHz 时钟建立系统节拍，供调度、超时和 `sleep` 使用 |
| 堆 | `chips/bk7258/common/bk7258_allocateheap.c` | 堆从 `g_idle_topstack` 延伸到链接符号 `_eheap` |

系统节拍不是可选装饰。如果 SysTick 没有稳定触发，调度超时、`sleep`、驱动等待和跨核握手都会出现“像卡死一样不返回”的连锁现象。

### 3.3 AP 与 CPU2 启动

| 功能 | 文件 | 关键函数/行为 |
|---|---|---|
| AP 复位入口 | `chips/bk7258/ap/bk7258_ap_vectors.c` | `bk7258_ap_reset_entry()` 把物理 CPU1 映射为 AP 逻辑 CPU0，设置栈后进入 `__start()` |
| AP NuttX 启动 | `chips/bk7258/ap/bk7258_ap_start.c` | `__start()` 初始化内存属性、VTOR、FPU、普通和 spinlock 数据区，最后进入 `nx_start()` |
| CPU2 启动 | `chips/bk7258/ap/bk7258_ap_smp.c` | `up_cpu_start()` 设置 CPU2 向量和启动记录，释放复位并等待调度器上线；二级入口建立 CPU2 运行环境后进入 idle/scheduler |
| AP 核间中断 | `chips/bk7258/ap/bk7258_ap_ipi.c` | 通过 Mailbox 发送/接收 IPI，使用原子 pending 和合并逻辑，支持 AP 内两个调度 CPU |
| AP 应用入口 | `boards/bk7258/common/src/bk7258_ap_entry.c` | `bk7258_ap_main()` 依次执行平台启动、板级初始化、发布 READY、延后初始化、应用启动和监督 |
| AP 生命周期 | `chips/bk7258/ap/bk7258_ap_lifecycle.c` | 检查平台、PSRAM、SMP、Mailbox、RPTUN 等状态后发布 READY，并维护心跳 |
| CP 启动 AP | `chips/bk7258/cp/bk7258_ap_control.c` | `bk7258_ap_start_locked()` 检查 PSRAM，先停 CPU2 再处理 CPU1，初始化 Mailbox/RPTUN，设置 AP 启动地址并等待 READY |
| CP 编排 | `chips/bk7258/cp/bk7258_cp_platform.c` | 平台阶段机调用 `bk7258_ap_start()`，再推进后续服务 |
| AP 监督 | `chips/bk7258/cp/bk7258_ap_supervisor.c` | 按 generation 读取共享状态，检查 AP/CPU2/RPTUN 活性，避免旧代消息误判，并提供恢复路径 |

`bk7258_ap_smp.c` 还通过 `__wrap_nx_bringup()` 协调 AP 主核完成 bring-up 后再放行 CPU2，避免两个调度 CPU 同时重复初始化板级资源。

### 3.4 内存与链接布局

`chips/bk7258/include/bk7258_amp.h` 给出的关键 SRAM 规划为：

| 区域 | 地址/大小 | 用途 |
|---|---|---|
| 总 SRAM | `0x28000000`，640 KiB | BK7258 内部 SRAM |
| AP spinlock | `0x28000000..0x2800ffff` | AP SMP 共享同步区 |
| CP RAM | `0x28010000`，`0x40000` | CPU0 NuttX RAM |
| AP RAM | `0x28050000`，RPTUN 配置下 `0x47000` | AP NuttX 两个调度 CPU 的运行内存 |
| RPTUN shared | `0x28097000`，`0x8000` | OpenAMP 资源表、vring 和通信 buffer |
| telemetry/shared page | `0x2809f000`，`0x1000` | 生命周期、心跳和跨核状态 |
| vendor PM | `0x2809f700` | 原厂电源管理共享区 |
| SWAP | `0x2809f800` | 原厂保留交换区 |

对应链接脚本：

- `boards/bk7258/common/scripts/ld.script`：CPU0 Flash/RAM、向量表、IRQ 栈、`.data/.bss/heap` 和 RPTUN 符号。
- `boards/bk7258/common/scripts/ld_ap.script`：AP Flash/RAM、spinlock 区、AP 主核和 CPU2 向量表、CPU2 启动栈、RPTUN 边界断言。

这些地址是多核 ABI 的组成部分。只复制 C 文件而不同时迁移链接脚本、生成的 partition 输入和两侧配置，会导致能编译但运行时互相覆盖。

### 3.5 RPTUN / RPMsg 通信

关键实现：

- `chips/bk7258/include/bk7258_rptun.h`
  - 共享控制块 offset `0x0000`、资源表 offset `0x0040`、carveout offset `0x0180`。
  - 两个 vring，每个 8 个 descriptor，8 字节对齐，buffer 512 字节。
  - 定义 OFFLINE、PREPARING、TABLE_READY、CONNECTING、CONNECTED、QUIESCING、FAULTED 状态。
  - 消息包含 generation，避免 AP 重启后旧通知污染新连接。
- `chips/bk7258/common/bk7258_rptun_mbox.c`
  - ISR 只做轻量复制/合并，完整校验和 callback 放在线程 worker 中。
  - 除 Mailbox 边沿通知外还轮询共享状态，弥补丢边沿竞态。
- `chips/bk7258/common/bk7258_rptun.c`
  - `bk7258_rptun_prepare_resource()` 建立 OpenAMP 资源表和 carveout。
  - `bk7258_rptun_start()` 进入 CONNECTING。
  - `bk7258_rptun_receive()` 校验 generation 并合并 vring 通知。
  - `bk7258_rptun_mark_connected()` 只在当前 generation 的 endpoint 证明成立后发布 CONNECTED。
  - `bk7258_rptun_initialize()` 允许 CP lower-half 在 AP 重启期间保持注册，同时重建共享表。
  - `bk7258_rptun_quiesce()` 负责有序停止。

因此下列文件应作为一个整体迁移：

```text
bk7258_amp.h
bk7258_rptun.h
bk7258_rptun.c
bk7258_rptun_mbox.c
bk7258_ap_control.c
bk7258_ap_supervisor.c
AP/CP 两侧相同的 RPTUN、Mailbox、内存布局配置
CP/AP 链接脚本中的共享内存符号与断言
```

不能把 135 的 RPTUN 一侧与当前工程旧协议或 264 的 Mailbox 上层直接拼接。

## 4. Board bring-up 与 R1 外设映射

### 4.1 通用 bring-up 路径

| 文件 | 作用 |
|---|---|
| `boards/bk7258/common/src/bk7258_boot.c` | `board_late_initialize()`：AP 进入 `bk7258_ap_platform_prepare()`，CP 进入 `bk7258_cp_bringup_initialize()` |
| `boards/bk7258/common/src/bk7258_appinit.c` | `board_app_initialize()` 调用 `bk7258_bringup()` |
| `boards/bk7258/common/src/bk7258_bringup.c` | 检查 CP 平台启动结果，再注册 DVFS/procfs、持久化 MTD/FTL 等 |
| `boards/bk7258/common/src/bk7258_cp_bringup.c` | 把 CP 平台 begin/finish 与板级设备、按键衔接起来 |
| `boards/bk7258/common/src/bk7258_ap_bringup.c` | 初始化 AP 控制器、JPEG、音频、I2C、MIC、PWM、DMA，以及 RTC/ADC/Timer 等总线 |
| `boards/bk7258/aidk_ai_toy/src/bk7258_board_bringup.c` | R1/AIDK 板即时初始化和延后初始化；LCD、Camera、SDIO、传感器、NFC、电池等放在 READY 后执行 |

把慢速或可失败外设放入 deferred bring-up，可以避免 LCD、Camera 或 SD-NAND 的问题阻塞 AP 心跳和 RPTUN 基础链路。这个结构值得保留。

### 4.2 R1/AIDK 引脚证据

`boards/bk7258/aidk_ai_toy/include/bk7258_board_config.h` 定义：

- KEY：GPIO 8、12、13；LED：GPIO 40、41；振动电机：GPIO 9。
- 音频 PA：GPIO 50；LCD 电源 LDO33：GPIO 52。
- LCD1：QSPI1，GPIO 2/3/4，DC 5，RST 45。
- LCD2：QSPI0，GPIO 22/23/24，DC 7，RST 6。
- SD-NAND：GPIO 14～19。
- Camera：I2C1 GPIO 42/43，PWR 49，RST 28，MCLK 27，DVP 29～39。

对应板级实现文件包括：

```text
bk7258_aidk_battery.c
bk7258_aidk_board_audio.c
bk7258_aidk_board_sdio.c
bk7258_aidk_buttons.c
bk7258_aidk_camera*.c
bk7258_aidk_dual_lcd.c
bk7258_aidk_mfrc522.c
bk7258_aidk_motor.c
bk7258_aidk_sc7a20*.c
```

注意：该头文件仍定义 `BK7258_BOARD_HARDWARE_VERIFIED 0`，并写有“按原理图推导、待验证”的旧注释；但仓库后续又有同一 AIDK 的实机验证文档。这个标记与后续证据不一致，移植时不能简单把它改成 1，而应逐项在本项目 R1 板上重新确认并更新记录。

## 5. 配置、构建、镜像与 Flash

### 5.1 最小配置来源

`boards/bk7258/aidk_ai_toy/openvela.conf` 指定：

- CPU0 配置：`boards/bk7258/aidk_ai_toy/configs/app`
- AP 配置：`boards/bk7258/aidk_ai_toy/configs/openvela_ap`
- 分区表：`bk7258_ab_fixed_block_full_release.csv`

源码检查确认：

- CPU0 配置包含 ARMv8-M SysTick、AP autostart/supervisor、PSRAM、RPTUN/layout/mbox、Wi-Fi vnet，入口为 `nsh_main`。
- AP 配置包含 AP core/IPI/SMP bootstrap、PSRAM/media/ext heap、RPTUN/layout/mbox、Wi-Fi vnet，入口为 `bk7258_ap_main`。
- AP 必须保留 `CONFIG_SMP=y`、`CONFIG_SMP_NCPUS=2`，以及与 CPU0 完全一致的共享内存/RPTUN配置。

整合时应先裁剪到最小启动配置，而不是一开始启用所有媒体外设：

```text
阶段 A：CPU0 NuttX + UART0 + IRQ + SysTick + heap + NSH
阶段 B：CPU0 启动 AP 主核 + AP NuttX + CPU2 SMP/IPI
阶段 C：RPTUN/RPMsg + generation/heartbeat/supervisor
阶段 D：R1 基础 bring-up（按键、双屏等逐项接入）
阶段 E：Wi-Fi
阶段 F：Camera、Audio、存储、电源管理
```

### 5.2 分区表

`bk7258_ab_fixed_block_full_release.csv` 的主要区域：

| 分区 | 位置/大小 | 说明 |
|---|---|---|
| `primary_bootloader` | 68 KiB | 启动加载器 |
| `primary_cp_app` | 1224 KiB | CPU0 NuttX 镜像 |
| `primary_ap_app` | 1564 KiB | AP NuttX 镜像 |
| `s_app` | 2788 KiB | 次级/更新镜像区域 |
| `usr_config` | `0x584000`，56 KiB | 保留用户配置 |
| `reset_marker` | `0x592000`，4 KiB | 保留复位标记 |
| `persistent_data` | `0x600000`，1 MiB | 保留持久数据 |
| `easyflash` | `0x7fa000`，8 KiB | 设备数据，不应被通用镜像覆盖 |
| `easyflash_ap` | `0x7fc000`，8 KiB | AP 设备数据，不应被通用镜像覆盖 |
| `sys_rf` | `0x7fe000`，4 KiB | 设备 RF 校准，不应取自他板 |
| `sys_net` | `0x7ff000`，4 KiB | 网络设备数据，不应取自他板 |

### 5.3 构建和打包实现

推荐入口：

```text
tools/bk7258/bk7258.py build --board aidk_ai_toy --boot direct --jobs 8
```

实际流程由以下脚本完成：

- `tools/bk7258/_lib/build.py`：校验 SDK、配置、工具链和分区；分别构建 CP/AP；检查两侧拓扑；生成 ELF、raw bin、manifest 和 provenance。
- `tools/bk7258/_lib/image.py`：把每 32 字节数据编码为带 2 字节 CRC16 的 Flash 形式，并生成各分区镜像。
- `tools/bk7258/_lib/package.py`：验证分区和产物，基于已接受的设备 base 生成可交付镜像。
- `tools/bk7258/bk7258.py`：公开命令入口，提供 build、SDK、accept-base、delivery、verify 等命令。

真实总链路为：

```text
源码
→ Kconfig/defconfig（openvela.conf 选择 CP/AP 配置）
→ build.py 分别调用官方构建流程
→ 链接脚本读取生成的分区地址
→ CP ELF / AP ELF
→ raw bin
→ 32+2 CRC 编码后的 Flash segment
→ partition CSV 决定落点
→ package 用本设备已接受的 full-flash base 保留 RF/MAC/配置/用户数据
→ 用户烧录
→ Bootloader 跳转 CPU0 向量
→ CPU0 __start → nx_start → board_late_initialize / board_app_initialize
→ CPU0 拉起 AP
→ AP __start → nx_start → CPU2 up_cpu_start
→ Mailbox/RPTUN/RPMsg 连接
→ deferred board bring-up
```

禁止用别的开发板的整片 Flash 作为本板 base；尤其不能覆盖 `easyflash`、`sys_rf`、`sys_net` 等设备特有区域。

## 6. 135 仓库现有实机证据与限制

源码仓库中存在以下 AIDK 实机记录：

- `docs/verification/bk7258/2026-09-07-aidk-peripheral-rpc.md`
  - 串口确认 AP READY、CPU2 online、RPMsg connected。
  - 通过 RPC 验证运动传感器请求、NFC 无卡路径、健康检查、两个 framebuffer 打开/绘制、5 秒音频路径。
- `docs/verification/bk7258/2026-09-07-aidk-audio-watchdog.md`
  - 验证 5 秒、16 kHz、单声道 PCM 采集路径。
  - 验证看门狗重启后 AP READY、CPU2 online、RPMsg connected。
- `docs/verification/bk7258/2026-08-31-aidk-ai-toy-sys-rf-preservation.md`
  - 记录该设备 `sys_rf` 区域多次读取哈希一致，强调通用 dense image 禁止覆盖设备 RF 数据。

这些记录只证明记录中列明的有限验收点。以下项目不能从这些材料推导为“已经完成”：

- Wi-Fi 在全 OpenVela 多核结构下的稳定连接、重连和异常恢复。
- Camera 从传感器、DVP、DMA、PSRAM、JPEG 到应用的完整稳定性。
- 音频主观质量、AEC/NS、连续双向实时语音。
- 深睡、唤醒、长时间运行和全部故障恢复。
- 本项目 R1 板硬件版本与参考 AIDK 样机的完全一致性。

## 7. 264 仓库可复用与不可复用部分

固定提交：`ba87d984a1e08bbcd5c09734700a3d4574739158`。

实际源码位于：

```text
board/beken/chips/bk7258
board/beken/boards/bk7258/bk7258-ap
```

其中能找到 `bk7258_start.c`、`bk7258_irq.c`、`bk7258_timerisr.c`、`bk7258_bringup.c`、Mailbox V2、Wi-Fi proxy、audio、camera、JPEG、LCD/QSPI、SDIO 等实现。

但其当前 AP 配置存在明确限制：

- 当前 `nsh` 和 `ai_agent` defconfig 没有启用 `CONFIG_SMP=y`。
- 配置没有启用 RPTUN。
- `ai_agent` 配置明确表现为单 CPU。
- 仓库文档也写明当前 `CONFIG_NCPUS=1`，CPU2 未使用。
- SMP 内容位于 `docs/archive/BK7258_OPENVELA_SMP_PORTING_PLAN.md`，属于归档计划，不是当前运行实现。

因此该仓库的 CPU0 仍由 Armino 承担，AP 只在物理 CPU1 上运行单核 NuttX。可以借鉴：

- 已经针对 BK7258/AIDK 使用过的 Wi-Fi、LCD、Camera、Audio、SDIO lower-half 和板级 glue。
- Mailbox V2 的芯片访问细节。
- 外部源码 manifest、overlay 摘要和 NOTICE 记录方式。

不能直接复用：

- `app/velasight` 产品应用。
- 其 CPU0 Armino 启动结构作为全 OpenVela 最终架构。
- 把单核 AP 配置误当作 AP 双核完成态。
- 用它的 Mailbox/RPC 上层替换 135 的 generation-aware RPTUN 生命周期协议。

若要引用某个外设驱动，必须逐文件对照 135 的时钟、IRQ、DMA、内存和资源归属，再适配到 135 的 SMP/RPTUN 架构，而不是整目录覆盖。

## 8. NuttX PR #332 的用途边界

固定提交：`8dbe907a8461c3b6b5ceddf3c0fcf7a690df1ffd`。

该分支提供 CPU0 最小 NSH/UART0 移植，包括：

- `bk7258_start.c`：MSPLIM、FPU、VTOR、Bootloader watchdog、UART、`.data/.bss`、`nx_start()`。
- `bk7258_lowputc.c`：物理 UART0 和 CH340 对应 GPIO 11/10。
- `bk7258_serial.c`：UART0 upper-half。
- `bk7258_irq.c`：NVIC 和系统级中断聚合。
- `bk7258_timerisr.c`：SysTick。
- heap、寄存器和 board 配置。

它适合回答“最小 CPU0 NuttX 需要什么”，也可用于交叉核对 135 的启动顺序。但该 PR 仍未合入且不含 AP 双核/RPTUN/完整 board bring-up，不应直接取代 135 主参考。

## 9. 建议纳入的精确范围

### 9.1 第一批：CPU0-only 最小启动

从 135 主参考选择并适配：

```text
chips/bk7258/cp/bk7258_vectors.c
chips/bk7258/cp/bk7258_start.c
chips/bk7258/common/bk7258_irq.c
chips/bk7258/common/bk7258_timerisr.c
chips/bk7258/common/bk7258_allocateheap.c
chips/bk7258/include/（只纳入上述实现实际依赖的寄存器、地址和配置头）
boards/bk7258/common/scripts/ld.script
boards/bk7258/aidk_ai_toy/configs/app
```

验收：CPU0 NSH、UART、IRQ、SysTick、`sleep`、heap；核对 ELF/map 地址。

### 9.2 第二批：AP 主核与 CPU2 SMP

```text
chips/bk7258/ap/bk7258_ap_vectors.c
chips/bk7258/ap/bk7258_ap_start.c
chips/bk7258/ap/bk7258_ap_smp.c
chips/bk7258/ap/bk7258_ap_ipi.c
chips/bk7258/ap/bk7258_ap_lifecycle.c
chips/bk7258/cp/bk7258_ap_control.c
chips/bk7258/cp/bk7258_ap_supervisor.c
chips/bk7258/include/bk7258_amp.h
boards/bk7258/common/scripts/ld_ap.script
boards/bk7258/common/src/bk7258_ap_entry.c
boards/bk7258/aidk_ai_toy/configs/openvela_ap
```

验收：AP READY、CPU2 scheduler online、IPI 双向测试、心跳增长、generation 一致。

### 9.3 第三批：RPTUN / RPMsg

```text
chips/bk7258/include/bk7258_rptun.h
chips/bk7258/common/bk7258_rptun.c
chips/bk7258/common/bk7258_rptun_mbox.c
CP/AP 配置中的 RPTUN/layout/mbox 项
CP/AP 链接脚本中的共享内存定义和断言
```

验收：TABLE_READY → CONNECTING → CONNECTED；端点属于当前 generation；AP 重启后旧消息不污染新会话；CP 监督能区分 AP 未启动、RPTUN 未连接和 CPU2 未上线。

### 9.4 第四批：通用与 R1 board bring-up

```text
boards/bk7258/common/src/bk7258_boot.c
boards/bk7258/common/src/bk7258_appinit.c
boards/bk7258/common/src/bk7258_bringup.c
boards/bk7258/common/src/bk7258_cp_bringup.c
boards/bk7258/common/src/bk7258_ap_bringup.c
boards/bk7258/aidk_ai_toy/include/bk7258_board_config.h
boards/bk7258/aidk_ai_toy/src/bk7258_board_bringup.c
boards/bk7258/aidk_ai_toy/src/bk7258_aidk_*.c（逐外设选择，不一次全开）
```

验收顺序：按键/LED → 双屏 → 存储 → Wi-Fi → Camera → Audio → 电源管理。每个外设都要记录设备节点、关键函数、日志和资源释放结果。

### 9.5 第五批：构建与交付工具

```text
boards/bk7258/aidk_ai_toy/openvela.conf
boards/bk7258/aidk_ai_toy/bk7258_ab_fixed_block_full_release.csv
tools/bk7258/bk7258.py
tools/bk7258/_lib/build.py
tools/bk7258/_lib/image.py
tools/bk7258/_lib/package.py
工具实际引用的其余同目录模块
```

工具也不能只复制四个入口文件，应该通过 import 和测试确认其完整依赖闭包。最终必须保存：工具版本、构建命令、配置 hash、ELF/bin hash、分区地址、烧录范围以及回退镜像 hash。

## 10. 必须排除或暂缓的内容

### 必须排除

- 135 的 `app/bk7258`、`app/dolphin`、`gateway/shaniu`、Android、QuickApp 等产品应用。
- 264 的 `app/velasight`。
- 产品 UI、模型数据、演示素材、云端凭据、证书、用户记录和任何 API Key。
- 其他开发板的完整 Flash、RF 校准、MAC、EasyFlash、用户分区。
- 只存在于 archive/plan 文档、没有当前配置和源码支撑的功能声明。
- 将两个仓库不同版本的共享结构、Mailbox 编号、RPTUN generation 协议混合使用。

### 暂缓

- Secure Boot、MCUboot、OTA 和签名交付：必须先明确比赛交付形式、信任根、密钥保管、rollback counter 和恢复方法。
- Voice/Agent/云端产品应用：应在底层 CPU0、AP SMP、RPTUN、基础设备和资源归属稳定后再接回。
- 所有“整仓覆盖”：只允许有证据的分阶段整合和接口适配。

## 11. 建议的整合与验证节奏

1. **冻结参考**：记录三个参考提交、许可证、当前工程提交、设备 full-flash 回退镜像和各分区 hash。
2. **CPU0-only**：先用最小配置验证 NSH、SysTick、heap、IRQ 和链接地址。
3. **AP + CPU2**：不带外设，验证 AP READY、SMP online、IPI 和 heartbeat。
4. **RPTUN**：验证共享 ABI、generation、RPMsg endpoint、AP 重启和监督恢复。
5. **基础板级资源**：逐个启用 GPIO、双屏、存储；一次只增加一个资源所有者。
6. **Wi-Fi**：单独验证扫描、关联、DHCP、DNS、断线恢复和跨核数据路径。
7. **Camera/Audio**：检查 DMA 可访问内存、PSRAM、cache、buffer ownership 和停止/释放路径。
8. **应用接回**：只在 BSP 接口稳定后，把现有产品应用通过明确 API 接回，不让应用直接依赖参考仓库私有结构。

每阶段都应具有：

- 可重复的构建命令；
- `.config`、ELF、map、bin 和 hash；
- 精确烧录范围；
- 开发板串口验证证据；
- 失败时的第一检查位置；
- 可恢复的上一阶段镜像。

## 12. 最终判断

135 仓库目前是公开竞赛仓库中最适合作为本项目“CPU0 和 AP 实际都运行 NuttX/OpenVela”目标的参考实现。它的价值不只是文件数量，而是形成了互相匹配的启动、链接、SMP、RPTUN、监督、bring-up 和构建打包闭环。

建议把它作为固定主线逐阶段整合；264 仅作为外设细节对照；PR #332 仅作为 CPU0 最小启动对照。所有功能最终仍以本项目 R1 的源码、ELF/map、分区、烧录范围和实机日志为准，不把参考仓库的验证结论直接当成本项目结论。
