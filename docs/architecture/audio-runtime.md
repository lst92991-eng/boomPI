# 音频运行时边界

## 当前状态

本文记录 P2 音频运行时的已实现基础契约。当前完成了 host 可验证的固定帧、
预分配 SPSC 队列、生产 sequence、消费连续性门禁、四通道解交织/极性映射、
四路 48→16 kHz FIR、generation-safe 采集前端编排，以及播放固定帧、代际门禁、
epoch fence 和有界软件队列清理。本阶段还建立了 DSP/唤醒核心接口、失败关闭实现、
测试专用 fake 和默认关闭的 vendor 依赖闸门。P2f-a 还完成了纯软件的 24→48 kHz
播放渲染：wide-int32 FIR、音量/扬声器数字增益、duck、带 release 的块峰值 limiter
和最终 S16 输出；P2f-b-a 完成了 portable sink、64 槽 accepted-prefix ledger、
partial exactly-once committer 和本地 `Drop -> Prepare` cancel ACK 状态机。P2f-b-b1
完成了 host 可验证的固定容量 SPSC 控制/结果通道、独立 urgent cancel、producer
start/stop 与 DSP reset 契约、分槽 EOS/critical 事件，以及 actor-owned exact
cancellation join。P2f-b-b2 再以不创建线程的 deterministic host worker core 接通
renderer、committer、mailbox 和有界单步状态推进。实际播放线程/调度、network
producer/DSP 执行端、ALSA adapter、
normal-EOS presentation completion、Rockchip 3A/Snowboy adapter、wake/VAD worker、
AEC reference 消费和打断闭环尚未接入。accepted（包括 EOS accepted）不等于
presented、played 或 audible。
Host 测试和交叉编译不能替代真机全双工、Mode1、AEC 或声学验收。

## 帧契约

- `CaptureFrame` 固定为 48 kHz、20 ms、S16_LE，每通道 960 samples，允许
  已配置的 2 或 4 通道采集。
- `Mono16kFrame` 固定为 16 kHz、20 ms、S16_LE、mono、320 samples。
- 活跃帧必须携带非零 `epoch` 和 `stream_id`；常开采集允许 `turn_id=0`。
- `sequence` 属于一个 `(epoch, stream_id)`。回绕到 0 必须在新帧上显式标记
  `discontinuity`，由 consumer 锁存故障并交 application actor 取消或换代。
- 帧槽不会在复用时清零整块 PCM。生产者必须完整覆盖声明的样本范围后才设置
  长度并发布；ALSA partial period、xrun 或短读必须取消 lease、计入丢帧并走
  预分配 discard buffer，不能发布上一帧残留样本。

## 声道映射与 48→16 kHz FIR

- `ChannelMap` 按 `mic_left`、`mic_right`、`reference_left`、
  `reference_right` 的逻辑顺序配置物理 slot 和 `+1/-1` 极性。创建时拒绝越界、
  重复 slot 和非法极性；失败不会覆盖已经生效的 mapper。
- mapper 只接受完整的 48 kHz、20 ms、四通道 `CaptureFrame`，输出四个独立平面。
  对 `-32768` 反相时饱和为 `32767` 并计数。实际 Mode1 slot 和麦克风极性仍由
  板端 HIL 决定，代码不把任何尚未实测的声道顺序写死。
- `FirDecimator48To16` 对四个平面使用相同的 211-tap、Q15、线性相位 Kaiser FIR，
  固定 phase 0 抽取，单帧从 `4 × 960` 产生 `4 × 320` samples，并为每路保存
  210 个跨帧历史样本。群延迟为 105 个 48 kHz 输入样本，即 35 个 16 kHz 输出
  样本（2.1875 ms）。
- 设计截止频率为 7.5 kHz；生成器约束 0–7 kHz 通带纹波不超过 0.01 dB、8–24 kHz
  阻带衰减至少 60 dB。当前量化表经离线频响扫描得到纹波约 0.006774 dB、阻带
  峰值约 -64.160 dB，系数和严格为 32768。
- 热路径不分配内存；卷积使用 64-bit 累加、正负对称的 half-away-from-zero 舍入
  和 S16 饱和计数。同一 discontinuity 或新 generation 后，调用方必须先
  `Reset()`，再处理新流，避免把上一代历史带入下一代。
- mapper 和 decimator 实例都由一个 DSP worker 独占。映射只在 worker 启动前或
  停止后创建；actor 发出的 reset/换代请求必须通过有界控制队列交给 DSP worker
  串行执行，不能跨线程直接调用 `Create`、`Reset` 或 `Process`，也不在热路径加锁。
- 这两个原语只提供确定性的输入整形，不等同于 AEC、波束形成、VAD 或已接通的
  实时音频链路。

## 采集 DSP 前端编排

`CaptureDspFrontend` 是 DSP worker 独占的纯同步编排对象，不创建线程，也不执行
ALSA、文件、网络或日志 I/O。固定处理顺序为：

```text
CaptureFrame
  -> FrameContinuityGate
  -> CaptureChannelMapper
  -> FirDecimator48To16
  -> DspCaptureFrame16k
```

- `Create` 只在 worker 启动前配置一次逻辑声道；`Arm` 选择新的非零
  `(epoch, stream_id)` 并清除 FIR 历史，`Disarm` 停用 generation 并清除历史。
  同 generation 不能重复 `Arm` 来无痕恢复 fault。
- application actor 必须在进程生命周期内严格递增 `epoch`，且不复用更早的
  `(epoch, stream_id)`。有限状态 gate 只保存当前/最近退役 identity；epoch 回绕前
  必须停止 producer、排空队列并重建 frontend，不能走 A→B→A 复用。
- `DspCaptureFrame16k` 固定携带 `MIC-L`、`MIC-R`、`REF-L`、`REF-R` 四个
  16 kHz/20 ms 平面，metadata 原样继承输入。它只是未来 3A adapter 的稳定输入，
  不是 AEC 后 mono 输出。
- 空输出指针在 gate 前拒绝，同一输入帧可以重试。错误 epoch/stream 的排队旧帧
  只被丢弃，不推进当前 generation，也不改变 FIR 状态或调用方输出。
- 当前 generation 的 discontinuity、跳号、重复、无标记回绕或时间戳回退会清除
  FIR 历史并锁存 continuity fault；当前 generation 的非法四通道帧会锁存
  transform fault。两类 fault 都不产生输出，且只能用新 generation 恢复。
- mapper/FIR 饱和属于可诊断的幅度事件，不中断合法帧；POD 结果分别返回两类计数。
  热路径只使用对象内预分配 scratch 和调用方输出，不分配、不加锁、不保存指针。
- worker 只把首次锁存 fault 通过有界控制通道报告给 application actor；本对象不
  直接调用 mutex/deque `EventBus`。真实实时率和 Mode1 行为仍必须 HIL。

## DSP 与唤醒后端契约

`AudioDspEngine` 固定消费 `DspCaptureFrame16k` 的 `MIC-L`、`MIC-R`、`REF-L`、
`REF-R` 四平面，成功时输出一个 16 kHz/20 ms mono `Mono16kFrame`。配置必须显式
选择 hardware capture 或 software playback reference、mono-left/mono-right/stereo
layout，并请求完整的 v1 AEC/NS/BF/AGC feature mask。这个 feature mask 是外层契约，
不代表 Rockchip 内部算法顺序或组合能力已经验证。

engine 由 DSP worker 独占；Configure/Arm/Disarm/Process 与销毁均在该 worker 串行
执行。非成功结果使输出 header 无效；错误 generation、discontinuity 和 backend fault
都不能继续产生伪连续 mono。默认 `UnavailableAudioDspEngine` 明确返回不支持，绝不
把某一路原始麦克风复制成“处理后”输出。`FakeAudioDspEngine` 只用于 host 测试编排，
不实现任何音频算法。

`WakeWordEngine` 固定消费完整的 16 kHz/20 ms mono frame，并返回无字符串 POD
decision/error。可用 score 的规范范围是 0–1000；Snowboy 没有置信度输出，因此未来
adapter 必须返回 `score_available=false`、`score_milli=0`，不能用 sensitivity 代替。
Snowboy 的 `-2` 只表示 detector 的 silence 分类，不是产品 VAD。

generation/continuity gate、500 ms AEC 后 pre-roll 和产品 VAD 属于尚未实现的单线程
wake/VAD worker。默认 `UnavailableWakeWordEngine` 永不报告 detection；测试 fake
也不会进入发布 target。Rockchip 的 `input_size` 单位、packing、返回码、算法组合和
reset，以及 Snowboy 模型加载与实时率仍未验证，不得从接口名称猜实现。详细边界见
[音频后端契约与依赖闸门](audio-backends.md)。

两个 vendor 开关默认 `OFF`。当前 pins 只允许在核对 Linux/ARM cross target、固定
RV1106 GNU compiler 与 uClibc sysroot 后，用显式 feasibility opt-in 的 Debug-only
probe 校验绝对路径和 SHA-256；Release 配置拒绝这些候选。通过 configure 只创建
imported targets，不等于 adapter 已实现或 HIL 通过。vendor 库、模型和资源继续保留
在仓库外。

## 播放软件渲染、代际与软件缓冲

- `TtsPcmFrame24k` 固定为 24 kHz、20 ms、S16_LE、mono，正常帧携带 480 samples。
  provider 的最后一帧可以是 `1..479` samples，但必须同时标记 EOS，并把未使用尾部
  清零；这样复用的队列槽不会把旧 PCM 当成新回答播放。活跃 TTS 帧的 `epoch`、
  `turn_id` 和 `stream_id` 均非零。
- `PlaybackResampler24To48` 是 playback worker 独占的固定 2 倍插值器。它使用
  65-tap、Q31、线性相位 half-band Kaiser FIR，20 ms 完整输入从 480 个 24 kHz
  samples 产生 960 个 48 kHz samples；跨帧保留 31 个源样本历史，群延迟为 32 个
  48 kHz 输出样本（约 0.667 ms）。卷积使用 64-bit 累加和正负对称的
  half-away-from-zero 舍入，热路径不分配、不加锁、不做 I/O 或浮点计算。
- FIR 输出先保存在 `ResampledPcmFrame48k` 的 signed int32 wide 域。合法带限瞬态可以
  超过 S16，因此重采样阶段不提前裁剪；`PlaybackGainLimiter` 处理完全部数字增益后才
  执行整条渲染链唯一一次 S16 转换。可复现生成器约束 0–10 kHz 通带纹波不超过
  0.005 dB、14–24 kHz 阻带衰减至少 76 dB；当前 Q31 表的离线扫描结果为约
  0.001664 dB 通带纹波、-78.020480 dB 阻带峰值，10 kHz 增益约 +0.001068 dB，
  14 kHz image edge 约 -78.206567 dB。
- EOS 是严格的两调用事务。对含 `N` 个有效源样本的最终输入，`Process` 先输出
  `2N` 个 prefix samples，强制 `end_of_stream=false` 并返回 `drain_required=true`；
  随后的 `Drain` 再输出完整卷积后缀的 63 个 samples，设置
  `end_of_stream=true` 并自动退役 generation。第 63 个 drain sample 是对应
  `h[64]=0` 的显式终止零；最多前 62 个尾部位置非零。drain pending 期间拒绝新的
  `Process`、`Arm` 和 `SetDucked`，显式 `Disarm` 可以取消未提交的尾部。
- prefix 和 drain 继承同一份 metadata，包括相同的 sequence 与 timestamp，并用
  `source_offset_sample_frames` 分别标记 0 和 `2N`。因此下游顺序键必须包含 offset；
  不能把这两个 chunk 直接送入只按 metadata 连续性判断的普通 `FrameContinuityGate`。
- `PlaybackGainLimiter` 的顺序固定为 `volume × speaker_gain`、逐样本 duck/recovery
  envelope、块峰值 limiter、最终 S16。默认 volume 为 60%、speaker gain 为 100%、
  duck target 为 25%、duck/recovery ramp 均为 80 ms、limiter ceiling 为 95%、
  limiter release 为 80 ms。volume 可配置为 0–100%，speaker gain 可配置为
  0–400%；100% 音量合法，但最大音量和高于 100% 的额外数字增益尚未经过最终壳体
  与板端 HIL/声学安全验收，且更高增益可能因 limiter 动作而降低动态范围。
- duck/recovery 以 48 kHz Q16 增益逐样本变化并跨 chunk 连续；重复设置相同 target
  不重启 ramp。limiter 先查看当前 chunk 的 wide peak，不增加额外帧 lookahead；
  若需要更强衰减，会在该 chunk 首样本前立即 attack。release 按样本跨 chunk
  单调恢复，若当前 chunk 的峰值上限更低则暂停恢复而不暗中消耗 release 时间；新的
  更高峰值会在 chunk 边界再次立即 attack。结果同时报告 limiter 前超过 S16 的样本数、
  实际受 limiter 衰减的样本数和最终防御性 clamp 数；正常整数路径最后一项应为 0。
- `PlaybackRenderer24To48` 是上述两级的 playback-worker facade，固定执行
  `TTS 24 kHz S16 -> 65-tap/Q31 2x FIR -> int32 wide -> volume/speaker gain ->`
  `duck -> block-peak limiter -> S16`，统一检查 generation/连续性、管理内部 scratch、
  EOS prefix/drain 和故障换代。它不观察 `PlaybackEpochFence`，也不是 cancel barrier
  或 Arm ACK。P2f-b-b2 的 deterministic host worker core 已在调用 renderer 前观察
  fence、执行失效处理并通过完整 generation gate；renderer 输出再交给 committer，后者
  在每次设备 write 前重新检查。该 core 只提供同步、有界的 `Step`，尚未接入真实播放线程
  与调度。
- `PlaybackPcmFrame48k` 只表示上述 facade 生成的有界 mono 软件 PCM chunk。它不携带
  设备 write 时序，也不证明任何样本已被 sink 接受、到达 Codec/DAC、从扬声器可闻
  播放或已经发布到软件 AEC reference；P2f-a 的 host 结果同样不是板端 HIL 结论。
- P2f-b-a 的 `PcmPlaybackSink` 是单一 playback worker 独占的 portable 边界，提供一次
  非阻塞 interleaved-S16 write 以及 `Drop`/`Prepare` 控制。write 结果明确区分
  accepted、would-block、interrupted、xrun、suspended、device-lost、zero-progress、
  fatal 和 unavailable；只有正向 accepted 结果携带精确接受长度、write 完成时间、
  首样本预计 presentation 时间、写前排队长度和 timing source。`UnavailablePcmPlaybackSink`
  始终失败关闭。control code 的零值是 `kUnset`，因此 value-initialized
  `PcmPlaybackControlResult{}` 必定 invalid 且不能被误判为成功；只有显式
  `kSucceeded` 且 native error 为 0 才成功。这里还没有真实 ALSA adapter；host
  scripted sink 的返回值不是 `snd_pcm_write*`、DMA、Codec 或扬声器证据。
- `AcceptedRenderChunk48k` 表示一次正向 sink write 实际接受的 mono 连续前缀。它保留
  source metadata，并额外携带非零 PCM incarnation、不可复用的 accepted sequence、
  跨正常 prefix 和 EOS FIR drain 的绝对 source offset、接受长度、chunk/EOS 完成标志、
  write 完成时间、预计 presentation 时间、写前排队长度和 timing source。每条记录
  最多 960 samples，未使用尾部必须为零；预计 presentation 时间不是确认播放时间。
  固定 64 槽 `AcceptedRenderQueue` 是 accepted ledger，不是完整
  `RenderReferenceFrame48k`，也没有接到 AEC consumer。
- `PlaybackCommitter` 是不拥有线程、无阻塞、热路径不分配内存的 portable commit core；
  submit、commit、cancel 和 reference-reset 四类公共结果也都以 `kUnset` 为零值，空
  aggregate 不会表示 accepted、acknowledged 或 confirmed。
  `PumpOnce` 最多尝试一次 sink write。sink 报告的任何正 accepted count 都是不可逆
  线性化点，即使 result code、timing 或附加字段自相矛盾，也不能把该范围重放。
  required-software-reference 模式会在 write 前预留 accepted ledger lease，因此 ledger
  backpressure 导致零 sink write。committer 在 write 前紧邻位置以及 write/publish 后
  重新观察 fence/gate；若 cancel 与通过完整校验的 positive write 竞态，先精确推进
  cursor 并发布事实性的 accepted prefix，再进入 cancellation-required，绝不重放或
  隐去已接受样本。partial write 每次只推进一次，EOS prefix/drain 使用 metadata 加
  source offset 判序。
- malformed positive result 会把报告长度保守截到本次请求范围，推进 cursor、代际累计
  数和 accepted sequence，然后返回 `kAcceptedThenSinkProtocolFault` 并要求取消；它不
  发布 `AcceptedRenderChunk48k`，因为不可信 timing 不能伪造成 reference。这样可能
  丢失软件 reference 覆盖，但不会重放设备可能已接受的 PCM；该代际必须由 cancel 隔离。
- accepted sequence 非零且不得回绕或复用。分配 `UINT64_MAX` 后，后续 submit/write 在
  设备调用前返回 restart-required；actor 仍须推进 fence 并执行 cancel，使旧 PCM 经
  `Drop`/`Prepare` 退役。即使该本地 teardown 成功并 ACK，committer 也进入 terminal
  fault，拒绝 reference-reset confirmation 和新 Arm；只能静止并重启整个运行时/进程，
  不能靠新 generation 或新 committer 假恢复 sequence 空间。
- `RenderReferenceFrame48k` 固定为 48 kHz、20 ms、每通道 960 samples，允许 mono
  或 stereo；内容定义为重采样、音量、duck、混音和 limiter 之后的最终数字 PCM，
  不能用收到的原始 TTS 包代替。metadata 时间戳表示首样本预计到达 DAC 的本地
  单调时刻，必须不早于 render 完成时刻；排队延迟上限为 1.5 秒。当前 committer 不
  生成该完整 reference frame；以后只能从 accepted ledger 组装，不得把 partial prefix
  补零伪装成完整 reference，也不得声称软件 AEC reference 已接通。
- `kEndOfStreamAccepted` 只证明最终 EOS chunk 已被 sink 接受。generation 在该状态仍可
  取消；当前没有基于真实 ALSA status/delay、DMA 或硬件事件的正常 EOS presentation
  completion，也没有安全退役 generation 并通知 actor 的完成事务。EOS accepted 不得
  写成 presented、played、audible 或“播放完成”。
- `PlaybackGenerationGate` 只由 playback worker 操作。激活必须同时携带 actor
  授权的非零 `(epoch, turn_id, stream_id)` 和从 `PlaybackEpochFence` 观察到的相同
  epoch；网络输入不能自行激活代际。活动代际必须先精确退役，迟到的旧 cancel 不能
  退役新代际；身份比较按 epoch、stream、turn 顺序进行，stale 帧不改变活动状态。
- application actor 是 `PlaybackEpochFence` 的唯一写者，playback worker 是唯一
  读者。fence 使用 lock-free 32-bit release/acquire atomic，只允许严格递增的非零
  epoch；回绕前必须静止并重建运行时。它只是低延迟失效信号，不是 wake、Arm/Cancel
  ACK，也不证明 ALSA、DMA、Codec 或扬声器中的旧 PCM 已经停止。
- gate 只保留当前和最近退役的 `(epoch, stream_id)`；actor 必须保证整个 session 内
  不复用历史身份，并在允许网络 producer 发布新代际前等待未来 playback Arm ACK。
  这是明确的控制面责任，不能依赖网络包到达顺序碰巧正确。
- TTS ingress 固定 64 帧，即 1.28 秒；accepted ledger 固定 64 个 accepted chunk，
  每个可以是完整 chunk 或 partial prefix；完整软件 reference 队列固定 16 帧，即 320 ms。
  `DrainPublishedTtsFrames` 只能由 consumer 调用，每次最多丢弃 64 个已发布帧，避免
  持续生产者饿死控制处理。触及上限不代表队列已空，producer 已持有但尚未发布的
  lease 也不可见；它以后即使发布，仍须在 sink commit 前再次通过 generation gate。
- 软件队列 drain 与 ALSA `snd_pcm_drain()` 完全不同。本地 `PlaybackCommitter::Cancel`
  要求 actor 先把 fence 推进到新 epoch，并携带精确 request ID 和旧 generation。它使
  gate 失效、丢弃 committer 内尚未接受的 suffix、执行 sink `Drop`、递增 PCM
  incarnation，再执行 `Prepare`；只有 `Drop` 和 `Prepare` 均成功才返回幂等的本地
  playback cancel ACK。失败或 identity 耗尽进入 sink fault，不得 ACK。该事务本身不
  清理尚未由 worker 接管的 TTS ingress，也不消费已发布或 consumer-held 的 accepted
  ledger 项。
- cancel result 分开携带 `retired_pcm_incarnation` 和 `prepared_pcm_incarnation`。
  前者标识 `Drop` 针对的旧 timeline，并须结合 `drop_succeeded` 判断是否确已退役；
  后者只有新 timeline 的 `Prepare` 成功后才非零。若 Drop 后 incarnation 已耗尽或
  Prepare 失败，prepared 值保持 0，禁止把递增过但未准备好的 identity 写成可用。
- 本地 playback cancel ACK 只证明 committer 已完成上述控制调用，不证明声学静音，
  也不证明旧 reference 已从 DSP/AEC 队列和算法历史中清除。当前 committer 对 software
  和 hardware reference policy 都保守地在每次成功 cancel 后进入
  `kAwaitingReferenceReset`，并要求匹配 request/generation 的 `ConfirmReferenceReset`
  后才允许 Arm 新代际。
- PCM incarnation 已为 `UINT64_MAX` 时，`Drop` 仍可事实性退役旧 timeline，但不能递增
  incarnation 或执行 `Prepare`。worker 将该 Drop-only 事实包装为结构有效、非 ACK 的
  terminal `kRestartRequired` completion；actor cancellation join 必须进入
  `kTerminalRestartRequired`，禁止把它当作普通 barrier 完成或继续 Arm。
- P2f-b-b1 的 `SpscPodQueue` 是单 producer/单 consumer、固定容量、值拷贝的 POD
  mailbox，不提供 lease、覆盖或动态分配。normal lifecycle command/result 分别固定为
  8/16 槽；urgent cancel 和 local cancel completion 各有独立 1 槽通道，因此普通控制或
  结果背压不能延迟、覆盖或隐藏取消。空 aggregate 仍以 `kUnset` 失败关闭。
- actor 到 network producer 的 start permit 与 stop request 分槽。未来 producer endpoint
  每轮必须先处理 Stop，再核对 fence/actor 授权，随后才处理 Start；弹出 Start 不能形成
  持久授权，在启用发布、获取写 lease 以及每次发布数据前，都必须重新确认没有 Stop
  待处理且 generation 仍获授权。producer-stop ACK 只证明旧代际不会再获取或发布数据，
  且没有持有写 lease；它本身不证明 ingress 已空。该 endpoint 尚未实现。
- `PlaybackCancellationJoin` 由 application actor 独占，并按精确 request、generation、
  cancel epoch 和 retired PCM incarnation 汇合 ACK。active cancel 必须依次取得：producer
  stop；本地 `Drop -> Prepare`；producer 已停后、低于有界 drain 上限的稳定 ingress 空
  观察；DSP/reference consumer 释放 lease、丢弃 retired incarnation、确认 local cancel
  后 accepted queue 已空并重置算法历史；最后再由 playback worker 完成
  `ConfirmReferenceReset`。只有全部成立才允许 Arm 下一代际，任一中间 ACK 都不证明
  DAC/扬声器静音。
- never-armed 路径只有在精确证明 renderer 与 committer 均从未 Arm、committer 仍为
  prepared-idle 且该 generation 没有 accepted PCM 时，才可跳过 DSP reset；
  renderer-only 路径还必须证明 cancel fence 之后 renderer 已 disarm、committer 从未
  Arm 且无 accepted PCM。两条 no-sink 路径仍必须等待 producer stop 和稳定 ingress 空，
  不能从空结构体或“看起来空闲”推断完成。
- worker 只为可关联的控制身份发布 mailbox result：普通 lifecycle 除 shutdown 外需要
  非零 request 与完整 generation，shutdown 至少需要非零 request，urgent cancel 需要
  非零 request 与完整 generation。身份完整但 payload 组合非法时返回自身 `valid()==true`
  的 typed error；无法关联的 envelope 不伪造无效 result，由 `Step()` 返回
  `kInvalidControlInput` 交给 runner 诊断。成功 Arm 在 ACK 真正进入结果 mailbox 后才从
  `kArming` 转为 `kActive`，因此 pending ACK 不能被 public state 冒充授权。
- 最终 `QuiesceIngress` ACK 会按精确 request/generation/retired-incarnation 缓存；ACK
  发布后的精确重试原样重放相同计数和 empty proof，不重新 drain、不清零统计，也不改变
  当前或新 generation 的状态。达到 64 帧上限的 `kInProgress` 仍由原事务继续推进，不能
  冒充最终 ACK。
- EOS accepted 事件使用 16 槽普通事件通道，cancellation-required/fault 契约使用独立 1 槽
  critical 通道。EOS 槽满时 worker 必须保留并重试同一事件，且不能先接受新 generation；
  critical 槽满时必须保留精确事件、停止 ingress/render/commit，并在所有非 urgent 工作
  之前重试，禁止覆盖、降级或静默丢弃。urgent cancel 是唯一例外，critical pending 时
  仍必须能执行 teardown。deterministic host worker core 已执行该规则，但它不创建线程；
  当前 core 只在 committer/renderer 要求 teardown 时发布 `kCancellationRequired`。带原生
  errno 的异步 `kFaulted` 事件留给后续 runner/platform adapter；core 自身的纯内部 terminal
  fault 通过 sticky `state()` 与 `Step()` 的 `kFaulted` 暴露，不能伪造 native error。
- TTS 队列满时的集成策略是停止 credit 并取消 response，禁止丢一帧后继续播放；
  `size_approx()` 只供诊断，不能计算 credit。该 actor/network/playback 握手和实际
  network producer、真实线程调度与 credit 握手仍属于后续集成。

## SPSC ownership

每个 `SpscAudioFrameQueue<Frame, Capacity>` 同时拥有固定帧槽和队列位置：

```text
single producer
  -> TryAcquireWrite / ProducerLease
  -> full frame overwrite
  -> Publish
  -> single consumer
  -> TryAcquireRead / ConsumerLease
  -> read-only processing
  -> Release
```

- `Capacity` 必须是 2 的幂，且在构造后不扩容。
- 队列满采用 drop-newest；acquire 立即失败，不阻塞，也不覆盖 consumer 正在读的槽。
- producer/consumer 各自最多持有一个 lease。lease 是 move-only；放弃写 lease 不发布，
  读 lease 析构会归还槽。
- lease 返回的指针或引用只在该 lease 生命周期内有效。`Publish`、`Cancel`、
  `Release`、move-from 或析构后都不得继续访问，也不得传给第三个线程。
- `size_approx()` 只用于脱敏水位诊断，不能作为随后 acquire 的同步前置条件。
- 队列只承载一个 producer 到一个 consumer。AEC 后 mono 音频进入 Snowboy 和网络时，
  复制到两个独立的预分配 SPSC 队列，不用跨 consumer 引用计数。

## 丢帧与连续性

`AudioFrameSequencer` 只属于 producer。调用顺序固定为：先 acquire 可写槽，再完成
整帧采集，最后生成 metadata 并发布。队列满或已取得的 period 作废时，producer
必须调用 `AccountDroppedFrame()`；下一可见帧同时带 sequence gap 和
`discontinuity=true`。xrun 等无法量化丢帧数的情况调用 `MarkDiscontinuity()`。

每个 PCM consumer 拥有独立 `FrameContinuityGate`。`Arm`、`Disarm` 和
`CheckAndAdvance` 全部在该 consumer 线程串行调用；application actor 通过单独的
同步控制消息请求换代，不能跨线程直接操作 gate。错误 epoch/stream 的旧帧被丢弃且
不改变当前状态。显式 discontinuity、跳号、重复、无标记 sequence 回绕或时间戳不
递增都会锁存 fault；同一 generation 不能用重复 `Arm` 无痕清除，只能由 actor 明确
停止旧流并启用新的 epoch/stream。

## 停止顺序

先停止新的 turn 和上游 producer，再让 consumer 排空或丢弃已发布槽，确认所有 lease
已经归还后才能销毁 queue。不得在 producer/consumer 仍运行时清空或析构队列。

## 验证

Host CTest 覆盖固定格式、非法 metadata、FIFO、满队列、槽复用、lease move/RAII、
丢帧传播、sequence 回绕、fault 锁存、100,000 帧双线程 FIFO、声道置换/极性/
饱和、跨帧 FIR、独立参考卷积、频响、舍入、重置、完整采集前端的 generation
切换、旧帧隔离和错误恢复，播放帧、fence、gate、held lease 和有界 drain，以及
DSP/唤醒契约的配置校验、POD 结果一致性、unavailable fail-closed 和测试 fake。
播放侧还覆盖 65-tap Q31 表再生成、跨帧 bit-exact 插值、频响、wide-int32 overshoot、
任意合法 EOS prefix+63 drain、音量/扬声器增益、80 ms duck/recovery、limiter
即时 attack/跨 chunk release、统计量、facade 状态与故障换代。P2f-b-a 进一步覆盖
accepted chunk 的字段/尾部约束和 64 槽 ledger，portable sink 结果与 fail-closed
unavailable/zero-initialized control，实现 partial exactly-once、write 前 backpressure、
fence/write 竞态、malformed positive 不重放/不发布伪 reference、typed fault、EOS
prefix/drain ordering、terminal sequence exhaustion、retired/prepared PCM incarnation，
以及精确 generation 的本地
`Drop -> Prepare -> cancel ACK -> reference-reset confirmation` 状态机。
P2f-b-b1 还覆盖固定 POD mailbox 的失败关闭、满队列/FIFO/回绕和 100,000 条双线程
传递，验证 ordinary backpressure 不阻塞 urgent cancel；并覆盖 producer start/stop、
DSP reset、EOS/critical 分槽契约，以及 active/no-sink cancellation join 的顺序、重复、
身份、epoch/incarnation 和失败路径。P2f-b-b2 的独立 worker 测试覆盖 deterministic
有界推进、分阶段 Arm、ingress→render→commit、EOS prefix/drain、事件保留重试、
critical pending 下 urgent cancel teardown，以及 active/no-sink 取消路径。ASan/UBSan
用于边界和生命周期检查。当前 Linux
ThreadSanitizer 运行时因 `unexpected memory mapping` 未能启动测试，这不是 race 报告，
也不能记作 TSan 通过；RV1106 preset 只证明目标工具链可编译。
这些结果仍只证明 pre-platform software PCM、portable contract、mailbox/join 状态机、
deterministic host worker core 和 scripted sink 行为；真实线程/调度、ALSA、network
producer/DSP endpoint、AEC
reference 消费、normal-EOS presentation completion、DSP/Snowboy 和声学行为仍按
[RV1106 验证闸门](../test/rv1106-validation-gates.md)执行。
