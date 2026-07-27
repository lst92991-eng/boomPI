# RV1106 验证闸门

本文列出进入真实功能开发前必须完成的验证。Host/交叉构建项只有在记录环境或工具链、
日期、命令和输出后才可标为通过；板端/HIL 项还必须明确板卡和镜像，否则均为“未验证”。

当前分项证据和阻断见 [2026-07-25 P0 可行性报告](p0-feasibility-report-20260725.md)；
2026-07-27 的 Rockit/ALSA/3A/DTB 只读盘点见
[vendor 音频证据基线](p0-vendor-audio-inventory-20260727.md)；
Rockchip 3A 的 tests-off 交叉链接与符号证据见
[3A 交叉链接验证记录](p0-rockchip-3a-link-validation-20260727.md)；
Rockchip MPI raw AI/AO 的 tests-off 交叉链接、21 个符号和板端只读存在性见
[MPI 音频交叉链接验证记录](p0-rockchip-mpi-link-validation-20260727.md)；
P2f-c-a 的 host/null/交叉链接证据见
[2026-07-27 ALSA playback adapter 验证记录](p2f-c-a-validation-20260727.md)。

## P0 环境与 ABI

- [ ] 记录 CPU ISA、float ABI、动态加载器、libc、libstdc++ 和内核版本。
- [ ] 确认交叉编译器与目标 sysroot 匹配，并运行最小 C++ smoke executable。交叉构建与 ELF 检查已通过，真机执行待补。
- [ ] 确认 ALSA、TLS、pthread 和所需系统调用在目标镜像可用。sysroot 链接已通过，当前板端运行待补。
- [x] 对 Rockchip/Snowboy 运行库执行 `file`、`readelf` 和依赖/符号检查。功能与性能仍分别保留在下方闸门。

## 音频

当前执行顺序是 vendor backend 最小闭环优先。以下已完成的 playback host/交叉测试继续
作为证据保留，但其余 playback worker、control 和 runtime 组合项暂停扩展，直到 rk_mpi/
ALSA 的真实全双工、capture layout 和 3A 契约关闭。

- [x] 只读核对 rk_mpi AI/AO 头文件、raw frame 生命周期、`librockit.so` 依赖、预构建
  test、当前 DTB/codec/ALSA 声明和 3A 候选；这些仍不是板端功能验证。
- [x] 已在当前板/镜像运行 schema v2 只读探针：以 host 时间 2026-07-27 21:12:34 +08:00
  记录一个 capture PCM、一个 playback PCM、Rockit、AI/AO test 和直接 3A 库存在，VQE
  JSON 缺失。板端时钟错误地停留在 2021 年；探针未打开 PCM、未执行 vendor，随后物理
  链路断开。存在性只关闭资源盘点，不是全双工或功能证据。
- [x] Rockchip MPI 音频的 tests-off 默认 ALL link-check 已用匹配 GCC 8.3/uClibc 工具链
  完成真实交叉链接；最终 ELF 保留 Rockit/MPP/RGA `NEEDED` 与 21 个 SYS/MB/AI/AO
  生命周期 `UND`，没有 `RPATH`/`RUNPATH`。该 target 不安装、不自动执行；MPI 与 3A
  两套 pinned inputs 同时启用的默认 ALL 构建也通过，但均不代表板端加载或音频功能通过。
- [ ] 用直接 ALSA 与 raw rk_mpi 分别验证 48 kHz S16_LE 2ch 真全双工、有限 timeout、
  明确重叠区间、全部 exit code/录音字节数、停止顺序和 dmesg xrun delta；串行运行不能
  冒充全双工。
- [x] 已实现默认 dry-run、三重显式 opt-in 的 direct ALSA 有界 HIL 工具和离线 fake 回归；
  它请求 480-frame period/4 periods，保存实际 ALSA verbose 输出，以单调时钟验证重叠，
  并在可达退出路径尝试恢复、回读单个 DAC enum，恢复失败独立报错。当前链路断开，尚未在板端
  执行，不能勾选上一项。
  操作契约见 [直接 ALSA 全双工 HIL 指南](p0-alsa-full-duplex-hil-guide.md)。
- [ ] 逐个 `amixer cget` 保存所有将改控件，设置后回读，并在正常/异常退出恢复原值。
  预构建 AI test 会无条件清除 loopback，不能假定原值为 `Disabled`。
- [ ] 通过能力查询后才尝试 4ch；固定 `TRCM clk-trcm=1`，逐次验证 loopback
  `Disabled/Mode1/Mode2/Mode2 Swap`，用正交低幅 L/R 序列确认双麦/reference slot、
  极性、延迟和漂移，再通过幅度/volume/mixer/mute 变化定位 reference tap。
- [x] 直接 Rockchip 3A 的 tests-off 默认 ALL link-check 已用匹配 GCC 8.3/uClibc 工具链
  完成真实交叉链接；最终 ELF 保留 AEC/common `NEEDED` 与 init/short/destory 三个 `UND`。
  该 target 不安装、不自动执行，不能作为板端加载或 3A 功能证据。
- [ ] 在板端验证 16 kHz/S16、2 mic + 1 ref、256-sample 的物理 slot 到交织逻辑输入映射；核对
  `input_size=768 shorts`、成功 512 bytes、非法尺寸 0、init null，并记录错误恢复、
  CPU/RSS、单帧最坏耗时和持续实时率。

- [x] Host fake 已覆盖 playback adapter 的 mono/stereo、partial、typed errno、写前/写后
  status、PREPARED 后正写、malformed-positive 和 `Drop`/`Prepare`；Linux ALSA `null`
  accepted-only smoke 已通过。两者都不是物理硬件或声学证据。
- [x] 匹配 BSP 的 GCC 8.3/uClibc sysroot 已用 ALSA 1.2.8 头/库构建真实 playback
  device adapter，并链接默认 ALL 的 adapter/ALSA/clock 符号检查 executable；该结果不代表
  `boompi-client` composition root 已实例化它，也不代表 executable 已在板端运行。
- [ ] 记录真实 ALSA card/device、支持格式、period/buffer 和 mixer 控件。
- [ ] 验证 48 kHz capture/playback 全双工，不用串行播放与录音冒充。
- [ ] 用真实 ALSA adapter 验证 partial write、would-block/interrupted、xrun/suspend、
  device loss、`drop`/`prepare` 和重新建链；不得重复写出已经 accepted 的 prefix。
- [ ] 对真实设备和解析后的直接硬件路径回读 exact 48 kHz/S16_LE/RW_INTERLEAVED、playback channels、period、
  buffer、start threshold、avail-min 和 MONOTONIC timestamp；写后仍为 PREPARED 时不得
  生成 presentation timing。named PCM 插件边界的 exact 不得冒充底层硬件路径 exact。
- [ ] 对 adapter 故障注入验证 control aggregate `{}` 失败关闭；malformed positive count
  只能保守推进请求范围、禁止重放和伪 timing reference，并立即进入取消路径。
- [ ] 验证 accepted sequence 耗尽会在下一次 write 前拒绝；完成 `drop`/`prepare` 后仍
  保持 terminal restart-required，不能通过换 generation 或重建 committer 假恢复。
- [ ] 接入并验证单播放 worker、network producer endpoint 和 DSP endpoint。host 已有
  固定 SPSC mailbox、cancellation join 和 deterministic 单步 worker core；该 core 不创建
  实际线程，也不代表 ALSA、network 或 DSP 执行端已经运行。
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
  不静默丢弃；critical pending 时 urgent cancel 仍必须能推进 teardown。
- [ ] 用 ALSA status/delay 与可观测硬件证据定义 normal-EOS presentation completion；
  sink accepted 或预计 presentation timestamp 不能直接报告为 played/audible。
- [ ] 区分并记录 DTB TRCM 时钟模式与 `I2STDM Digital Loopback Mode` mixer 设置；二者
  都不得当作 capture slot 证据。
- [ ] 没有可靠硬件 reference 时停止，不自动启用未经评审的软件 reference。
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
