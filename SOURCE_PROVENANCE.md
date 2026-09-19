# 源码来源与复用边界

本文件记录终版候选中外部代码的固定来源、许可证和本队修改。它不能替代各源
文件原有的版权头和许可证文件。

## 1. 全 OpenVela BK7258 BSP 主参考

- 仓库：`https://github.com/open-vela/contest2026_135_yongwangzhiqian`
- 分支：`dev-ai-contest-2026`
- 固定提交：`7079493e73159e00c1b03e60bee6ee69845751cd`
- 根许可证：Apache-2.0
- 纳入范围：
  - `chips/bk7258/`：CPU0、AP、CPU2 SMP/IPI、IRQ、SysTick、PSRAM、
    Mailbox/RPTUN、Wi-Fi 和基础外设驱动；
  - `boards/bk7258/common/`：CP/AP 启动、链接脚本、bring-up 和分区规则；
  - `boards/bk7258/aidk_ai_toy/`：R1/AIDK 的引脚、电源、双屏、Camera、
    Audio、SD-NAND 等板级绑定；
  - `nuttx/`：上述 BSP 依赖的 NuttX 覆盖文件；
  - `tools/bk7258/`：配置、双镜像构建、地址检查、镜像编码和打包工具。

本队修改：

- 新增 `team441_cp`、`team441_ap` 配置，去掉参考项目的 Agent、云端、产品
  UI、模型、配网和 OTA 默认项；参考仓的 app/openvela_ap/drivercheck/xts
  配置未纳入本队维护范围；
- `openvela.conf` 改为选择本队 CP/AP 配置；
- `tools/bk7258/_lib/build.py` 的来源哈希范围改为本队实际存在的应用目录；
- `tools/bk7258/_lib/layers.py` 对文本例外文件按 LF 规范化后计算 SHA256，避免
  Windows CRLF 检出造成误报，并新增主机回归测试；
- 删除参考项目专用的 AIDK 自动化说明，清理 `rcS` 中未启用的产品启动项，媒体
  ROMFS 前缀改为本队名称；
- 新增 `app/bsp_diag/`，只保留 AP/RPTUN/SMP 和 Wi-Fi VNET 两个诊断入口。

未纳入：参考仓的 `app/bk7258`、`app/dolphin`、Gateway、Android、QuickApp、
产品 UI、模型、凭据、用户资料和整片 Flash。

## 2. Beken SDK

- 仓库：`https://github.com/Embracecactus/bk_avdk_smp`
- 固定提交：`cb080de1655d579c7593ecf504c440997c4c137b`
- 来源分支：`openvela/v3.1.1.9`
- 许可证：Apache-2.0
- 工作区路径：`vendor/beken/bk_avdk_smp`

该项目由 repo manifest 固定，只作为 BK7258 芯片库和硬件接口依赖。终版禁止
从其他开发板复制 EasyFlash、RF 校准、MAC、配对或用户分区。

本队双屏眼睛动画位于 `boards/bk7258/aidk_ai_toy/src/bk7258_aidk_eye.c`。
`bk7258_aidk_eye_assets.c` 是从旧版 R1 产品使用的 Beken `genie_eye.avi` 帧
转换出的 RGB565 调色板和像素数据，恢复时记录的源文件 SHA256 在资产文件头。
动画调度、双屏绘制和状态反馈已适配本次全 OpenVela BSP；资产不是来自参考队伍
135 的产品应用。

## 3. BSP 诊断命令

`app/bsp_diag/apctl_main.c` 和 `bkwifi_main.c` 分别派生自上述主参考提交的：

```text
app/bk7258/bk7258_apctl_main.c
app/bk7258/bk7258_wifi_main.c
```

两份源文件保留 Apache-2.0 SPDX 头。本队只重做构建/Kconfig 接入，不加入
产品协议或固定网络凭据。

## 4. NuttX 与其他第三方组件

OpenVela/NuttX 及其子组件继续适用各自许可证。`nuttx/` 覆盖层保留原文件头，
其固定依赖见 `nuttx/dependencies.lock.json`。若最终构建实际链接 BSD、MIT 或
其他许可证组件，提交包应同时保留对应许可证/NOTICE，不能用本仓根
Apache-2.0 取代。

## 5. 辅助对照（未复制代码）

- `open-vela/contest2026_264_VelaSightsuixingAIzhinengyanjing`
  `ba87d984a1e08bbcd5c09734700a3d4574739158`：仅用于外设调用链对照；其
  CPU0 仍为 Armino，未作为全 OpenVela 主 BSP。
- `open-vela/nuttx` PR #332，提交
  `8dbe907a8461c3b6b5ceddf3c0fcf7a690df1ffd`：仅用于 CPU0-only 启动对照。

完整源码审查结论见 `docs/BK7258参考BSP审查.md`。
