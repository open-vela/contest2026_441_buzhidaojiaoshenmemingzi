# CP控制域

目录：`chips/bk7258/cp/`。按核心职责划分为**7组**；目录内源/配置文件47项。

| 职责 | 关键入口 | 已有基础 | 本项目适配/功能工作 |
| --- | --- | --- | --- |
| 系统启动与基础运行 | bk7258_start.c、bk7258_vectors.c、bk7258_cp_platform.c | 已有启动/架构基础 | 维护CP配置与运行接入 |
| AP控制与监督 | bk7258_ap_control.c、bk7258_ap_supervisor.c | 已有多核控制基础 | 接入产品诊断与状态核对 |
| 时钟、GPIO与系统控制 | bk7258_clock.c、bk7258_gpio_lowerhalf.c、bk7258_wdt.c | 已有芯片/SDK接口 | 维护OS接口与板级使用 |
| Flash与存储事务 | bk7258_storage.c、bk7258_flash.c、bk7258_flash_mtd.c | 复用NuttX与固定SDK存储接口 | 新增只读QUIESCE屏障以适应无ONCHIP_DATAFS拓扑，关联31732fd |
| 电源协调与软关机 | bk7258_pm_coord_cp.c、bk7258_pm_soft_off.c | 复用既有PM接口和SDK睡眠入口 | 三秒门限、按键释放/唤醒恢复顺序及产品退出协调，关联bd64b12、0063868 |
| 跨域外设服务 | bk7258_saradc_server.c、bk7258_temperature_server.c、bk7258_bt_controller.c | 已有平台服务骨架 | 按实际配置选用 |
| 镜像与设备管理 | bk7258_ota_pair.c、bk7258_ota_image.c、bk7258_identity.c | 已有镜像/设备基础 | 保持交付身份与当前direct启动一致 |


[返回模块总览](README.md)
