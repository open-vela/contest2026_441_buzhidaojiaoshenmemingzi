# 启动加载层

目录：`chips/bk7258/bootloader/`。按核心职责划分为**3组**；目录内源/配置文件34项。

| 职责 | 关键入口 | 已有基础 | 本项目适配/功能工作 |
| --- | --- | --- | --- |
| 早期运行条件 | start.S、boot_clock.c、boot_runtime.c | 已有BootROM/SDK及启动实现 | 保留当前direct启动的时钟、内存和运行合同 |
| Flash与载入 | boot_main.c、boot_flash.c、bootloader.ld | 已有Flash/链接基础 | 核对CP/AP镜像位置与加载边界 |
| 镜像策略与完整性 | boot_bl1_policy.c、boot_bl1_manifest.c、boot_sha256.c | 已有镜像处理与校验代码 | 当前交付按实际入口核查 |


[返回模块总览](README.md)
