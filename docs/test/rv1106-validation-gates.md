# RV1106 验证闸门

本文列出进入真实功能开发前必须完成的板端检查。除非测试记录明确给出板卡、镜像、日期和命令输出，否则状态均为“未验证”。

## P0 环境与 ABI

- [ ] 记录 CPU ISA、float ABI、动态加载器、libc、libstdc++ 和内核版本。
- [ ] 确认交叉编译器与目标 sysroot 匹配，并运行最小 C++ smoke executable。
- [ ] 确认 ALSA、TLS、pthread 和所需系统调用在目标镜像可用。
- [ ] 对 Rockchip/Snowboy 动态库执行 `file`、`readelf` 和依赖/符号检查。

## 音频

- [ ] 记录真实 ALSA card/device、支持格式、period/buffer 和 mixer 控件。
- [ ] 验证 48 kHz capture/playback 全双工，不用串行播放与录音冒充。
- [ ] 用可辨识信号确认 Mode1 四通道顺序、双麦极性和数字参考采样位置。
- [ ] Mode1 不成立时停止，不自动启用未经评审的软件 reference。
- [ ] 验证 Rockchip 3A 16 kHz 输入布局、错误码、单帧最坏耗时和持续实时率。
- [ ] 最终壳体中记录距离、角度、音量、背景噪声、ERLE、残余回声和 double-talk 结果。

## Snowboy

- [ ] 确认 runtime/model 来源、SHA-256 和再分发许可。
- [ ] 验证模型初始化、16 kHz mono 连续输入、目标英文唤醒和错误路径。
- [ ] 记录 CPU、RSS、最坏帧耗时、漏唤醒和误唤醒；功能跑通后再进行至少 30 分钟稳定性测试。
- [ ] 不兼容时保存证据并请求架构决策，禁止静默换引擎或自行改为多进程。

## 网络、UI 与系统

- [ ] 以太网和 Wi-Fi 分别验证 DHCP、路由、断线和恢复。
- [ ] 验证 Wi-Fi 芯片/驱动支持目标 AP/STA 配网流程。
- [ ] 验证 UDP 发现、WSS 首配、SPKI 固定、重连和 half-open timeout。
- [ ] 验证 ST7789P3 横屏像素格式/旋转/刷新、GT911 坐标和背光休眠。
- [ ] 验证 BusyBox init 的实际 `inittab` 字段和 supervisor 有序停止/重启。
- [ ] 仅在 `/dev/watchdog`、DTS 和驱动均确认后测试硬件 watchdog。
- [ ] A/B 更新最后验证：签名失败拒绝、掉电安全、候选三次失败回滚和 mark-good。

## 安全边界

只读探测可以直接进行；刷镜像、改设备树、分区、启动项或持久删除数据必须针对该次操作取得用户明确授权。测试日志不得包含 API Key、Wi-Fi 密码、device token、证书私钥或用户录音。
