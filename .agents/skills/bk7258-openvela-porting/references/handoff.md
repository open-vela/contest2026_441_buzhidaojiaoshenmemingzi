# 构建和导出交付

## 原生构建

使用目标项目实际构建入口与配置；不能因文件名类似就套用别板命令。BK7258项目已有bk7258.py时先读该版本--help，验证layer/layout、成对配置、依赖、ELF和编码镜像。

源身份=基线+真实dirty差分+固定依赖/生成配置；顶层HEAD不足以说明外部apps或SDK状态。源码重现、构建成功和二进制逐字节相同分别检查。

## 扁平CP/AP导出检查器

```bash
python3 scripts/inspect_handoff.py /path/to/export --board <physical-board>
```

脚本路径相对本Skill。输入需符合bk7258.build-manifest/3，包含build-manifest.json、source-record.json及boot/CP/AP/pair镜像和CP/AP配置/ELF；预期板型必须显式给出。可选selector指向使用者自己的选择器，不能使用示例机器路径。

检查源码记录与manifest一致、各资产哈希/长度以及该格式的CP/AP角色约束。返回0表示支持范围内一致，2表示具体不匹配。它不解析全部ELF、重算全部分区编码、验证签名或访问硬件。

不支持的格式先输出schema缺口，回到原生工具/人工映射，不通过改manifest伪装支持。导出时烧录前状态保留，后续硬件结果以相同源/镜像身份另行关联。
