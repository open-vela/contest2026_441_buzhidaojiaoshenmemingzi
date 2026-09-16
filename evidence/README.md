# 真机证据索引

本目录只保存可公开、已检查的证据副本。原始日志仍保存在本地，不把 Wi-Fi
凭据、API Key、设备私有分区或用户信息提交到仓库。

## 证据分级

| 目录 | 固件路线 | 能证明什么 | 不能证明什么 |
| --- | --- | --- | --- |
| `cpu0-only/` | CPU0-only OpenVela | CPU0 烧录、NuttX/NSH 最小启动 | AP、Wi-Fi 和外设终版状态 |
| `full-openvela-experimental/` | 全 OpenVela 实验镜像 | CPU0 启动 AP、RPTUN、CPU2 SMP、Wi-Fi 当次成功 | 当前终版提交已构建或已回归 |
| `mixed-reference/` | CPU0 Armino + AP OpenVela 历史对照 | R1 双屏、Camera/JPEG、Audio、电源硬件链可用 | 全 OpenVela 终版也已通过 |
| `camera/` | 历史 Camera 实拍 | GC2145 曾输出可解码 JPEG | 连续帧率和长期稳定性 |

## 当前文件

- `full-openvela-experimental/ap-rptun-smp-wifi-redacted.log`：保留状态与时序，
  SSID、密码、MAC 和局域网地址已替换为占位符；
- `mixed-reference/vision-display-regression.log`：640×480 单帧、JPEG 与双屏
  历史回归；
- `mixed-reference/audio-recplay-16000.log`：16 kHz/16 bit/mono 录放音历史
  回归；
- `mixed-reference/key1-power-regression.log`：长按电源行为历史回归；
- `camera/gc2145-sample.jpg`：板端导出的相机样例。

终版镜像生成后，还必须新增同一 Git 提交和同一镜像 SHA256 对应的启动、多核、
Wi-Fi、显示和至少一条多媒体回归日志，才能把相应状态从“实验/历史”升级为
“终版通过”。
