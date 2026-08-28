---
name: bk7258-openvela-porting
description: Port BK7258 R1 to openvela/NuttX, synchronize the team-owned board/chip overlays, and review vision_badge hardware evidence. Use for R1 bring-up, driver integration, public-port evaluation, reproducible builds, and capability audits; not generic ARMINO/FreeRTOS apps.
---

# BK7258 R1 openvela 移植

以证据门禁推进 BK7258 R1 移植。公开 PR、厂商示例和相似开发板只作为候选参考，直到版本、许可证、评审状态、R1 硬件差异、构建和实机结果均已核对。

## 路由

- 开始前阅读 [references/source-map.md](references/source-map.md)，确认资料权威级别和当前基线。
- 宣称能力落地前阅读 [references/evidence-gates.md](references/evidence-gates.md)。
- 在完整工作区构建前运行 `bash scripts/sync-openvela-port.sh --check`。队伍仓的 `board/bk7258-r1/` 是板级来源；`porting/nuttx/arch/arm/src/bk7258/` 是芯片层维护镜像。只有明确需要时才使用 `--install` 或 `--capture`，随后检查 Git diff。
- 审查本项目时运行 `python scripts/audit_project.py <项目根目录> --json`（脚本路径相对本 skill），再人工核对运行证据。退出 2 表示审查输入不完整，不是“没有问题”。
- 新驱动优先沿用 NuttX/openvela 子系统骨架；仅将目标硬件常量和必要的底层适配带入板级代码。

## 协作审查

- 遇到可独立复核的日志归纳、公开代码差异、文档一致性或候选方案时，优先调用
  已登录的 MiMo 与 Claude CLI 做只读初审；主执行者负责拆分任务、核对原始输入、
  复现关键结论并作最终决策。
- 辅助模型不得直接获得仓库写权限，也不得接收或输出密钥。需要现有 CLI 登录环境
  时，只在进程内从本机配置加载，禁止把凭据打印到命令行输出、文档或日志。
- 辅助结论必须与本地源码、生成配置、ELF/镜像和 R1 原始日志交叉检查。附件缺少
  新增日志、来源层级混淆或无法复现的建议一律不采纳。
- CLI 未登录、无输出或响应过慢时及时停止，不让辅助审查阻塞构建和实机验证。

## 不变量

1. 未核对原理图、BOM、器件 ID 和引脚前，不把 R1 套件等同于公开的 bk7258-devkit。
2. 未合并 PR 不视为正式发布或本队实机证据；记录仓库、分支和固定提交。
3. 未保存恢复路径、分区表、校准区与 MAC 数据前，不执行整片擦除。
4. ARMINO 仅位于厂商边界以下；应用层使用 NuttX/openvela API、文件描述符和 errno 语义。
5. 驱动架构参考 NuttX 同类实现，寄存器、引脚和时序常量参考目标板权威资料。
6. 明确区分：规划、接口占位、实现存在、构建验证、R1 实机验证、能力落地。100 帧、10 次冷启动等是本项目自定测试目标，不是大赛统一强制指标；具体阶段可以按依赖并行。
7. 不编造构建、烧录和运行结果；保存命令、版本、哈希、原始日志及失败样本。
8. 不把整个公共 NuttX 仓复制进队伍仓；只维护有意修改的镜像文件，并固定其上游基线。注释变成功能改动时必须明确升级状态和验证要求。

## 推进顺序

1. 盘点工具链、SDK、openvela/NuttX、厂商代码和公开 PR 的固定版本。
2. 建立恢复与烧录路径，再完成 CPU0 启动和 NSH。
3. 验证 PSRAM、GPIO、定时器，再依次推进 Wi-Fi、相机、音频和显示。
4. 每个驱动同时检查 Kconfig、构建接入、板级注册、错误路径、中断/DMA、缓存一致性、重复开关和负向测试。
5. 应用层一次替换一个 ENOSYS 后端，保持主流程可观测且可回退。
6. 依次通过该功能所依赖的干净构建、镜像打包、实机证据门禁；例如离线相机验证不要求 Wi-Fi 先完成。失败时记录阻塞，并转向不依赖该条件的本地工作。

## 输出格式

每次工作报告：固定基线、修改层级、已通过的最高门禁及证据、阻塞项与下一实验、图形/AI/多媒体是否已落地、建议的中文分步提交。除非当前请求明确授权，不提交、不推送、不烧录、不擦除，也不创建或合并 PR。
