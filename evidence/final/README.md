# BK7258 R1 作品实机证据索引

r14 是本次技术报告采用的镜像。`vision-badge-r14-build-manifest.json` 记录构建配置、来源树摘要和最终镜像哈希；`vision-badge-r14-SHA256SUMS.txt` 列出 boot、CP、AP、pair 文件哈希。构建日志证明 CP 的 `vision_badge_rpc.c` 在 r14 重新编译并链接。烧录日志于 2026-09-19 19:47:30 记录 `Writing Flash OK`。

- `vision-badge-r14-status.txt`：COM12 返回 `AP endpoint ready`。
- `vision-badge-r14-key1.txt`：KEY1 短按触发，JPEG 21,099 字节，中文结果 81 字节，`stage=done status=0`。
- `vision-badge-r14-query.txt`：NSH 命令触发，JPEG 17,527 字节，中文结果 78 字节，`stage=done status=0`。
- `vision-badge-r13-eye-wifi-boot.log`：r14 未修改 AP 镜像；该 AP 的启动日志记录 Camera 和 SD 数据面通过、原厂样式双屏眼睛任务启动及 Wi-Fi DHCP ACK。硬件点亮照片在 `submission/硬件照片.jpg`；眨眼动作由开发板操作者现场观察确认。
- `vision-badge-r5-apctl.txt`：较早的全 OpenVela BSP 实机自检，记录 AP READY、RPTUN CONNECTED、CPU2 SCHEDULER_ONLINE、AP SMP PASSED 和 online mask `0x3`。

r14 镜像保存在本机 `G:\CIE\R1\firmware\vision-badge-20260919-r14`，仓库以 SHA256 和构建清单指向对应镜像。以下 r9 文件保留为历史视觉链路证据。

This directory contains the non-secret evidence used by the technical report.

- `vision-badge-r9-query-20260919.txt`: COM12 @ 115200 8N1 capture. It records `vision_badge run`, a Chinese MiMo result, `jpeg=20095`, `answer=90`, `stage=done`, and `status=0`.
- `vision-badge-r9-README.txt`: VM build command, image sizes and hashes, and the flash-script reference.
- `vision-badge-r9-SHA256SUMS.txt`: SHA256 values for boot/cp/ap/pair images.
- `vision-badge-r9-build-manifest.json`: board/profile, build provenance, layout and finalized image hashes.

The log and manifest contain no API key or Wi-Fi password. The actual firmware images remain in `G:\CIE\R1\firmware\vision-badge-20260919-r9` and are referenced by hash.
