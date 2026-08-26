# 资料地图与刷新规则

## 权威顺序

发生冲突时，按以下顺序取证：

1. 实际发放 R1 套件的原理图、BOM、数据手册、出厂分区和实测器件 ID。
2. 当前 openvela/NuttX 主线与大赛官方文档。
3. 已合并的厂商移植。
4. 固定提交的公开 PR。
5. ARMINO、厂商示例和相似 BK7258 开发板。
6. 个人笔记与推断。

低级别资料不能覆盖高级别资料；无法确认时明确写“待 R1 实机/资料验证”。

## 首选资料

- [大赛支持硬件](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/hardware_porting/supported_hardware.md)
- [声网开发套件使用说明](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/attachment/%E5%A3%B0%E7%BD%91%E5%BC%80%E5%8F%91%E5%A5%97%E4%BB%B6%E4%BD%BF%E7%94%A8%E8%AF%B4%E6%98%8E.pdf)
- [BK7258 AIDK 硬件资料](https://docs.bekencorp.com/arminodoc/bk_aidk/bk7258/zh_CN/v2.0.1/hw-reference/index.html)
- [open-vela/nuttx BK7258 PR #332](https://github.com/open-vela/nuttx/pull/332)
- [vendor_beken PR #2](https://github.com/open-vela/vendor_beken/pull/2)
- [Agora BK7258 示例分支](https://github.com/AgoraIO-Community/Conversational-AI-IOT-Sample/tree/bk7258/v2.0.1)
- [Beken AI solution](https://github.com/bekencorp/bk_solution_ai)
- [官方 NuttX 驱动开发 skill](https://github.com/open-vela/.claude/tree/dev-ai-contest-2026/skills/nuttx-driver-development)
- [官方 openvela 构建 skill](https://github.com/open-vela/.claude/tree/dev-ai-contest-2026/skills/openvela-build)
- [官方 PCM 音频 skill](https://github.com/open-vela/.claude/tree/dev-ai-contest-2026/skills/pcm-audio)

## 使用公开 PR 前

本 skill 的工作方式参考官方 `nuttx-driver-development`（主）、`openvela-build`、`pcm-audio`（辅）；这里是项目化整理，不是这些上游 skills 的完整复制，也不自动安装或执行它们。

本次阅读的官方 skills 仓库快照为 `fa2ead7db669912bce49fb04e9876505b5455853`。本项目 PR 版本复核记录在 `docs/progress/BK7258基线快照.md`（相对项目根目录）；文档中的历史评估不能自动适用于新的 PR 头提交。

1. 查询 PR 是否仍开放、基分支和最新头提交。
2. 记录仓库、分支、提交哈希、查询日期和是否发生强推。
3. 阅读 diff、评审意见、CI 结果和许可证。
4. 将公开板与 R1 套件逐项比较：SoC 修订、Flash/PSRAM、晶振、UART、DVP、音频 codec、屏幕、按键和电源。
5. 只摘取已理解的最小变化；保留来源和本地改动说明。

网络资料会变化。开始新阶段或引用 PR 前重新刷新，不依赖旧笔记中的“最新”。
