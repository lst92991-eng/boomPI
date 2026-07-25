# 音频运行时边界

## 当前状态

本文记录 P2 音频运行时的已实现基础契约。当前只完成 host 可验证的固定帧、
预分配 SPSC 队列、生产 sequence 和消费连续性门禁；ALSA、声道映射、重采样、
Rockchip 3A、Snowboy、播放和打断尚未接入。Host 测试和交叉编译不能替代真机
全双工、Mode1、AEC 或声学验收。

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
丢帧传播、sequence 回绕、fault 锁存和 100,000 帧双线程 FIFO。ASan/UBSan 用于
边界和生命周期检查，ThreadSanitizer 用于 SPSC race 检查；RV1106 preset 只证明目标工具链可编译。真实 ALSA/DSP 行为仍按
[RV1106 验证闸门](../test/rv1106-validation-gates.md)执行。
