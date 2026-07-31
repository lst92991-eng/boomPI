# 音频运行时边界

## 当前状态

当前产品路径是一个已经在 RV1106 上运行过的纵向闭环，不再围绕预想中的通用
playback/capture 框架组织代码。2026-07-31 已删除未进入最终 ELF 的 control、committer、
worker、通用 DSP frontend、软件 ledger 和旧 ALSA adapter；其历史测试不能代表当前运行时。

现役组件为：

- `AlsaSingleTurnIo`：48 kHz 双声道 capture/playback 句柄和 bounded ALSA I/O。
- `RockchipVoiceDsp`：匹配 BSP 的 16 kHz 固定帧 3A 调用。
- `SnowboyWakeWordEngine` 与 `WebRtcVadGate`：本地唤醒和语音起止。
- `PlaybackRenderer24To48`：24 kHz 下行到 48 kHz 播放帧，并执行 gain/limiter。
- `WssClient`、`ServerControlDecoder`：音频和 turn 控制协议。
- `ManualSingleTurnSession`：当前纵向状态机、队列和线程所有者。

`manual_single_turn` 是历史命名；当前实现已经包含常驻唤醒、追问和打断，但尚未拆成稳定的
产品状态机。后续只有在真实阻塞或所有权问题证明需要时才拆分，不以文件长度为理由增加
空壳 worker。

## 实际数据流

```text
ALSA capture: 48 kHz / S16_LE / 2 ch / 20 ms
  -> capture pump（固定 8 帧，即 160 ms 队列）
  -> 48 -> 16 kHz
  -> Rockchip 3A
  -> Snowboy + VAD
  -> WSS 16 kHz mono uplink

WSS TTS: 24 kHz / S16_LE / mono
  -> 24 -> 48 kHz renderer
  -> gain + limiter
  -> 64 帧有界 jitter queue（首播预缓冲 6 帧，即 120 ms）
  -> ALSA playback
  -> 最终播放 PCM 降采样为 16 kHz 软件 reference
  -> Rockchip 3A + 打断判定
```

当前 capture 只有两个物理输入 slot。双麦左右、极性和软件 reference 的声学对齐以真实板端
结果为准；不能从 `TRCM`、Mode2 示例或变量名推断硬件具备四路数字回采。

## 固定帧与有界缓存

- capture/playback 硬件边界：48 kHz、S16_LE、2 ch、960 frame/period、20 ms。
- 上行、VAD、Snowboy：16 kHz、S16_LE、mono、320 samples/20 ms。
- Qwen 下行：24 kHz、S16_LE、mono；单个协议包不超过 960 bytes。
- capture bridge：8 帧/160 ms。满时不得阻塞 ALSA producer；当前 turn 标记 discontinuity，
  前端 reset 前持续排空旧帧。
- playback jitter queue：64 帧，硬上限 1.28 s；enqueue 最多等待 100 ms。
- playback reference queue：16 帧；开始消费前保留 3 帧/60 ms lead。
- 唤醒与打断 pre-roll：唤醒前 500 ms；已确认打断最多保留 3 s 新用户语音。

所有容量、采样率和超时必须保持显式单位。实时循环中不做文件 I/O；调试录音只能由明确
opt-in 工具产生，不能进入默认路径。

## 线程和所有权

`ManualSingleTurnSession` 独占 turn 状态。网络回调只提交有界数据和控制结果，不直接切换
会话状态。

- capture pump 只负责持续 `Capture20Ms` 和入队；不能被 END PCM、`turn.commit` 或服务端
  等待阻塞。
- voice consumer 在唤醒、普通录音和播放打断阶段接管同一 capture 队列。消费者切换前先
  建立下一接收者；出现队列溢出后，以 sticky discontinuity 阻止旧帧跨代进入 DSP。
- playback consumer 独占 ALSA write、jitter queue 出队和最终软件 reference 生成。
- response watchdog 只负责有界超时；它不能重启服务端或偷偷创建第二条会话。

停止顺序是：停止新 turn/网络生产，停止 capture pump，停止并清空 playback，重置 DSP 和
唤醒历史，最后关闭 ALSA/WSS。不得用固定 `sleep` 猜测线程已经退出。

## 打断与回声门

播放期间 capture 保持运行。最终送入扬声器的 48 kHz PCM 被降采样并延迟对齐后作为软件
reference；收到的原始 TTS 包不能直接作为 reference。

当前轻量门控只做每帧能量、固定一帧 vendor 延迟对齐、600 ms 校准、500 ms echo tail 和
2-of-3 证据；确认窗口为 160 ms。确认后立即 `DropPlayback`、发送 cancel，并把预卷作为新
utterance 开头。它是对 vendor AEC 后残余回声的保护，不是 AEC 替代品。

已知限制：正常 TTS 结束后的 3 s follow-up 仅在前 500 ms 受 echo tail 保护；嘈杂或 AGC
抬噪环境仍可能误触发或持续到 utterance 上限。该问题尚未完成产品验收，不能在文档中写成
已修复。

## 验证边界

Host 测试保留 renderer/resampler/gain、continuity、Snowboy adapter、协议与 WSS 的确定性
检查。Linux `alsa-null-api-flow-only` 只验证现役 `AlsaSingleTurnIo` 的 API 流程；真实
capture、全双工、AEC、打断、扬声器首声和长期稳定性必须在目标板上验收。

历史 `PcmPlaybackSink48k`、`PlaybackCommitter`、`PlaybackWorker` 和通用
`AudioDspEngine` 的验证记录可以保留追溯，但对应代码已删除，不得列入当前测试矩阵。
