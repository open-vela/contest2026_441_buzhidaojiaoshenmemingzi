---
name: bk7258-openvela-porting
description: Review or plan BK7258 CP/AP openvela board integration, driver lifecycle contracts, and reproducible firmware handoff. Use for a BK7258 board bring-up, a same-chip board variation, a driver change, or a CP/AP export audit. Not a generic MCU flashing tool or proof of hardware operation.
---

# BK7258 openvela适配与交付审查

把目标板的输入、系统角色、设备合同和交付证据连接起来。使用者提供自己的工作区、板型和版本；R1仅是可选案例，不是所有板的默认配置。

## 输入与任务选择

先阅读[输入矩阵](references/inputs.md)，从已有仓库与资料确定project_root、board、CP/AP配置、SDK/工具链版本及本次目标。缺失硬件事实记为unknown，不用R1常量补空。

- **新板/同芯片变体**：输出差异矩阵、应改层级和按依赖排序的bring-up步骤。板级差异留board，通用芯片能力留chip。
- **驱动或媒体改动**：读取[接口与生命周期](references/contracts.md)，先核对固定upper/lower调用合同，再定位最小差分及验证边界。
- **构建/交付审查**：读取[交付检查](references/handoff.md)，使用项目原有构建/布局工具。只有匹配支持的扁平导出格式才调用随附只读检查器。
- **参考实例**：需要具体问题到代码的映射时读[R1案例](references/r1-example.md)，不把案例结果外推到自己的板。

## 工作流程

1. **定位输入**：读实际配置、依赖锁、原理图/器件/引脚资料，区分板型、CPU域和系统实例。
2. **选择层级**：启动/IRQ/堆、多核/跨核、board注册、子系统驱动、产品服务、构建交付；说明实际受影响层。
3. **核对合同**：上下文/锁、缓冲域/owner、错误返回、取消/deadline、generation、SDK ABI和板级时序。
4. **形成最小修改**：延续已有骨架，先修正确性与退出路径，不另造协议栈或用整仓覆盖解决局部问题。
5. **按风险验证**：先相关语法/配置/运行用例，再成对构建/布局，最后在需要时安排对应硬件行为；已通过项目不无故重跑。
6. **交付审查**：关联源、dirty差分、依赖、生成配置、ELF、镜像和实际结果，输出清晰的可复核结论。

规划不要求立即修改或连接设备。用户只要求静态检查/材料整理时，停留在授权范围内，不启动云服务或硬件实验。

## 关键约束

- 物理CPU与逻辑CPU分别描述；不能从CP/AP名称推出任意目标的核数。
- 不凭defconfig一行判定功能已启用，查生成配置和构建接入。
- 不凭同名回调复制释放/join，查实际upper-half是否持锁及调用上下文。
- 私有池、系统堆、DMA缓冲和跨核共享区分别确认，不跨域释放或传裸指针。
- 内容相同、差分存在和源文件由本仓维护都不等于原创归属；使用实际来源与可追差分。
- 哈希/构建/模拟通过分别报告，不推定实机、耗流、长稳或像素显示。
- 不自动烧录、整片擦除、发布、上传日志或启动收费请求；取得对应授权才执行。

## 输出合同

```text
Target       板型、CPU域、配置、版本、输入缺口
Scope        修改/审查层级及代码位置
Contract     owner、上下文、锁、错误、取消和恢复
Change       基础复用、具体适配与新增行为
Evidence     命令、源身份、结果及覆盖范围
Next         仍需的最小动作和回退/停止条件
```

结论应让另一位开发者能继续本次任务，不需要阅读原开发对话或使用原作者机器目录。
