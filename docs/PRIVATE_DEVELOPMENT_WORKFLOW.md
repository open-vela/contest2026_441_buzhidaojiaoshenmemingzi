# 比赛期间的私密开发与公开提交策略

更新日期：2026-08-27。

## 当前事实

- `open-vela/contest2026_441_buzhidaojiaoshenmemingzi` 当前为 public。
- `xxluestc/contest2026_441_buzhidaojiaoshenmemingzi` 是它的 public fork。
- GitHub 的 fork 可见性跟随 fork network；public 仓的 fork 不能单独改成 private。
- 大赛官方《参赛代码提交指南》写明专属仓“默认 public，有特殊需求可申请改为私仓”。
- 最终参赛代码和主动选定导出的 AI Coding 日志仍需在 9 月 20 日截止前提交到比赛专属仓。私密开发只能延迟公开，不能替代最终交付。

已经推送到 public 仓库的提交和内容，应视为已经公开。之后删除分支、改写历史或删除 fork，都不能保证第三方缓存、clone 和 GitHub fork network 中不再存在。

## 推荐方案

### 方案一：向组委会申请把专属仓改为 private

这是最符合官方规则的方案。联系组委会时一次说明：

1. 队伍编号 441 和专属仓 URL；
2. 申请比赛期间改为 private，截止评审时按组委会要求开放或授权访问；
3. 当前已有一个 public fork，请组委会确认应删除后重建 private fork，还是采用直接协作/其他 PR 流程；
4. 确认 private 状态下 fork、PR、自行 merge、评委访问和截止收权是否保持不变。

不要先假定组委会把上游改私有后，现有 public fork 会自动变私有。GitHub 文档明确说明：public 仓改为 private 时，已有 public forks 会保持 public 并脱离原 network。

### 方案二：在获批前仅做本地开发

当前 Ubuntu 工作区可继续 commit，但不要 push 到 `fork` 或 `openvela`：

```sh
cd /home/alientek/openvela/contest2026_441_buzhidaojiaoshenmemingzi
git status --short --branch
git remote -v
git log --oneline '@{upstream}..HEAD'
```

本地提交不会自动出现在 GitHub。推送前必须单独审核将公开的 commit、diff、文档、日志和大文件。

### 方案三：建立独立 private 备份仓

如果需要异地备份，可在 GitHub 新建一个 private 空仓库，把它作为普通 remote；不要点击 Fork，也不要把它加入 public fork network。这个仓只用于开发备份，不代替大赛提交。

建议 remote 语义：

```text
private   私密开发备份，日常 push 目标
fork      个人 public fork，仅在审核后发布
openvela  组委会专属仓，用于最终 PR
```

建立 private remote 后，可设置默认 push 目标，降低误推风险：

```sh
git remote add private <你的私有仓 URL>
git config remote.pushDefault private
git config branch.dev-ai-contest-2026.pushRemote private
```

在 private 仓创建完并确认访问权限前，不要执行这些配置，也不要把占位 URL 原样使用。

## 哪些内容暂不进入 public fork

- 未完成的产品创意、交互流程和差异化算法；
- 尚未经过构建/实机证据支持的完整移植分支；
- AI Coding 原始日志和阶段性研究笔记；
- API key、Token、Wi-Fi 密码、设备校准数据、MAC/证书私钥；
- 原厂受限 SDK、芯片手册、原理图或不允许再分发的二进制。

密钥永远不应进入 Git 历史，即使目标仓是 private。若密钥曾进入 public 仓，正确处理是立即撤销/轮换，再清理历史；只删除当前文件不够。

## 本地记录放置规则

- 可公开、可复现的移植调研：放 `docs/`，准备最终随作品提交。
- 暂不公开但希望后续继续参考的研究：放 `docs/internal/`，并仅在本机 `.git/info/exclude` 中排除；不要为了隐藏它而修改会被提交的 `.gitignore`。
- 厂商 SDK、手册和 R1 原理图：留在工作区 `beken_reference/` 或 Windows `G:\CIE\R1`，不复制进比赛仓。
- AI Coding 日志：先保留在本机 staging；审核、脱敏后按官方工具导出到 `logs/`，接近提交时再纳入公开/获批后的私有比赛仓。

## 发布门禁

每次准备从本地或 private 仓发布到 public fork 时，依次确认：

1. 明确这次要公开的 commit 范围；
2. 审查 `git diff <public-head>...HEAD`；
3. 扫描 secret、个人信息、原厂受限资料和大文件；
4. 检查外部代码的许可证、版权和来源记录；
5. 运行对应构建/测试并保存证据；
6. 已完成并通过上述检查的内容，可按当前授权直接 push 到个人 fork；向大赛官方仓创建或合并 PR 仍单独执行。

在组委会确认私仓流程前，默认只把已完成、可公开且通过检查的内容推送到个人
`fork`；未完成的创意、阶段性移植、原始 AI 日志和敏感资料继续留在本地。不得把
“可以直接 push”扩大为向 `openvela` 官方仓直接推送或自动创建、合并 PR。
