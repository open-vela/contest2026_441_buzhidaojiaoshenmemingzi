# 开发案例

## 存储退出的只读事务屏障

### 问题

R1采用SD-NAND且不启用ONCHIP_DATAFS。产品关闭媒体后进入电源退出，原存储检查依赖不存在的DATA域，导致屏障准备返回错误。原测试将关键guard模拟为成功，未能覆盖实际板级拓扑。

### 分析与实现

沿存储guard的真实调用链核对owner、嵌套、写域与Flash事务互斥条件。新增QUIESCE只读用途：复用同一事务锁，允许无DATA拓扑等待共享事务边界，不提供写权限；产品按AP排空/sync、CP同步与屏障的顺序退出。

代码入口为`chips/bk7258/cp/bk7258_storage.c`及`app/vision_badge/src/power_service.c`，具体修改关联31732fd。

### 验证与Review要点

测试编入实际storage实现，覆盖无DATA配置、只读屏障、写入拒绝、嵌套、忙与IRQ边界。reset/SDK/Flash边界由测试替身提供，软件返回与设备实际休眠分别核对。Review关注屏障是否扩大写域、锁顺序是否一致，以及每个失败出口能否回收资源。

## 相机闲置停流后的新观察

### 问题

持续会话保留相机owner时，热相机因闲置超时关闭；后续观察仍持有owner，却没有重新打开设备，导致下一次新帧获取失败。

### 分析与实现

将会话owner和底层设备运行状态分开判断。保留原会话所有权，在需要新帧时按生命周期close/reopen，并限制恢复次数。取消和过期请求不能触发旧结果提交，也不能重复创建相机owner。

代码入口为`app/vision_badge/src/camera_service.c`，验证入口为`tools/agora/test_camera.py`。

### 验证与Review要点

实际C worker的适配测试覆盖关闭/重开、owner保留、新帧、取消与单次有界重试，结果为CAMERA_LIVE_TIMEOUT_RECOVERY_PASS。设备I/O使用测试替身；同场两次正常取图的设备记录独立关联。Review关注重试上界、帧归还、取消唤醒以及新旧请求的身份隔离。

## 协作方式

每个问题先明确接口和资源合同，AI提交实现与相关自检，维护者按需求、关键代码和验证结果Review。规范入口为[CONTRIBUTING](../CONTRIBUTING.md)，验证记录见[evidence](../evidence/README.md)。
