# 第三方组件与许可证

本项目按Apache-2.0发布本队代码；组件及源文件的原版权头、许可证和NOTICE继续有效。本文件列出构建/运行依赖与公开基础，描述组件关系。

| 组件/基础 | 固定身份或锁入口 | 使用范围与许可证 |
| --- | --- | --- |
| openvela/NuttX | `76354c637858ecb0aa4601629327acb6f44a26bb` | 内核、SMP、RPTUN、Audio/Video等框架；Apache-2.0及各文件声明 |
| openvela apps | `dcc6a95c3b323e533c98fde8fb209f99e24f0fdd`加锁定集成差分 | 标准应用/库，Apache-2.0及各组件声明；差分见公开补丁 |
| BK7258公开BSP基础 | `Embracecactus/contest2026_135_yongwangzhiqian@930d75ae34f215dcaf8707751d5e27cc59ec7ca9`固定对照 | 启动/节拍/SMP等有相同实现；头部Apache-2.0 |
| Beken SDK | `tools/bk7258/`所用SDK锁及build-manifest | 固定硬件库/头文件，保留随SDK许可证；不以本仓许可证重授闭源部分 |
| openvela LVGL | `tools/bk7258/lvgl.json` | 图形组件；MIT及依赖声明 |
| sherpa-onnx | v1.13.8 / `11afbd009a7f8c08f4bcf2fc1b265d0df4670fbf` | 电脑辅助ASR引擎；Apache-2.0 |
| 双语ASR模型 | `tools/agora/local-asr-model.json`固定98590b7版本与资产SHA | 模型卡Apache-2.0；大权重驻电脑仓外，不放入代码候选 |
| Agora Token参考实现 | `AgoraIO-Community/Conversational-AI-IOT-Sample@2e736ebefdb7ffb2832ae1a6f95039d8589e1f85` | MIT，保留原文件与许可；部署时固定恢复 |
| Agora RTC SDK与云服务 | 当前SDK锁/显式私有部署配置 | 依随SDK/服务协议使用；不将服务或声学算法称为本队实现 |

GLM/MiMo视觉调用为显式配置的API接入，不把服务端模型源码纳入本仓。可选本地KWS候选依赖的版本、许可和兼容差分见`tools/bk7258/wake.json`及各原文件声明。

各组件的源文件头和实际依赖LICENSE共同构成许可说明。
