# 跨域公共层

目录：`chips/bk7258/common/`。按核心职责划分为**6组**；目录内源/配置文件42项。

| 职责 | 关键入口 | 已有基础 | 本项目适配/功能工作 |
| --- | --- | --- | --- |
| IRQ、tick与堆 | bk7258_irq.c、bk7258_timerisr.c、bk7258_allocateheap.c | 复用NuttX/SDK及公开节拍实现 | 核对时钟/堆边界与当前配置 |
| Mailbox与RPTUN | bk7258_rptun.c、bk7258_rptun_mbox.c | 已有RPTUN/RPMsg及Mailbox基础 | 维护两侧配置和资源合同 |
| SDK与OS边界 | bk7258_sdk_runtime.c、bk7258_sdk_irq.c、bk7258_os_adapt.c | 固定SDK与已有兼容层 | 维持ABI、执行上下文和依赖边界 |
| 串口与诊断输出 | bk7258_serial.c、bk7258_lowputc.c | 已有串口/console基础 | 作为启动、跨核和媒体证据入口 |
| 跨域服务与文件接口 | bk7258_rpmsgfs.c、bk7258_wifi_control.c | 已有跨域基础接口 | 维护服务接入和错误传播，具体能力按配置核查 |
| 共享电源和镜像合同 | bk7258_pm_activity.c、bk7258_ota_rpmsg.c、bk7258_boot_slot.c | 已有共享协议/布局定义 | 产品退出与交付配置基于该层 |


[返回模块总览](README.md)
