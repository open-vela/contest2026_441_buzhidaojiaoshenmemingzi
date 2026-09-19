# vision_badge 应用

该目录通过队伍 manifest 映射到
`apps/system/contest2026_441_vision_badge`，提供 BK7258 R1
视障视觉辅助胸牌的 OpenVela 原生应用。

当前版本固定了四个服务边界：

- `camera_service`：V4L2 设备探测与单帧 JPEG 采集接口；
- `vision_service`：MiMo 图像问答请求与有界结果接口；
- `audio_service`：板载麦克风 PCM 采集接口；
- `feedback_service`：控制台、语音和振动反馈接口。

Camera 使用 V4L2 MMAP 采集 640×480 JPEG；MiMo 适配器以流式 Base64 请求
`/v1/chat/completions`，并执行 TLS 证书与域名校验。CPU0 的 NSH 命令通过
有界 RPMsg 请求 AP 执行相机和 MiMo 流程，入口为：

```text
vision_badge status
vision_badge run "入口在哪里"
短按 KEY1（无需输入命令）
```

`run` 与 KEY1 短按均可触发“采集 → MiMo 视觉理解 → 控制台反馈”的状态机。
r14 实机保存了两次 `stage=done status=0` 的查询，其中 KEY1 路径在 CP 串口打印
中文识别正文。API Key 从
`/mnt/sdnand/prov/mimo_api_key` 读取，首尾空白会被忽略；若该文件不存在，会兼容
读取旧产品已写入的 `prov/wifi.bin` 或 `prov/vela.cfg` VSWP 记录。密钥不会编译进固件。
若 SD-NAND 尚未挂载，应用会尝试挂载 `/dev/mmcsd0p0`，不存在时使用
`/dev/mmcsd0`。

应用层通过标准设备路径和服务接口与板级实现隔离，因此硬件切换不改变
“采集 → 云端理解 → 反馈”的状态机。BK7258 的 Camera、Audio、Wi-Fi
设备节点已由 r13/r14 实机日志验证。语音播放和振动反馈保持为后续恢复项。

## 主机回归

在项目根目录的 Linux 主机执行 `bash scripts/test-host.sh`，用本机 GCC 严格
编译应用与接口回归测试，检查参数校验、主机占位行为、输出复位及失败阶段。
生成物放入已忽略的 `build/host/`。测试不打开设备、不请求模型，不能代替
BK7258 固件构建或实机验收；替换占位后端时要同步更新对应测试契约。
