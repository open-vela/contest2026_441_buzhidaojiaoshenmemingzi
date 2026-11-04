# 版本与验证记录

验证按主题组织，关联同一组源码/配置和镜像身份：

| 目录 | 记录 |
| --- | --- |
| [build](build/identity.json) | 成对配置、源输入与编码镜像身份；[仓库与部署检查](build/repository-checks.json) |
| [system](system/status.json) | CP/AP、SMP、RPTUN和监督状态 |
| [media](media/lifecycle.json) | 采图、播放、资源退出及局部阶段耗时 |
| [drivers](drivers/graphics.json) | LVGL到显示接口的组件适配链 |
| [reproducibility](reproducibility/build.json) | 固定输入恢复、独立输出构建及验证 |
| [source](source/modules.json) | 模块职责与具体差分；[交付源输入](source/delivery.json)关联固件版本 |

验证记录按主题组织，并通过源码输入、生成配置和镜像哈希关联同一版本。upstream_record_sha256用于与对应原记录核对；字段中的scope说明该项检查覆盖的对象与执行环境。

构建/离线与设备行为分别记录，单独采图或播放时长不解释为整场交互延迟。LCD属于驱动和组件适配，渲染提交与实际像素观察分开处理。AI开发对话和私人机器状态不放在这些技术证据里。
