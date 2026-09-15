# 当前语音数据流与所有权

ALSA只打开、读取、写入和关闭原始PCM。voice_input是独立输入处理任务，不是ALSA驱动：线程中顺序执行原始读取、双麦/refL联合重采样、3A，交付后应用执行wake、逐块VAD、speech策略、网络发送。

```text
输入线程：alsa_audio::read → audio_convert::capture → rockchip_3a::process → 4槽交接
应用线程：voice_input::read → wake::detect → vad::process → speech::update → voice_net
播放线程：有界队列 → audio_convert::playback → ALSA write → 有效尾音 → drain
```

不再有跨采集线程的listener reset/arm命令、应答状态或100ms握手。wake/VAD由应用线程独占，listen直接复位；网络等待不进入输入线程。两路PCM均配置后才start输入，退出中断I/O、join后释放资源。

VAD仅返回-1/0/1。120ms人声确认、700ms句尾、60s语句上限、500ms前滚、400ms追问、AEC预热/尾音和插话探针都在speech。主状态只有Idle、Listening、WaitingReply、Speaking；连接和上传事实查询网络模块已有状态，不另存Offline/Uploading枚举或帧计数。

为防止处理排队音频时读到未来的播放结束，输入线程把当时的播放快照与PCM一起交付。playback::observe仅由输入线程消费，业务使用frame.output。3A仍把PCM与metadata一起延迟，直接写交付帧，删除中转PCM。

Mode1仍读四槽；refR始终未用于3A，现在在软件转换前丢弃，只对双麦/refL共同重采样。原始四槽不被修改。48k声卡与16k算法之间的转换尚不能凭现有资料全部删除，仍待匹配SDK/真板验证。

网络独占接收sequence校验。播放只维护当前/已退休generation，取消不清水位，没有另一份highest_generation或网络序号。DONE只调用finish；取尽有效滤波尾音与ALSA后才Drained。短回答、队列满、prepare/write/drain期间取消和真实Failed锁存均有Host测试。

协议为[BPV4固定文本控制](../../protocol/protocol-v4.md)，板端不再依赖cJSON。服务端直接接管socket消息的PCM，去掉第二份数组；仍保留真正隔开socket读取与云端阻塞调用的有界队列。服务端继续作为Key-only课程黑箱。
