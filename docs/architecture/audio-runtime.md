# 音频运行时边界

## 当前状态

本文记录 P2 音频运行时的已实现基础契约。当前完成了 host 可验证的固定帧、
预分配 SPSC 队列、生产 sequence、消费连续性门禁、四通道解交织/极性映射、
四路 48→16 kHz FIR 和 generation-safe 采集前端编排；ALSA、Rockchip 3A、
Snowboy、播放和打断尚未接入。
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
饱和、跨帧 FIR、独立参考卷积、频响、舍入、重置，以及完整前端的 generation
切换、旧帧隔离和错误恢复。ASan/UBSan 用于边界和生命周期检查，ThreadSanitizer
用于 SPSC race 检查；RV1106 preset 只证明目标工具链可编译。
真实 ALSA/DSP 行为仍按 [RV1106 验证闸门](../test/rv1106-validation-gates.md)执行。
