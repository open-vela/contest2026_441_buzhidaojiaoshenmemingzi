# 服务端配置示例

这些文件只有占位值。复制到仓库之外的服务运行目录，移除`.example`后缀并填写自己的配置；不要用示例值连接云服务，也不要把填好的文件提交回来。

| 私有文件 | 用途 |
| --- | --- |
| `config/agora.env` | RTC应用身份与服务端会话API凭据 |
| `config/agora-lan.cfg` | 设备可达的服务地址、鉴权key与基础音量 |
| `config/agora-service-key.txt` | 单独生成的64位小写十六进制随机key，与agora-lan.cfg的device_key一致 |
| `config/agora-pipeline.json` | 已配置语音管线的身份 |
| `config/voice-provider.local.json` | 语音会话使用的对话模型配置 |
| `config/mimo-vision.local.json` | MiMo主要视觉后端的普通API配置 |

语音会话模型与视觉模型是不同配置；选择MiMo视觉不会隐式替换语音ASR/LLM/TTS。GLM可按服务端参数显式选择为辅助视觉后端。

`server_host`使用设备可达且服务主机实际拥有的局域网IPv4，不填公网工具回调地址或127.0.0.1。`volume`是原控制协议的PCM百分比，不是十档音量的档号；产品层默认五档并保存手动选择。

完整目录、依赖、启动和设备配置步骤见[服务端部署](../../docs/server.md)。
