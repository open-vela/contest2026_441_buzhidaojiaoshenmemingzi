# 产品应用

目录：`app/vision_badge/`。按核心职责划分为**7组**；目录内源/配置文件46项。

| 职责 | 关键入口 | 已有基础 | 本项目适配/功能工作 |
| --- | --- | --- | --- |
| 命令与跨核业务 | vision_badge_main.c、vision_badge_rpc.c | 复用NSH与RPMsg框架 | 产品命令、请求关联和跨核派发 |
| 会话控制 | agora_control.c、agora_service.c | 复用RTC SDK与Audio接口 | 同场观察、取消/退出、租约与错误清理 |
| 相机服务 | camera_service.c | 复用Video与GC2145/DVP | 热相机单owner、新帧/归还、超时和有界恢复 |
| 音频服务 | audio_service.c | 复用NuttX Audio | 采集/播放准备、资源释放、PCM音量和连续性合同 |
| 语音唤醒入口 | wake_service.c、wake_asr_udp.c | 复用固定ASR及网络接口 | 有界PCM传输、文本匹配和资源owner交接 |
| 电源与设置 | power_service.c、agora_control.c | 复用底层PM/存储能力 | 退出排空、音量记录、开关机协调 |
| 状态与流程反馈 | feedback_service.c、workflow.c | 复用board反馈接口 | 统一产品阶段；图形仅作为板级状态显示 |


[返回模块总览](README.md)
