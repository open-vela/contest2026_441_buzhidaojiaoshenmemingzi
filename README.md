# BK7258 R1 全 OpenVela 适配与多模态能力验证

> 2026 首届 openvela AI 硬件开发者大赛 · 不知道叫什么名字（队伍 441）· 邢晓亮 · 新硬件平台适配

本项目面向 BK7258 R1（AIDK AI Toy）开发板，重点完成一条可追溯的全
OpenVela/NuttX 板级适配链路：物理 CPU0 运行 CP 侧 NuttX，物理 CPU1 与
CPU2 共同运行 AP 侧 SMP NuttX，并通过 Mailbox、共享内存和 RPTUN/RPMsg
协同工作。在此基础上逐项接入双屏、Camera、Audio、Wi-Fi、按键、电源和存储。

项目的主要成果不是一个只在演示时能运行的上层应用，而是把下面这条路径完整
落到真实源码、配置、镜像和开发板证据中：

```text
源码 → Kconfig/defconfig → CP/AP 构建 → ELF/map/bin
     → 分区与镜像编码 → Flash → CPU0 启动
     → AP/CPU2 启动 → RPTUN → board bring-up → /dev 设备与诊断命令
```

## 当前结论

截至 2026-09-19：

- CPU0-only OpenVela 已在本队 R1 实机启动到 NSH；
- 全 OpenVela 实验镜像已取得 AP READY、RPTUN CONNECTED、CPU2 SMP PASSED
  和 Wi-Fi 成功日志；
- 双屏、GC2145 Camera/JPEG、16 kHz Audio 录放音和长按电源链路已在历史混合
  固件上验证，用于确认 R1 硬件链可用；
- 已将公开参考仓库 `contest2026_135_yongwangzhiqian` 的多核 BSP 以固定提交
  `7079493e73159e00c1b03e60bee6ee69845751cd` 为基线整合到本仓，并剔除其产品
  应用、云端、模型、UI 和凭据；
- 新增本队 CP/AP 配置和 `apctl`、`bkwifi` 诊断命令；
- r14 已在指定 Ubuntu VM 构建，并由本队 R1 实机完成 KEY1 拍照识别、MiMo 中文结果
  返回和双屏原厂样式眼睛动画验证。两次保存的视觉查询均为 `stage=done status=0`；
  语音识别与播报未纳入本次提交成果。

## 系统结构

```text
BK7258
├─ 物理 CPU0：CP NuttX/OpenVela
│  ├─ UART0 / NSH
│  ├─ AP 生命周期与监督
│  └─ RPTUN、Wi-Fi VNET 诊断
└─ AP NuttX/OpenVela（SMP）
   ├─ 物理 CPU1：AP 逻辑 CPU0，bring-up 与主要驱动
   ├─ 物理 CPU2：AP 逻辑 CPU1，SMP 调度
   └─ 双屏、Camera、Audio、Wi-Fi、SD-NAND、PSRAM
```

完整启动、内存与通信关系见 [系统架构](docs/系统架构.md)。源码来源和复用边界见
[源码来源与复用边界](SOURCE_PROVENANCE.md)。

## 目录

```text
app/bsp_diag/                 AP/RPTUN/SMP 与 Wi-Fi 诊断命令
app/vision_badge/             KEY1/NSH 拍照、MiMo 查询、跨核结果返回
chips/bk7258/                 BK7258 CPU0/AP/CPU2、IRQ、SysTick、RPTUN 等
boards/bk7258/                通用板级层、R1 板级层、CP/AP 配置和分区
nuttx/                        本 BSP 所需的 NuttX 覆盖文件
tools/bk7258/                 构建、地址检查、镜像编码与打包工具
evidence/                     经脱敏的本队实机证据
logs/                         按大赛格式导出的 AI Coding 日志
submission/                   官方模板、正文草案和报告候选稿
.agents/skills/               本项目自建 Skill
```

## 构建

在完整 openvela Linux 工作区中执行：

```bash
cd /home/alientek/ov441_ws/team-repo
python3 tools/bk7258/bk7258.py build \
  --board aidk_ai_toy --boot direct --jobs 8 \
  --workspace /home/alientek/ov441_ws
```

构建工具会分别处理 CP/AP 配置，校验两侧拓扑与内存布局，并生成带来源和哈希的
构建清单。详细复现、烧录边界和验收命令见
[构建与提交指南](docs/构建与提交指南.md)。烧录由开发板操作者完成，仓库脚本不
自动擦除整片 Flash，也不覆盖 RF、MAC、EasyFlash 和用户数据分区。

## 证据规则

项目只使用三种状态：

1. **源码整合**：文件和配置已进入仓库；
2. **构建通过**：同一提交已生成 ELF/map/bin/镜像和 SHA256；
3. **实机通过**：同一提交、同一镜像的串口日志或真机照片已保存。

证据索引见 [evidence/README.md](evidence/README.md)，当前进度见
[当前进度](docs/progress/当前进度.md)。参考仓的结果只能证明方案来源，不能替代
本队 R1 的构建和实机验证。

## 比赛提交材料

- 按官方模板填写的技术报告：`submission/技术报告-BK7258-R1-视觉辅助胸牌.docx`
- 同内容 PDF：`submission/技术报告-BK7258-R1-视觉辅助胸牌.pdf`
- 可审查正文：`submission/技术报告正文草案.md`
- r14 构建、烧录与串口证据：`evidence/final/`
- 硬件正面照片：`submission/硬件照片.jpg`
- AI Coding 日志：`logs/xxluestc/`
- 自建 Skill：`.agents/skills/bk7258-openvela-porting/`

报告中的性能、功耗和稳定性只填写有测试方法和原始日志的数据。提交压缩包还需加入
不超过 5 分钟的演示视频；报告、照片与源码由本仓提供。

## 安全与许可证

- Wi-Fi 密码、API Key、证书私钥和设备私有数据不进入仓库；
- 外部代码的仓库、固定提交、许可证和修改边界记录在
  [SOURCE_PROVENANCE.md](SOURCE_PROVENANCE.md)；
- 根目录 `LICENSE` 不取代第三方源文件和组件原有的许可证义务。
