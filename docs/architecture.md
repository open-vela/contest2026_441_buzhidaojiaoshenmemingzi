# 系统架构

## 处理器与职责

| 运行域 | 职责 | 主要接口 |
| --- | --- | --- |
| CP / 物理CPU0 | 系统入口、AP控制监督、按键及电源协调 | NSH、Mailbox、RPTUN/RPMsg |
| AP SMP / 物理CPU1和CPU2 | 一个NuttX实例，运行外设与产品服务 | Audio、Video、LCD、网络与应用RPC |
| 服务端 | 唤醒识别、会话控制、视觉工具中继 | 设备鉴权、PCM、JPEG和会话租约 |
| 云服务 | 语音会话与视觉理解 | RTC及显式配置的模型API |

CP和AP通过共享内存与Mailbox连接RPTUN/RPMsg。AP两个调度CPU共享调度器和地址空间；CP是另一套openvela/NuttX实例。

## 一次观察的生命周期

语音提出观察目标→会话工具请求→设备获取新帧→服务端视觉理解→原会话语音回答。每个请求有关联身份和期限，旧回复不更新新请求；图像/音频缓冲的producer、consumer和释放责任独立。

相机保持单owner，采图后归还驱动缓冲；音频消费者独立推进，等待时可播放接应。退出会话时取消挂起请求并释放资源。音量作用于输出PCM，不改采样率或采音内容。

## 设备接口

- 图像：GC2145 / DVP / JPEG → NuttX Video接口 → camera_service。
- 音频：MIC与DAC → NuttX Audio接口 → audio_service与RTC。
- 控制：三键 → 产品控制 → RPC/应用服务；电源先排空与存储屏障。
- 显示适配：LVGL → LCD/framebuffer适配 → GC9D01双屏，用于板级图形验证。

详细角色、代码位置与本项目修改见[平台模块表](porting/README.md)。
