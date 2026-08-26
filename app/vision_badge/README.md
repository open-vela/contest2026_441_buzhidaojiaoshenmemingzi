# vision_badge 应用骨架

该目录通过队伍 manifest 映射到
`packages/demos/contest2026_441_vision_badge`，提供 BK7258 R1
视障视觉辅助胸牌的 openvela 原生应用骨架。

当前版本固定了四个服务边界：

- `camera_service`：V4L2 设备探测与单帧 JPEG 采集接口；
- `vision_service`：MiMo 图像问答请求与有界结果接口；
- `audio_service`：板载麦克风 PCM 采集接口；
- `feedback_service`：控制台、语音和振动反馈接口。

除设备节点探测和控制台反馈外，硬件/云端后端暂时明确返回 `-ENOSYS`，
避免把未经过真机验证的占位逻辑标成已完成。命令行入口为：

```text
vision_badge status
vision_badge selftest
vision_badge run "入口在哪里"
```

`run` 已串起“采集 → 云端理解 → 反馈”的状态机；在 M3、M5、M6
后端依次实现前，它会在对应阶段失败并输出错误，而不会返回伪造结果。

应用层通过标准设备路径和服务接口与板级实现隔离，因此硬件切换不改变
“采集 → 云端理解 → 反馈”的状态机。BK7258 的 Camera、Audio、Wi-Fi
设备节点和能力要在 BSP 完成后通过真机探测确认，当前默认路径只是接口约定。

## 主机回归

在项目根目录的 Linux 主机执行 `bash scripts/test-host.sh`，用本机 GCC 严格
编译应用与接口回归测试，检查参数校验、ENOSYS 占位行为、输出复位及失败阶段。
生成物放入已忽略的 `build/host/`。测试不打开设备、不请求模型，不能代替
BK7258 固件构建或实机验收；替换占位后端时要同步更新对应测试契约。
