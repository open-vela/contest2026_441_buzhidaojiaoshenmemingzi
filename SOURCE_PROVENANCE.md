# 源码来源与复用边界

本文件记录终版提交中的组件边界、许可证和本队修改。各源文件的版权头和组件许可证继续以源文件为准。

## OpenVela 与 NuttX

`chips/bk7258/`、`boards/bk7258/`、`nuttx/` 和 `tools/bk7258/` 组成 BK7258 R1 的 OpenVela/NuttX BSP。CP/AP 配置、R1 引脚绑定、分区表、构建工具和视觉应用由本队维护。

本队新增和维护内容包括：

- `team441_cp`、`team441_ap` 两套 CP/AP 配置；
- CPU0、AP、CPU2 SMP、Mailbox、共享内存和 RPTUN/RPMsg 的板级接入；
- R1 双屏、Camera、SD、Wi-Fi、按键和电源的设备注册；
- `app/bsp_diag/` 诊断命令与 `app/vision_badge/` 视觉识别应用；
- 构建、分区检查、镜像编码和证据清单工具。

## Beken SDK

Beken SDK 作为 BK7258 芯片库和硬件接口依赖，由 manifest 固定版本。SDK 目录不包含本队应用业务；本仓不写入设备密钥、Wi-Fi 密码、MAC、RF 校准或用户分区数据。

双屏眼睛动画位于 `boards/bk7258/aidk_ai_toy/src/bk7258_aidk_eye.c`，动画资产和调度均绑定 R1 板级实现。

## 许可证

OpenVela/NuttX、Beken SDK 及其子组件继续适用各自许可证。`nuttx/` 覆盖层保留原文件头，固定依赖记录在 `nuttx/dependencies.lock.json`。根目录 Apache-2.0 声明不取代第三方源文件和组件原有的许可证义务。
