# 框架覆盖集成

目录：`nuttx/`。按核心职责划分为**3组**；目录内源/配置文件11项。

| 职责 | 关键入口 | 已有基础 | 本项目适配/功能工作 |
| --- | --- | --- | --- |
| LCD设备覆盖 | drivers/lcd/、include/nuttx/lcd/ | 复用NuttX设备框架及公开器件实现 | R1显示所需GC9D01接口集成 |
| 其他可选器件接口 | drivers/sensors/、drivers/contactless/、drivers/input/ | 已有可选器件实现/声明 | 保留构建闭包，是否启用按当前配置，不计入产品功能清单 |
| 依赖身份 | dependencies.lock.json | 固定上游框架 | 跟踪实际依赖树与覆盖范围 |


[返回模块总览](README.md)
