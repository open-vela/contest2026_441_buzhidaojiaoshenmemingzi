# BK7258 构建工具

`bk7258.py` 是本仓唯一的 BK7258 构建、检查和打包入口。当前比赛终版只使用：

```bash
python3 tools/bk7258/bk7258.py build \
  --board aidk_ai_toy --boot direct --jobs 8
```

其中 `build.py` 负责 CP/AP 配置和构建，`layout.py` 读取分区，`image.py` 负责
BK7258 32+2 CRC 编码，`package.py`/`trust.py` 保留交付校验能力，`layers.py`
检查 chip、board、app 的依赖方向。目录中其余模块属于参考工具的依赖闭包，
本队没有把它们对应的产品、Gateway、语音配置或 OTA 功能写成终版成果。

构建输出位于完整 openvela 工作区的 `out/bk7258/`。构建完成后必须保存生成的
CP/AP `.config`、ELF、map、bin、构建清单和 SHA256，再由用户决定烧录。工具
不会授权覆盖 RF、MAC、EasyFlash 或用户数据，也不替用户执行烧录。

详细步骤见 `docs/构建与提交指南.md`，外部代码来源见
`SOURCE_PROVENANCE.md`。
