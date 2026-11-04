# 服务端部署与接口

## 服务职责

| 模块 | 入口 | 职责 |
| --- | --- | --- |
| 会话网关 | tools/agora/session_server.py | 设备鉴权、会话租约、启动/退出与请求关联 |
| 视觉中继 | tools/agora/scene_relay.py | 发送当前JPEG、保留用户目标、限制结果与期限 |
| 场景策略 | tools/agora/vision_policy.py | 面向场景的简短回答和工具协议 |
| 唤醒识别 | tools/agora/local_asr_wake.py | 流式ASR及完整唤醒短语匹配 |
| 资源锁 | local-asr-model.json、requirements-local-wake.txt | 固定运行依赖与ASR模型资产 |

MiMo为主要视觉后端，GLM为辅助可选后端。部署时显式配置模型、API与凭据，不将凭据编入设备或提交到仓库，不在请求失败后自动换模型。

## 配置与启动

准备Python及requirements-local-wake.txt中的固定依赖，按local-asr-model.json恢复模型并核对SHA256。模型权重部署在服务端运行目录；真实配置和设备密钥独立保管。

`python tools/agora/session_server.py --help`列出当前版本可用参数；Windows部署可使用`tools/agora/start-product.ps1`。启动前按脚本帮助准备R1_LOCAL_ROOT指定的配置和模型目录，显式指定MiMo视觉配置及回调地址。先执行CheckOnly类预检，再启动服务。

设备和服务端需可达；语音平台调用视觉工具时，工具回调地址也需可达。对外仅提供配置的工具接口，拒绝缺少鉴权或不匹配的请求。临时回调进程存活并不代表连接健康，部署应检查实际回调路径。

启动服务只进入待命，设备通过按键或唤醒请求会话。主动退出、设备失联与凭据期限触发清理；异常保留可定位阶段和错误码，日志不输出凭据、原始音频或图片。


## 服务运行目录

运行目录独立于Git仓库，例如`C:/r1-service`，通过`-LocalRoot`或`R1_LOCAL_ROOT`指定：

```text
service-root/
├── config/                  从config/examples复制并填写
├── asr-venv/                固定服务Python依赖
├── models/
│   └── sherpa-zh-en-98590b7/ 固定ASR资产与verified-inputs.json
└── tools/
    ├── agora-token-reference/ 固定Token参考与LICENSE
    └── cloudflared/           工具回调转发程序（或用参数另指定）
```

## Windows部署步骤

以下命令使用PowerShell 7，在仓库根目录运行，替换运行目录和串口参数。执行环境须允许受信任的本地脚本。配置文件全部在服务运行目录，示例中不包含真实凭据。

```powershell
python -m venv C:/r1-service/asr-venv
& C:/r1-service/asr-venv/Scripts/python.exe -m pip install -r tools/agora/requirements-local-wake.txt
& ./tools/agora/bootstrap-token-reference.ps1 -Destination C:/r1-service/tools/agora-token-reference
& C:/r1-service/asr-venv/Scripts/python.exe tools/agora/prepare_local_asr.py --model-root C:/r1-service/models/sherpa-zh-en-98590b7 --restore
```

按[配置示例](../config/examples/README.md)准备私有文件，另生成随机设备key，同步写到`agora-service-key.txt`和`agora-lan.cfg`。为回调准备cloudflared可执行文件；可用`-CloudflaredPath`指定实际位置，不依赖旧开发机器目录。

先预检，再由操作者显式启动：

```powershell
& ./tools/agora/start-product.ps1 -LocalRoot C:/r1-service -CloudflaredPath C:/tools/cloudflared.exe -MimoVisionConfig C:/r1-service/config/mimo-vision.local.json -CheckOnly
& ./tools/agora/configure-device.ps1 -ConfigPath C:/r1-service/config/agora-lan.cfg -PortName <device-port> -CheckOnly
```

两个CheckOnly都不启动模型、云会话或串口操作。占位符未替换、Python/模型/工具未安装或服务地址不属于本机时，应先修正部署条件。

确认设备在线后，去掉设备配置命令的CheckOnly以写入配置；再去掉启动命令的CheckOnly启动待命服务。设备通过中键或唤醒进入对话，使用云会话时按服务计费。停止启动窗口/控制入口后应确认会话与临时回调退出。

Linux服务端可直接使用`session_server.py`，设置相同的`R1_LOCAL_ROOT`及`AGORA_TOKEN_REFERENCE`，并显式传入bind、vision、MiMo配置、模型目录、product-mode和工具回调等参数。完整参数由`--help`列出；语音平台只应访问`/vision/tool`回调，不将设备控制端口直接公开。
