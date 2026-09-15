# 顺着一帧声音读代码

## 初始化

App_Init依次打开voice_input（原始输入、转换器和3A）、wake、vad、playback；两路PCM准备好以后start输入线程，最后open网络。失败后main调用App_Close，按网络、播放、输入、检测器的顺序回收。

## 输入处理线程

`client/src/audio/voice_input.cpp` 的CaptureTask执行以下真实调用，源码在每步之间检查失败和停止：

```cpp
alsa_audio::read(raw.pcm.data(), &raw.discontinuity);
audio_convert::capture(raw, &channels);
rockchip_3a::process(channels, &frame.pcm, &metadata);
frame.output = playback::observe();
```

只有原始PCM操作在alsa_audio中。3A和重采样由输入任务显式调用，不再把整条流水线藏在名为采集的API内。断流复位滤波/3A并交付断点，不能把前后两段拼成连续输入。

## 应用线程

`App_ReadSpeech`取处理帧，依次调用wake::detect、vad::process、speech::update，然后START、按顺序发送借用PCM、最后END。VAD负值立即作为错误；语句模块决定是否开口或结束，不再由VAD库封装决定业务。

Result只借用句首环/当前帧；调用下一次update/listen/reset前必须消费。当前帧已在前滚环时只发送一次，尾帧先PCM后END。发送失败立即CANCEL并丢弃剩余借用，不构造第二份PCM事件数组。

应用状态为Idle/Listening/WaitingReply/Speaking，在线和已START由网络已有状态表示。声学准入集中在speech：原始麦准入、600ms AEC预热、300ms自然尾音、400ms追问，以及候选→静音→等参考→清尾音→确认插话。playback只提供渲染事实与播放服务，不管理问答。

## 播放

App_ReceiveReply直接begin/write。网络验证包序号，playback隔离generation并维护容量。DONE只finish输入，播放线程先取滤波有效尾音再drain；物理播完才追问。取消与关闭会中断旧write/drain，新一轮不得抢过旧收尾；真实设备失败不能被取消掩盖。

## 文件

| 职责 | 文件（相对client/src） |
| --- | --- |
| 原始声卡配置/读写 | platform/rv1106/alsa_audio.cpp |
| 实时输入处理/交接 | audio/voice_input.cpp |
| 3A配置/处理/释放 | platform/rv1106/rockchip_3a.cpp |
| 唤醒、逐块VAD | platform/rv1106/wake.cpp、vad.cpp |
| 语句/前滚/追问/插话策略 | audio/speech.cpp |
| 播放/取消/尾播 | audio/playback.cpp |
| 固定文本控制和WSS | network/voice_codec.cpp、voice_net.cpp |

Host使用真实处理胶水、FFmpeg和任务，仅声卡调用/vendor核心由替身提供；不代表真实声卡、ABI、声学或云端验收。当前按职责预算统计见[本轮记录](../test/budget-refactor.md)。
