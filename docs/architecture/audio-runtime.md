# boomPI 音频运行时

## 数据流

```text
ALSA capture: 48 kHz / S16_LE / Mode1 / 4ch / 20 ms
  -> [mic0,mic1,refL,refR]
  -> 四通道同相位 48 -> 16 kHz
  -> Rockchip 3A input [mic0,mic1,refL]；丢弃 refR
  -> 16 kHz mono AEC output
  -> Snowboy + WebRTC VAD + WSS uplink

WSS TTS: 24 kHz / S16_LE / mono
  -> 180 ms 首播缓存（短 EOS 例外）
  -> 24 -> 48 kHz
  -> 用户音量 × 板级增益 -> limiter
  -> 48 kHz / S16_LE / stereo ALSA playback
  -> Codec Mode1 数字回采到 refL/refR
```

capture 固定协商 `960/1920` frame period/buffer，playback 固定协商 `960/3840`。
一个 PCM frame 是 20 ms；所有 VAD、AEC warm-up、pre-roll 和打断计数都沿用这条时间轴。

## 有界缓冲

| 缓冲 | 容量 | 满载语义 |
| --- | ---: | --- |
| capture handoff | 4 × 20 ms | 清除陈旧帧，下一帧标记 `discontinuity` |
| 唤醒 pre-roll | 25 × 20 ms | 仅保留最新 500 ms |
| barge-in pre-roll | 67 × 20 ms | 仅保留当前打断需要的有界历史 |
| TTS ring | 75 × 20 ms | 取消当前响应，不无限堆积 |
| network event ring | 64 项 | 视为连接失效，不覆盖旧事件 |

TTS 首次累积 9 帧（180 ms）后播放；已经开播后，只有连续 30 ms 未收到 PCM 才进入
rebuffer，至少重新累积 2 帧（40 ms）再恢复。正常结束走 drain；打断或错误走 drop，旧 TTS
不得跨 turn 继续播放。

## 线程与所有权

- capture/DSP 线程使用 `SCHED_FIFO 40`，独占阻塞 capture、四通道重采样、Rockchip 3A、
  Snowboy、WebRTC VAD 和 capture 队列生产端。
- playback 线程使用 `SCHED_FIFO 30`，只消费 TTS ring 并执行重采样、增益、限幅和 ALSA 写入。
- application actor 消费 capture frame，唯一修改会话状态并发起上行、取消和重连。
- WebSocket、UI、网络启动和摄像头线程不处理逐帧 PCM。

如果系统不允许实时调度，客户端记录 warning 并继续运行；这不等价于实时优先级已生效。
板端验收必须读取线程策略，而不能只根据源码常量判断。

## 唤醒与 VAD

- Snowboy 灵敏度默认 `0.7`；命中后重置 listener，最多等待 6 s 开口。
- WebRTC VAD 在 3A 后 16 kHz mono 上判断语音频谱。
- `-30 dBFS` 是 raw 双麦较强一路的“起始准入门”，用于防止远处噪声经算法放大后启动
  utterance；语音开始后只由 WebRTC VAD 延续，避免安静尾音被阈值截断。
- 普通语音连续 120 ms 后开始，连续 700 ms 静音后提交；单轮硬上限 60 s。
- 正常 TTS 结束后进入 3 s 免唤醒追问。

VAD 的 dBFS 门限与播放中打断门限是两个参数，不能互相替代。

## AEC 与打断

TTS mono 被复制成左右声道，因此 Mode1 的 `refL/refR` 高度相关。生产链路保留四通道原始
capture，但送入 Rockchip 3A 前固定丢弃 `refR`，形成双麦、单参考输入。打断判定使用 AEC 后
`voice_dbfs`，默认门限 `-25 dBFS`；raw mic RMS 不能直接证明近讲。

播放开始后，第一次检测到有效硬参考才启动 600 ms AEC warm-up。自然播放结束后隔离 300 ms
声学尾音并重置 VAD。播放中的主动打断按以下顺序进行：

1. 原音量下连续 6 帧（120 ms）满足 AEC 后近讲门限；
2. 一次性静音，不反复改变播放音量；
3. 最多等待 15 帧，取得连续 3 帧低 reference；
4. 再等待 3 帧（60 ms）清除尾音并重置 listener；
5. 连续 3 帧重新确认近讲，随后 drop 剩余 TTS 并发送 cancel；
6. 新 turn 仍需有效 VAD start 和连续 20 帧（400 ms）低 reference。

确认失败则恢复播放并冷却 15 帧（300 ms）。这段静音探针是应用层防自激措施，不能用来
计算 ERLE 或宣称 AEC 声学通过。

## 连续性与故障

- capture xrun、四通道重采样错位或 actor 队列溢出都会产生显式 `discontinuity`，并重置
  3A、Snowboy 和 VAD 历史；序号不能静默连续。
- playback xrun 恢复后只续写尚未接受的样本，不能重放当前 20 ms 块的已接受前缀。
- capture 控制命令只在 frame 边界执行；超时会明确失败，不永久阻塞退出。
- WSS 断线使当前 turn 失败；实时音频不做应用层重传。
- cancel ACK 只关闭旧响应，不能单独启动新 turn；迟到的旧 generation 数据必须丢弃。
- `Close` 先请求线程退出并中断阻塞 PCM，再 join，最后释放 ALSA、DSP、Snowboy、VAD 和 WSS。

## 验证边界

Host harness 可以验证队列、状态迁移、取消乱序、rebuffer 和有界退出；vendor fake 可以验证
`[mic0,mic1,refL]` packing。只有目标板 HIL 才能验证 Mode1 通道顺序、参考相关性、XRUN、
实时优先级和真实播放。AEC 残余回声、自激、double-talk、最终壳体声学和远场指标必须由
同音量的人工/仪器测试关闭。命令见 [Host 与板端验证](../test/host-validation.md)。
