# AP执行域

目录：`chips/bk7258/ap/`。按核心职责划分为**8组**；目录内源/配置文件74项。

| 职责 | 关键入口 | 已有基础 | 本项目适配/功能工作 |
| --- | --- | --- | --- |
| AP启动、SMP与IPI | bk7258_ap_start.c、bk7258_ap_smp.c、bk7258_ap_ipi.c | SMP主体与固定公开基础相同/仅注释差异 | 本项目配置、运行接入与核验 |
| 生命周期与健康状态 | bk7258_ap_lifecycle.c、bk7258_ap_health.c | 已有平台状态与心跳基础 | 结合CP监督检查在线和阶段，保留角色边界 |
| 音频输入与输出 | bk7258_mic.c、bk7258_aud.c | 复用Audio upper-half、SDK与已有lower-half | 已确认MIC连续性/丢样本诊断关联0cb374f；AUD的curbyte/关闭合同差分归为当前适配 |
| 相机与JPEG媒体 | bk7258_dvp.c、bk7258_jpeg_encoder.c | 复用Video/硬件媒体基础 | 增加DVP交付诊断并协调ready帧/调度，关联39a36d0、c3fe256 |
| 显示与图形接入 | bk7258_lvgl_fb.c、bk7258_lcd_spi.c、bk7258_qspi.c | 复用LCD接口和图形组件 | 接入LVGL状态反馈及R1双屏路径，关联d87cf53；属于驱动适配 |
| 总线、DMA与器件访问 | bk7258_dma.c、bk7258_i2c.c、bk7258_spi.c、bk7258_pinmux.c | 已有总线与SDK硬件叶 | 按板配置绑定并使用 |
| 网络、蓝牙与USB | bk7258_wifi.c、bk7258_bt_hci.c、bk7258_usbhost.c | 已有通信控制器/协议接入基础 | 产品选用网络等既有能力，其他可选能力按配置分别核查 |
| 电源与镜像协作 | bk7258_pm_coord_ap.c、bk7258_pm_client.c、bk7258_ota_manager.c | 已有跨域服务与镜像接口 | 产品退出对接资源排空 |


[返回模块总览](README.md)
