# 构建与服务工具

目录：`tools/`。按核心职责划分为**5组**；目录内源/配置文件113项。

| 职责 | 关键入口 | 已有基础 | 本项目适配/功能工作 |
| --- | --- | --- | --- |
| 构建与布局 | bk7258/bk7258.py、bk7258/_lib/ | 已有构建/镜像机制与依赖闭包 | 成对配置、层级/布局和输入身份追踪 |
| 依赖恢复 | restore_workspace.py、lvgl_dependency.py | 固定Git/SDK/工具链与组件 | 复原目录映射、校验依赖和兼容差分 |
| 语音会话服务 | agora/session_server.py | 复用语音平台API | 设备控制、租约与工具请求生命周期 |
| 视觉与唤醒服务 | agora/scene_relay.py、agora/local_asr_wake.py | MiMo/GLM API和固定ASR引擎 | 场景目标、结果限制、音频传输与唤醒接入 |
| 针对性验证 | agora/run-offline-checks.sh、bk7258/test_* | 标准测试与编译工具 | 真实实现/负向场景检查，硬件边界与结果单独记录 |


[返回模块总览](README.md)
