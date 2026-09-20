# BK7258 R1 CP/AP 配置

本仓只维护一对比赛终版配置：

| 角色 | 配置目录 | 作用 |
| --- | --- | --- |
| CP | `aidk_ai_toy/configs/team441_cp` | 物理 CPU0 NuttX、NSH、AP 管理、RPTUN、Wi-Fi VNET 诊断 |
| AP | `aidk_ai_toy/configs/team441_ap` | 物理 CPU1/CPU2 SMP NuttX、RPTUN 与 R1 外设 |

`aidk_ai_toy/openvela.conf` 将这对配置和固定分区表交给统一构建入口：

```bash
python3 tools/bk7258/bk7258.py build \
  --board aidk_ai_toy --boot direct --jobs 8
```

`profile.conf` 只提供构建工具需要的 board、role、SDK 和兼容组元数据；NuttX
最终使用的是 Kconfig 生成的 `.config`。因此每次构建后必须核对生成 `.config`
中的关键选项，而不能把 defconfig 中出现一行当作配置已经生效。

终版仓库只保留上述 CP/AP 配置。新增配置时应同步更新构建说明和实机证据。
