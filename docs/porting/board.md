# R1板级绑定

目录：`boards/bk7258/aidk_ai_toy/`。按核心职责划分为**5组**；目录内源/配置文件34项。

| 职责 | 关键入口 | 已有基础 | 本项目适配/功能工作 |
| --- | --- | --- | --- |
| 器件与引脚 | include/bk7258_board_config.h | 固定公开板定义；可见差别仅注释 | 沿用R1实际映射 |
| 初始化与注册 | src/bk7258_board_bringup.c | 已有board bring-up | 接入可选诊断、显示任务和vision_badge RPC及错误统计 |
| 按键与产品控制 | src/bk7258_aidk_buttons.c | 已有GPIO轮询骨架 | 三键映射、十档音量和三秒电源交互，关联bd64b12、2e1ad19 |
| LCD与图形验证 | src/bk7258_aidk_eye.c、src/bk7258_aidk_eye_status.c | 已有屏幕驱动/图形库 | 固定LVGL组件接入与状态反馈，属于适配证据 |
| 成对配置与分区 | configs/、openvela.conf、分区CSV | 板型与构建规则基础 | 维护CP power/AP product配对、功能选择和分区身份 |


[返回模块总览](README.md)
