# v2 音频运行时

固定链路：ALSA 48 kHz / 4ch → 联合重采样16 kHz → Rockchip双麦单参考3A → Snowboy/VAD → VoiceAudio语义事件 → VoiceApp → VoiceLink。下行24 kHz mono经有界TTS ring、重采样、音量和limiter后输出48 kHz stereo。

帧契约和板级标定在 `board_voice_profile.h`。一个ALSA frame是同一采样时刻的所有通道；960个48 kHz frames才是20 ms。

| 缓冲 | 容量 | 超限行为 |
| --- | --- | --- |
| capture交接 | 4 × 20 ms | 显式discontinuity，不拼接缺帧 |
| 普通pre-roll | 25 × 20 ms | 保留最近500 ms |
| 打断历史 | 32 × 20 ms | 覆盖声学探针与交接，不再等3 s cancel ACK |
| TTS ring | 75 × 20 ms | 取消回答，不覆盖旧PCM |
| VoiceLink上行 | 总计至多800 ms + 有界退休标记 | 明确背压/断线 |
| VoiceLink入站 | 64项 | 溢出连接失败 |

采集和播放线程分别独占各自PCM操作。开始播放先在capture帧边界武装AEC，再由playback线程prepare。退出通过中断read/write/drain、join线程后释放设备。

VoiceAudio负责原有声学策略：VAD起始120 ms、结束700 ms；硬件参考出现后AEC warm-up600 ms，自然结束尾音隔离300 ms；追问准入连续400 ms近讲。播放中保留候选120 ms、临时静音、等待低参考、清尾音、二次近讲确认的探针。假候选恢复音量；确认后立即drop，Barge事件先于已缓存PCM发给应用。

VoiceApp收到Barge即开始新generation，第一帧START|SUPERSEDE让服务端退休旧工作，无cancel ACK等待。短句最后一帧携END，旧代PCM与PlaybackDone都不能结束新回复。触屏单独停止使用STOP，残缺输入不会被伪装成END提交。

音量是UI偏好，声学profile只由维护者整体更新。代码为零音量的确定静音单独处理；极低音量与reference阈值的关系仍需真板验证。有硬件输出却丢失参考仍作为故障线索，不能盲目放开回声准入。

HIL只验证真实链路和观测事件。Host合成帧测试覆盖生产VoiceAudio判定，但不证明AEC消除量、远场识别或最终壳体声学效果。人工项目见 [验证入口](../test/host-validation.md)。
