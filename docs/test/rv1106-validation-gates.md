# RV1106 验证闸门

本文列出进入真实功能开发前必须完成的验证。Host/交叉构建项只有在记录环境或工具链、
日期、命令和输出后才可标为通过；板端/HIL 项还必须明确板卡和镜像，否则均为“未验证”。

当前分项证据和阻断见 [2026-07-25 P0 可行性报告](p0-feasibility-report-20260725.md)；
2026-07-27 的 Rockit/ALSA/3A/DTB 只读盘点见
[vendor 音频证据基线](p0-vendor-audio-inventory-20260727.md)；
Rockchip 3A 的 tests-off 交叉链接与符号证据见
[3A 交叉链接验证记录](p0-rockchip-3a-link-validation-20260727.md)；
Rockchip MPI raw AI/AO 的 2026-07-27 tests-off 交叉链接、当时 21 个符号和板端只读存在性见
[MPI 音频交叉链接验证记录](p0-rockchip-mpi-link-validation-20260727.md)；
当前 22 个符号、raw HIL 离线闸门和真实交叉构建见
[2026-07-28 MPI HIL 构建验证记录](p0-rockchip-mpi-hil-build-validation-20260728.md)；
当前镜像的 MPI owner、init/stop 风险与只读执行阻断见
[2026-07-28 MPI HIL 只读前置验证记录](p0-rockchip-mpi-audio-preflight-20260728.md)；
P2f-c-a 的 host/null/交叉链接证据见
[2026-07-27 ALSA playback adapter 验证记录](p2f-c-a-validation-20260727.md)；
三秒单麦手动单轮的真实 ALSA/TLS/WSS 闭环见
[2026-07-28 RV1106 手动单轮 HIL 记录](p1-rv1106-manual-single-turn-hil-validation-20260728.md)；
当前 direct ALSA 48 kHz 全双工 transport、旧镜像差异与临时双 ADC 对照见
[2026-07-28 直接 ALSA 全双工验证记录](p0-alsa-full-duplex-validation-20260728.md)。
2026-08-01 的 Mode1 四通道相关性、2 mic + 2 ref direct 3A、启动/退出和仍未关闭的真人声学
边界见 [P0 Mode1 硬件播放参考验证记录](p0-mode1-hard-reference-validation-20260801.md)。下方带
2026-07-27/28/29 日期的条目保留当时证据；2026-08-01 当前结果只关闭其明确覆盖的后续闸门。

## P0 环境与 ABI

- [x] 已记录 ARMv7-A/NEON/VFPv4、hard-float、`/lib/ld-uClibc.so.0`、uClibc、
  libstdc++/GLIBCXX 边界和 Linux 5.10.160；见上述手动单轮 HIL 记录及 P0 可行性报告。
- [x] 匹配 GCC 8.3/uClibc sysroot 的交叉构建、ELF 检查和真板最小 C++ client 均已通过；
  2026-07-28 手动单轮产物按 SHA-256 部署到 `/tmp` 并返回 0，未覆盖持久程序。
- [x] 当前手动单轮实际使用 ALSA、TLS、pthread、poll/clock/socket 等路径并在目标镜像返回
  成功；该项只关闭当前最小 client 的运行时闭包，不代表 rk_mpi/3A/Snowboy 运行库已验证。
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
- [x] 2026-07-27 Rockchip MPI 音频的 tests-off 默认 ALL link-check 已用匹配
  GCC 8.3/uClibc 工具链完成真实交叉链接；当时最终 ELF 保留 Rockit/MPP/RGA `NEEDED` 与
  21 个 SYS/MB/AI/AO 生命周期 `UND`，没有 `RPATH`/`RUNPATH`。该 target 不安装、不自动执行；MPI 与 3A
  两套 pinned inputs 同时启用的默认 ALL 构建也通过，但均不代表板端加载或音频功能通过。
- [x] 当前 raw MPI HIL 开关默认 `OFF`，显式 target 为 `EXCLUDE_FROM_ALL`，不进入默认
  build、install、CTest 或启动链；离线 CMake/CLI 闸门已通过。link-check 与 HIL 在精确加入
  `RK_MPI_MB_GetSize` 后均以 22 个 Rockchip MPI `UND` 完成匹配 GCC 8.3/uClibc 的真实
  交叉构建，且无 `RPATH`/`RUNPATH`。两个 ARM ELF 均未在板端运行，因此下一项 raw
  全双工功能闸门仍不勾选。
- [x] 当前镜像已运行固定用途只读 preflight：`/proc` FD 扫描完整，PCM owner 为 0，但
  `rkipc` 持有 22 个 `/dev/mpi/*` FD；`dmesg` 可读但没有 follow，`/dev/kmsg` stream
  语义未验证，target 也没有 `timeout`。C++ HIL 已同步扩展为在首次 MPI 调用前拦截配置的
  PCM 与全部 `/dev/mpi/*` owner。preflight 明确返回 `safe_to_execute=false`，没有运行 ARM
  ELF、停服务、发 signal、访问 mixer 或写板端文件。
- [ ] 建立当前镜像专用、可验证且保留 Ethernet/DHCP/SSH 的 maintenance boot，在 rkipc
  首次启动前跳过它并证明不会 respawn。禁止调用会 `killall rkipc/udhcpc`、无界等待并执行
  全 OEM `rcK` 的 `S21appinit stop`；启动流程或镜像变更必须先取得用户本次明确授权。
- [x] 当前板的 direct ALSA 已以实际 48 kHz/S16_LE/RW_INTERLEAVED/2ch、480-frame
  period、1920-frame buffer 完成 6 秒 capture 与 4 秒数字静音 playback；两轮重叠分别
  3950/3940 ms；capture 长度精确，playback 输入文件长度精确且 `aplay` 返回 0，dmesg
  delta clean，mixer 恢复且测试后 PCM closed。
  当前板仍是旧 `RV1106-Atguigu`/`SingadcL` 镜像，所以目标自定义 BSP 必须复测。
- [ ] raw rk_mpi 仍须独立验证同率 2ch 真全双工、有限 timeout、重叠、退出和 kernel log；
  当前被 `rkipc` 的 22 个 `/dev/mpi/*` FD 阻断，direct ALSA 不能替代该项。
- [x] 已实现默认 dry-run、三重显式 opt-in 的 direct ALSA 有界 HIL 工具和离线 fake 回归；
  它请求 480-frame period/4 periods，保存实际 ALSA verbose 输出，以单调时钟验证重叠，
  并在可达退出路径尝试恢复、回读单个 DAC enum，恢复失败独立报错。该工具已按上一项在
  真板执行；手动串行单轮不是其通过依据。
  操作契约见 [直接 ALSA 全双工 HIL 指南](p0-alsa-full-duplex-hil-guide.md)。
- [x] 生产后端保存、设置、回读并在正常退出/半初始化失败时恢复
  `I2STDM Digital Loopback Mode`；本轮启动冒烟在 SIGTERM 后自动恢复为 `Disabled`。
  其他未来新增 mixer 仍须逐个保存/回读/恢复。
- [x] 能力查询和正交低幅信号已确认当前板 Mode1 四通道为
  `[mic0,mic1,refL,refR]`；997 Hz→`refL`、1499 Hz→`refR` 相关系数均为 `0.9983`，
  全双工窗口无 xrun。物理扬声器主要使用 DAC-L，声学到达约 `14–17 ms`（阈值法近似）。
- [x] 直接 Rockchip 3A 的 tests-off 默认 ALL link-check 已用匹配 GCC 8.3/uClibc 工具链
  完成真实交叉链接；最终 ELF 保留 AEC/common `NEEDED` 与 init/short/destory 三个 `UND`。
  该 target 不安装、不自动执行，不能作为板端加载或 3A 功能证据。
- [x] 显式 `EXCLUDE_FROM_ALL` 的固定帧 3A HIL 已完成 Linux fake 6/6 与匹配工具链严格
  交叉构建；固定调用为 `init(16000,16,2,1)` 和一帧 `input_size=768 shorts`，成功结果
  约束为 512 bytes。它未复制或运行到板端，不关闭物理布局、算法效果或实时率；证据见
  [2026-07-29 构建验证](p0-rockchip-3a-hil-build-validation-20260729.md)。
- [x] 当前板已验证 16 kHz/S16、2 mic + 2 ref、256-sample 的 direct 3A 固定帧调用：
  `init(16000,16,2,2)`，`input_size=1024 shorts`，成功返回 512 bytes，guard 完整；
  init/process 为 `11262 us`/`1561 us`。错误恢复、CPU/RSS、最坏耗时和持续实时率仍未关闭。

- [x] Host fake 已覆盖 playback adapter 的 mono/stereo、partial、typed errno、写前/写后
  status、PREPARED 后正写、malformed-positive 和 `Drop`/`Prepare`；Linux ALSA `null`
  accepted-only smoke 已通过。两者都不是物理硬件或声学证据。
- [x] 匹配 BSP 的 GCC 8.3/uClibc sysroot 已用 ALSA 1.2.8 头/库构建真实 playback
  device adapter，并链接默认 ALL 的 adapter/ALSA/clock 符号检查 executable；该结果不代表
  `boompi-client` composition root 已实例化它，也不代表 executable 已在板端运行。
- [x] 已记录实际使用的 `hw:0,0` capture/playback，以及程序精确设置并显式回读的
  48 kHz、2ch、960-frame period、3840-frame buffer；S16_LE/RW_INTERLEAVED 已精确设置。
- [ ] 枚举硬件支持格式全集、解析后的底层硬件路径和全部相关 mixer 控件。
- [x] 已在 direct `hw:0,0` 同时运行 48 kHz capture/playback；最小公共打开区间 3940 ms，
  不是串行录放。该项只关闭当前镜像的 ALSA transport，不关闭 raw MPI、正确 BSP、声学播放
  或双麦/reference layout。
- [x] 2026-07-28 已在 `hw:0,0` 以精确 48 kHz/S16_LE/2ch、960-frame period、
  3840-frame buffer 完成三秒单麦手动单轮：150 个 16 kHz 上行帧、96,000 字节、一次
  commit，随后接收 25 个 24 kHz 下行帧并完成 26 个 renderer chunk 的 ALSA playback
  写入与 drain；最后一帧后先用 `snd_pcm_drop()` 停止 capture，再 commit 和 playback。
  客户端和 fake 服务端均通过，测试后 PCM closed。该串行单轮不勾选上一项全双工，也不证明
  slot 0 非静音/信号质量、双麦或 AEC。
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
- [x] 当前板已有可靠 Mode1 硬件 reference；生产实现不启用软件 reference，也不保留旧
  reference ring/60 ms lead。硬件与软件 reference 仍禁止叠加。
- [x] 软件 reference 评审项当前不适用；若未来 BSP 失去硬件参考，必须重新开启方案评审，
  不得直接恢复旧实现或使用原始 TTS/补零前缀。
- [ ] Rockchip 3A 的现行 16 kHz/2 mic + refL 布局和一次调用已验证；错误恢复、单帧最坏耗时、
  CPU/RSS 和持续实时率仍待关闭。
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
