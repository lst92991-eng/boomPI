# RV1106 验证闸门

本文列出进入真实功能开发前必须完成的板端检查。除非测试记录明确给出板卡、镜像、日期和命令输出，否则状态均为“未验证”。

当前分项证据和阻断见 [2026-07-25 P0 可行性报告](p0-feasibility-report-20260725.md)。

## P0 环境与 ABI

- [ ] 记录 CPU ISA、float ABI、动态加载器、libc、libstdc++ 和内核版本。
- [ ] 确认交叉编译器与目标 sysroot 匹配，并运行最小 C++ smoke executable。交叉构建与 ELF 检查已通过，真机执行待补。
- [ ] 确认 ALSA、TLS、pthread 和所需系统调用在目标镜像可用。sysroot 链接已通过，当前板端运行待补。
- [x] 对 Rockchip/Snowboy 运行库执行 `file`、`readelf` 和依赖/符号检查。功能与性能仍分别保留在下方闸门。

## 音频

- [ ] 记录真实 ALSA card/device、支持格式、period/buffer 和 mixer 控件。
- [ ] 验证 48 kHz capture/playback 全双工，不用串行播放与录音冒充。
- [ ] 用真实 ALSA adapter 验证 partial write、would-block/interrupted、xrun/suspend、
  device loss、`drop`/`prepare` 和重新建链；不得重复写出已经 accepted 的 prefix。
- [ ] 对 adapter 故障注入验证 control aggregate `{}` 失败关闭；malformed positive count
  只能保守推进请求范围、禁止重放和伪 timing reference，并立即进入取消路径。
- [ ] 验证 accepted sequence 耗尽会在下一次 write 前拒绝；完成 `drop`/`prepare` 后仍
  保持 terminal restart-required，不能通过换 generation 或重建 committer 假恢复。
- [ ] 接入并验证单播放 worker、network producer endpoint 和 DSP endpoint。host 已有
  固定 SPSC mailbox 与 cancellation join 契约，不代表这些执行端已经运行。
- [ ] 验证 network producer 每轮先处理 Stop，并在 Start 生效、获取 write lease 与每次
  publish 前重验 Stop/fence/actor 授权；producer-stop ACK 后还必须由 playback worker
  取得稳定 ingress 空观察。
- [ ] 验证 cancel fence 到停止旧代际 write、本地 playback ACK、DSP/reference reset ACK
  join 和最终扬声器静音的时序。active cancel 必须汇合 producer stop、本地
  `drop`/`prepare`、稳定 ingress 空、按 retired PCM incarnation 完成的 DSP reset 与 worker
  confirmation；结果必须区分 retired/prepared PCM incarnation，且任何数字 ACK 都不能
  单独充当声学静音证据。
- [ ] 对 never-armed 与 renderer-only/no-sink 取消路径做故障注入，确认仅在严格证明
  committer 从未 Arm、无 accepted PCM，且 renderer-only 已在 cancel fence 后 disarm 时
  才跳过 DSP reset；两条路径仍须等待 producer stop 和稳定 ingress 空。
- [ ] 注满普通 EOS 与 critical 事件通道，确认 EOS 事件被保留并在新 generation 前重试，
  critical 事件被精确保留且 worker 停止 ingress/render/commit、优先重试，不覆盖、不降级、
  不静默丢弃。
- [ ] 用 ALSA status/delay 与可观测硬件证据定义 normal-EOS presentation completion；
  sink accepted 或预计 presentation timestamp 不能直接报告为 played/audible。
- [ ] 用可辨识信号确认 Mode1 四通道顺序、双麦极性和数字参考采样位置。
- [ ] Mode1 不成立时停止，不自动启用未经评审的软件 reference。
- [ ] 若评审后启用软件 reference，验证 accepted-prefix ledger 到 AEC 输入的组装、换代、
  partial/cancel 边界、延迟对齐和旧 reference 清除；不得用原始 TTS 或补零前缀代替。
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
