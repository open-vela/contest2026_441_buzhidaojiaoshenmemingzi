# BK7258 芯片层学习注释镜像

这里保存完整 OpenVela 工作区中 `nuttx/arch/arm/src/bk7258/` 的队伍维护文件。
当前 11 个文件以 `open-vela/nuttx` 候选提交
`8dbe907a8461c3b6b5ceddf3c0fcf7a690df1ffd` 为基线，只增加了帮助理解启动、
中断、串口、定时器和内存边界的中文注释；C 预处理后进行 token 对比，未发现
可执行代码变化。

这个目录不是一份完整的 NuttX，也不能单独编译。真实编译位置仍是工作区的
`nuttx/arch/arm/src/bk7258/`。使用仓库根目录的同步脚本维护两边：

```bash
# 只比较，不改文件
bash scripts/sync-openvela-port.sh --check

# 仓库镜像覆盖到完整 OpenVela 工作区
bash scripts/sync-openvela-port.sh --install

# 在完整工作区修改注释后，采集回本仓库等待 Git 审查
bash scripts/sync-openvela-port.sh --capture
```

以后若开始修改寄存器操作或启动逻辑，必须把“注释镜像”状态改为“功能改动”，
同时记录基线提交、构建结果和 R1 实机证据。公共 NuttX 的正式合并仍应在对应
上游仓库走独立 PR，本目录用于本队学习、复现和评审。
