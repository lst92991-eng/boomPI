# boomPI namespace语音架构

应用直接使用语句、播放和网络模块。采集任务显式调用算法模块；语句模块不持有线程，也不转发回复音频。

```text
main → App_Init / App_Process / App_Close
                  ├─ audio_capture::read → speech::update → voice_net::start/send/end
                  ├─ voice_net::poll → playback::begin/write/finish
                  └─ DeviceUi → LVGL / 触摸 / 音量 / 配网 / 摄像头

capture线程: ALSA read → audio_convert → rockchip_3a → wake → vad → 80ms交接队列
playback线程: 1.5s有界队列 → audio_convert → 音量/限幅 → ALSA write → 尾音 → drain
```

VoiceAudio、AudioTasks、AudioPipeline（及历史AudioEngine/AudioBackend）和VoiceLink类均已删除。namespace文件直接持有资源，没有调用旧类的包装，也没有新增运行时管理器。

application独占业务状态、generation和开口/问答超时。Listening表示还没有START，Uploading表示已经开始发送；两者显示同一聆听画面。speech只拥有500ms句首、追问准入与插话探测。播放模块直接校验回复generation/sequence并发布自己的状态。

BPV3把START、PCM、END、CANCEL分开。PCM头为12字节，保留generation和sequence，删除flags和reserved。DONE关闭播放输入，随后排空滤波器与ALSA才发布Drained；因此应用不再另存网络/播放完成布尔组合。纯文本DONE直接进入追问。见[协议](../../protocol/protocol-v3.md)。

启动顺序为audio_capture::open、playback::open、audio_capture::start、voice_net::open：两路PCM配置完成才首次读取，保持原BSP顺序。关闭先停网络，再停播放，最后停采集；线程join后才释放句柄。capture/playback保持SCHED_FIFO 40/30，申请失败仅记录警告。UI与网络不进入实时音频循环。

48kHz四通道Mode1与48kHz双声道声卡配置保留；3A使用实际RKAUDIOParam和rkaudio_preprocess接口，256点适配与Snowboy局部旧ABI保留。当前源码不证明整板16k能力或实际声学效果。服务端仍是只配置DashScope Key的课程配套黑箱。

当前重写的删除清单与验证边界见[交接记录](../test/namespace-rewrite.md)，旧版本设计和测试记录仅作为历史对照。
