# 顺着真实语音主流程阅读

这份说明对应当前namespace实现；旧VoiceAudio/AudioTasks/AudioPipeline已经删除。

## 1. 从麦克风到处理后的帧

入口是 `client/src/audio/audio_capture.cpp` 的 `CaptureTask`。以下是函数中按顺序出现的真实调用（每一步的失败检查见源码）：

```cpp
alsa_audio::read(raw.pcm.data(), &raw.discontinuity);
audio_convert::capture(raw, &channels);
rockchip_3a::process(channels, &clean);
wake::detect(frame.pcm, &frame.wake);
vad::process(&frame, playback::observe());
```

ALSA的frame表示一个时刻的所有通道：当前960×4个S16是20ms原始数据。双麦与refL降到16kHz，各320样本；3A按当前256点vendor块处理，通过必要FIFO交付320点。wake只检测热词，vad负责逐帧分类及原有准入/尾音保护。

这些namespace直接持有句柄和算法历史，不是旧对象的代理。硬件细节在各模块内，读取和处理顺序在采集线程中可见。48kHz/Mode1/256点/Snowboy ABI是保留的适配边界，不凭host结果改动。

## 2. 开口与句首

读 `client/src/application/voice_client.cpp` 的 `App_ReadSpeech`，再读 `client/src/audio/speech.cpp`：

```cpp
const auto read = audio_capture::read(&frame, 20ms);
const speech::Result result = speech::update(frame, speaking);
playback::set_scale(result.playback_scale);
```

Result只包含决定和指向原PCM的指针，生命周期到下一次update/listen/reset。Start或Barge时，应用先`voice_net::start`，再遍历`result.frames[0..count)`调用`voice_net::send`；最后一帧成功后调用`voice_net::end`。当前帧在pre-roll内只发送一次，实时帧不再复制成AudioEvent。

正常开口保持120ms VAD确认；追问要求400ms近讲，未准入短句的旧END清掉。插话保留原来的候选→静音→等参考→清尾音→再确认，没有简化成VAD命中就打断。

## 3. 从服务器到扬声器

App_ReceiveReply直接调用`playback::begin/write`。DONE只调用`playback::finish`，不会宣布已经播完。

`client/src/audio/playback.cpp` 的 `PlaybackTask` 持有队列并直接执行：

```cpp
audio_convert::playback(frame.pcm.data(), frame.used, &stereo);
WriteToSpeaker();
```

WriteToSpeaker直接计算同帧peak、应用用户音量与探测scale，然后ALSA write。收尾中的Finish先取尽滤波有效尾音，再drain；只有此后status为Drained。只有实际声卡边界执行16→48kHz和L/R复制。cancel中断正在进行的write/drain，新一轮必须等待收尾并清滤波历史。

## 4. 当前源码导航

| 文件 | 学习内容 |
| --- | --- |
| audio/audio_capture.cpp | 读取、处理、发布及检测器帧边界命令 |
| audio/speech.cpp | 一个句首环、追问准入、插话探测和借用结果 |
| audio/playback.cpp | 有界队列、蓄水、cancel、EOS、drain |
| network/voice_net.cpp、voice_codec.cpp | START/PCM/END/CANCEL，WSS与旧轮隔离 |
| platform/rv1106/alsa_audio.cpp | Mode1、完整短读/短写、XRUN、中断和释放 |
| platform/rv1106/audio_convert.cpp | 采集联合降采样、直接双声道输出、极短尾音 |
| platform/rv1106/rockchip_3a.cpp | 实际RKAUDIOParam树、256块与PCM/metadata对齐 |
| platform/rv1106/wake.cpp、vad.cpp | 薄vendor封装、负值错误、声学保护 |

所有路径相对client/src。Host测试执行真实任务与上述处理胶水，仅ALSA设备和vendor核心使用替身；它仍不证明厂商ABI、真实回采和声学效果。当前结果见[结构重写记录](../test/namespace-rewrite.md)。
