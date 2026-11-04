# 构建与运行

## 1. 工作区与前置条件

只在Ubuntu工作区交叉编译。队伍仓目录名保持contest2026_441_buzhidaojiaoshenmemingzi，位于工作区下一层；源码依赖、工具链、SDK和Python环境分别准备，不能把源码已拉取当作可构建。

在包含kconfiglib及构建依赖的Python环境中操作；将示例虚拟环境路径替换为实际路径。恢复前先准备依赖和SDK所需的本地或网络来源。已有恢复入口是tools/restore_workspace.py，默认采用版本化config/restored-workspace.lock.json及nuttx/dependencies.lock.json校验依赖身份并建立manifest目录链接。先读脚本--help，选择新的工作区；不在稳定环境强制sync覆盖已有差分。静态XML中的分支并非精确发布锁。

## 固定工具与框架集成

从仓库根检查当前工具帮助，按锁准备工具链及CP/AP SDK profile：

```bash
python3 tools/bk7258/bk7258.py toolchain install --help
python3 tools/bk7258/bk7258.py sdk install --help
python3 tools/bk7258/bk7258.py sdk rebuild --help
```

工具链可用固定归档安装；SDK使用匹配profile的已准备bundle，或在固定SDK源目录重建。安装/重建后分别verify，不能只恢复SDK Git源码就直接当作完整bundle。

apps依赖还包含四项libwebsockets集成差分。恢复固定apps基线后，在尚未应用该差分的干净apps目录执行：

```bash
git -C /path/to/openvela/apps apply --check /path/to/team-repo/evidence/reproducibility/apps-integration.patch
git -C /path/to/openvela/apps apply /path/to/team-repo/evidence/reproducibility/apps-integration.patch
```

若正向check失败，先检查版本和已有修改；可用reverse --check判断是否已经应用，不强行覆盖或重复应用。补丁内容及来源与依赖版本成套保存。

## 2. 当前环境构建

在已准备好的Ubuntu工作区中执行（将/path/to/openvela替换为实际工作区）：

```bash
cd /path/to/openvela
source /path/to/venv/bin/activate
python3 tools/bk7258/bk7258.py verify layers
python3 tools/bk7258/bk7258.py verify layout \
  --partition boards/bk7258/aidk_ai_toy/bk7258_ab_fixed_block_full_release.csv
python3 tools/lvgl_dependency.py --workspace /path/to/openvela
python3 tools/bk7258/bk7258.py build \
  --cp-config boards/bk7258/aidk_ai_toy/configs/team441_cp_power \
  --ap-config boards/bk7258/aidk_ai_toy/configs/team441_ap_product \
  --partition boards/bk7258/aidk_ai_toy/bk7258_ab_fixed_block_full_release.csv \
  --boot direct --jobs 2 --workspace /path/to/openvela
bash tools/agora/run-offline-checks.sh
```

## 3. 构建检查与输出

从当次out目录保存CP/AP的生成.config、ELF、map、编码bin、boot/pair、build-manifest及SHA256。使用本仓工具的manifest验证子命令，具体参数先看对应--help，不绕过布局或依赖检查。

重点核对CP/AP角色、AP SMP_NCPUS=2、RPTUN两侧配置、LOAD段/共享区/分区边界、32+2编码，以及生成配置真实生效。仅在defconfig声明并不证明最终启用。

已有另一workspace新out构建/验证与实际源恢复证据。Boot/CP与演示包相同，隔离AP/pair不同且未上板；演示仍用已验收原包。构建通过不升级硬件验收范围。

## 设备部署与启动

核对生成配置、ELF、编码镜像、布局和SHA256后，由操作者烧录明确的boot/CP/AP/pair区域，保留RF、校准和用户区。未指定写入范围不得整片擦除。

启动后通过uname -a、apctl status及bkwifi status检查系统、跨核和网络状态。AP心跳递增、RPTUN连接及SMP上线分别检查。配置服务端后，开机等待中键或唤醒；中键短按开始/退出对话，左右键调整音量。

服务端安装与配置见[服务端部署](server.md)。镜像版本与已有验证见[交付记录](../evidence/build/identity.json)。
