# 顺着一帧声音读代码

## 初始化

App_Init依次打开输入、播放，然后启动输入线程和网络。输入open顺序为ALSA、重采样器、Rockchip 3A、Snowboy、VAD；两路PCM都配置后才首次读取。main的成功和失败出口共用App_Close，先停止线程再释放资源。

## 输入任务的真实调用

在[voice_input.cpp](../../client/src/audio/voice_input.cpp)的CaptureTask中：

```cpp
const int captured = audio_capture::read(raw.data());
// captured < 0报错；0表示断流并重置算法，不能拼接旧前缀。
audio_convert::capture(raw, channels);
rockchip_3a::process(channels, frame.pcm);
const int detected = wake::detect(frame.pcm);
const int voice = vad::process(frame.pcm);
// voice < 0报错，0无声，1人声。
frame.vad_now = voice == 1;
```

这是实际调用摘录；完整代码逐步检查返回值，最后通过四帧有界交接交付给应用。算法之间不排队。

## 一句话怎样形成

在[应用主流程](../../client/src/application/voice_client.cpp)中，仅Listening或Speaking时调用speech::update。进入监听时speech::reset清空历史与计数。

1. 未确认时在500ms环中保留PCM。
2. 连续120ms人声确认，返回start和历史PCM地址，当前帧已包含在其中。
3. 确认后直接交付当前PCM地址；700ms静音或60s媒体上限同时返回end。

应用依次调用voice_net::start(replacing)、send(每帧)、end()。插话时先取消旧播放，触发插话的同一句话直接成为新问题。网络分配轮次号并退休旧回复。发送失败时取消本轮，不再访问这批借用数据。

## 回复和尾播

receive_reply把AUDIO交给playback::write，DONE调用finish；只有status返回Drained才开始3s追问。网络校验轮次与连续性；播放只管理有界采样环、声卡与取消完成屏障。两者都不理解云端模型。

没有静音试探、参考等待、额外电平门限、预热/尾音屏蔽或二次确认。实际AEC、自激与弱声体验待上板调整。硬件48k适配及厂商256点块仍需匹配SDK与真板证据才能进一步删除。见[所有权](../architecture/audio-runtime.md)及[验证记录](../test/audio-unified-refactor.md)。
