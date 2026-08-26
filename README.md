# 基于 openvela 的视障随行视觉辅助胸牌

> 2026 首届 openvela AI 硬件开发者大赛 · 队伍 441 · AI 硬件产品创新赛道

面向具有独立出行能力的视障用户，本项目使用大赛发放的
**对话式 AI 开发套件 R1（声网 & 博通集成，BK7258）**制作免手持视觉辅助设备。
用户到达目标区域后，设备按需拍摄前方场景，经 Wi-Fi 将单帧 JPEG 发送至
MiMo 多模态服务，识别入口、门牌、站点标识和公共服务设施，再通过语音或
振动反馈关键信息。

当前目标为 **BK7258 R1**。截至 2026-08-26，独立 Ubuntu 工作区已完成
公开 `bk7258-devkit:nsh` 与队伍 `vision_badge` 两套 ARM 固件构建；应用后端仍有
`-ENOSYS` 占位。开发板尚未到达，没有 R1 启动或外设实机结果。

`bk7258-devkit` 是当前借用的公开 BSP 名称，**不代表已确认与 R1 完全兼容**。
官方仍将套件列为待适配，两份候选 PR 尚未合并。详见
[板卡差异与源码入口](board/bk7258-r1/README.md)和[构建验证](docs/progress/构建验证-20260826.md)。

## 一、硬件与工作边界

R1 套件集成 BK7258、DVP 摄像头、双麦与本地 AEC、双屏、触摸、振动、
陀螺仪、NFC、SD NAND、扬声器接口、USB 转串口、蓝牙配网和 2.4 GHz Wi-Fi。
这些是套件可用资源，不代表 openvela 已有对应驱动。

项目分成两条相互解耦的工作线：

1. **BK7258/openvela 适配**：先完成 CPU0/NSH 基线，再补齐时钟、GPIO、PSRAM、
   Wi-Fi、Camera、Audio、显示和板级外设；
2. **应用功能开发**：保持 `camera_service`、`vision_service`、`audio_service`、
   `feedback_service` 接口稳定，继续完成“采图 → 云端理解 → 反馈”闭环。

公开案例、复用判断和风险见 [BK7258 移植调研](docs/BK7258移植调研.md)，
阶段计划见 [BK7258 移植计划](docs/BK7258移植计划.md)。

## 二、最小技术闭环

```text
用户触发
  → BK7258 R1 DVP Camera
  → openvela Camera 设备接口（目标 /dev/video0）
  → 单帧 JPEG
  → Wi-Fi / HTTPS
  → MiMo Vision
  → 有界结构化结果
  → 控制台 / 语音 / 振动反馈
```

第一阶段不持续上传视频，也不在端侧部署大型视觉模型。应用接口、状态机和
主机测试可与平台适配并行；每项实机功能只依赖其必要的平台能力，例如离线
采图不必等待 Wi-Fi 和音频完成。M1～M10 的定义见
[MVP 验收标准](docs/MVP验收标准.md)。

## 三、目录结构

```text
├── app/vision_badge/       # 原生应用、服务接口和闭环状态机
├── board/bk7258-r1/        # 队伍配置、上游依赖与使用边界
├── docs/                   # 调研、移植计划、架构、预算、验收与测试
├── logs/                   # 按大赛规范导出的 AI Coding 日志
├── scripts/                # 环境检查、构建、烧录、串口监视
└── contest2026_441_buzhidaojiaoshenmemingzi.xml
                            # repo manifest 与 linkfile 映射
```

## 四、拉取、构建与运行

### 1. 在现有 Ubuntu 工作区复现（当前入口）

当前独立工作区为 `/home/alientek/openvela`，参赛仓库位于其下同名目录。
已同步 234 个项目；候选版本由 `.repo/local_manifests/441-bk7258.xml` 固定：

- NuttX PR #332：`8dbe907a8461c3b6b5ceddf3c0fcf7a690df1ffd`；
- vendor_beken PR #2：`f46d0576b539d2fa48c8ea308cb2044d5f227f34`。

先运行只读检查，再依次构建两套配置：

```bash
cd /home/alientek/openvela
bash contest2026_441_buzhidaojiaoshenmemingzi/scripts/check-env.sh
./build.sh vendor/beken/boards/bk7258/bk7258-devkit/configs/nsh/ --cmake -j2
bash contest2026_441_buzhidaojiaoshenmemingzi/scripts/build.sh -j2
```

产物分别位于 `cmake_out/bk7258-devkit_nsh/` 和
`cmake_out/bk7258-devkit_contest2026_441_vision_badge/`，包含 `nuttx`（ELF）、
`nuttx.bin`、`nuttx.hex` 和 `System.map`。重复执行通常是增量构建，不等于一次新的
干净构建。实际固件编译器为预编译 `arm-none-eabi-gcc 13.4.0`，不是主机 GCC。

主机侧回归不需要开发板，也不会调用真实模型：

```bash
cd /home/alientek/openvela/contest2026_441_buzhidaojiaoshenmemingzi
bash scripts/test-host.sh
```

### 2. 新建工作区时的依赖说明

以下是大赛基础拉取命令，**不是当前 BK7258 未提交快照的一键复现命令**：

```bash
repo init -u https://github.com/open-vela/contest2026_441_buzhidaojiaoshenmemingzi \
  -b dev-ai-contest-2026 -m contest2026_441_buzhidaojiaoshenmemingzi.xml
repo sync -c -j4
```

同步后，队伍仓位于工作区的
`contest2026_441_buzhidaojiaoshenmemingzi/`，公共 `nuttx/`、`packages/`、
`vendor/` 位于上一级。

基础命令会拉取专属仓 `dev-ai-contest-2026` 分支；只有本次硬件切换审核合入后，
新工作区会取得队伍 BK7258 内容。还需显式选择配套候选 PR，模板见
[candidate-prs.xml](board/bk7258-r1/candidate-prs.xml)，仅供独立实验工作区使用，
不会自动生效，也不应叠加覆盖现有 VM 的同项目版本设置。不要在已有未提交
改动的工作区重新初始化或切换版本。

manifest 会映射：

- `app/vision_badge` → `packages/demos/contest2026_441_vision_badge`
- 队伍配置 → `vendor/beken/boards/bk7258/bk7258-devkit/configs/contest2026_441_vision_badge`

构建脚本检查本地 BSP 和链接配置是否存在，不查询 PR 是否已合并。选定配套
候选源码后可以实验构建；缺少 BSP 时会明确退出。队伍 defconfig 是候选基线，
R1 引脚和内存布局核对后，应通过配置工具及 `savedefconfig` 更新。

### 3. 打包、烧录与串口

`nuttx.bin` 不能直接烧录；需要按 BK7258 分区和线性 CRC 规则打包为完整镜像。
打包流程尚待与最终上游 BSP 固定，烧录脚本只接收已审核的完整镜像：

```bash
bash scripts/flash.sh /dev/ttyUSB0 path/to/all-app-nuttx.bin --confirm
bash scripts/monitor.sh /dev/ttyUSB0
```

进入 NSH 后可运行：

```text
vision_badge status
vision_badge selftest
```

`status` 只报告真实设备节点；未实现后端返回 `-ENOSYS`。

## 五、当前进度

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| BK7258 公开资料与移植案例 | 已调研 | 找到 NuttX PR #332、vendor_beken PR #2 和声网/博通示例 |
| BK7258 R1 队伍配置 | 候选配置构建通过 | 借用 devkit BSP，R1 硬件匹配仍待核对 |
| 应用构建接入与服务接口 | 主机回归及 ARM 链接通过 | 内置命令已注册；硬件/云端后端仍待实现 |
| CPU0 + UART0 + NSH | 本队已构建，未上板 | 作者报告 DevKit 实板通过，不替代本队 R1 证据 |
| PSRAM、单屏、双麦 | 公开实验参考 | 当前最小配置的系统堆仍使用 SRAM；未证明 R1 可用 |
| Wi-Fi、DVP Camera、完整音频与双屏 | 待适配 | 是当前平台主路径 |
| M1～M10 应用闭环 | 待真机逐项验证 | 以测试证据更新状态 |

## 六、安全、协作与 AI Coding

- Wi-Fi SSID/密码、MiMo API Key、证书私钥不得进入代码、defconfig、文档或日志；
- 引用公开 PR 先核对许可证、提交状态和评审意见，不把未合并代码当稳定 API；
- 比赛期间新增的队伍板级适配放在专属仓子目录，并通过 manifest 映射；
  修改公共 `nuttx` 等已有代码时，按[提交指南](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/code_submission_guide.md)
  fork 对应公共仓并提交比赛分支 PR；作品获奖后的上游提交另按官方要求执行；
- AI 用于需求拆解、资料核查、接口骨架和文档生成；真实会话日志按
  [logs/README.md](logs/README.md) 归档并审查敏感信息；
- 仓库采用“本地变更 → 团队审核 → 分步中文 commit → push → Pull Request”的流程，
  未经审核不推送。

## 七、项目 skill 与能力审查

项目专用 [BK7258 R1 openvela skill](.agents/skills/bk7258-openvela-porting/SKILL.md)
参考官方 NuttX 驱动、openvela 构建与 PCM 音频 skills，沉淀资料取舍、分阶段验证和真实能力证据要求。
目录使用 `.agents/skills`，可在本项目中通过 `$bk7258-openvela-porting` 引用。

当前图形、AI、多媒体均未完成本队实机闭环，详见[能力落地审查](docs/能力落地审查.md)。
静态审查及其主机测试：

```sh
python .agents/skills/bk7258-openvela-porting/scripts/audit_project.py . --json
python -B .agents/skills/bk7258-openvela-porting/scripts/test_audit_project.py
```
