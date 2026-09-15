# 客户端阅读顺序：沿着一帧数据往下走

本版按用户提供的正式业务样例整理。先看每个任务怎样取数据、处理和交付，再看它调用的驱动和算法。服务端作为配套黑箱，只理解客户端输入输出协议；参考仓库的 test/text 和演示代码不进入本阅读主线。

## 1. main：配置、初始化、循环、退出

打开 [main.cpp](../../client/apps/boompi_client/main.cpp)，顺着四个中文步骤读：

```text
LoadClientConfig：读取设备身份与可选服务器配置
App_Init：打开显示、音频，启动网络
while：反复执行 App_Process()
App_Close：停止网络，回收音频与显示
```

main 中的实际循环是：

```cpp
bool succeeded = App_Init(config);
while (stop_requested == 0 && succeeded) {
  succeeded = App_Process();
}
```

没有退出信号、上一轮处理成功，就继续下一轮。信号只修改退出标志，线程和设备在 App_Close 中回收。命令行辅助功能保留在文件下方：`--check-config` 检查配置，`--save-wifi` 从标准输入保存凭据，`--voice-loop` 与默认启动等价。

配置声明见 [voice_client_config.h](../../client/include/boompi/config/voice_client_config.h)。学生不填写声学校准参数；固定服务器地址和公钥指纹由教师预置，留空时自动发现。

## 2. 应用模块：四个普通函数

应用接口只有 App_Init、App_Process、App_Close、App_GetError。没有应用对象、构造函数或析构函数；程序只有一份对话状态，由主线程管理。初始化和处理中的库异常在模块内转为失败结果，main 仍走统一关闭流程。

打开 [voice_client.h](../../client/include/boompi/application/voice_client.h) 和 [voice_client.cpp](../../client/src/application/voice_client.cpp)。文件先集中四个对外函数，后面按回复、录音、触摸摆放内部处理函数。

| 函数 | 直接展开的处理过程 |
| --- | --- |
| App_Init | 读取音量 → 打开显示/触摸 → 打开音频 → 启动网络 |
| App_ReceiveReplyAndPlayAudio | 取服务器消息 → 检查连接/轮次 → 更新字幕或交给播放 |
| App_ReadSpeechAndUpload | 取本轮录音结果 → 确认开口/插话 → 顺序上传 PCM → 结束输入 |
| App_ReadUserAction | 取触摸操作 → 调整音量或停止回答 |

App_Process 每次按“检查超时、回复、录音、触摸”的顺序调用这些函数。它不自己循环，成功返回 true，致命故障返回 false，错误原因由 App_GetError 返回。

第一次跟正常问答：

```text
Wake → App_WaitForSpeech
SpeechStart → App_BeginUpload（分配本轮 generation）
Pcm → App_UploadSpeechFrame（首帧 START，末帧 END）
服务器 Audio → App_QueueReplyAudio
PlaybackDone → App_WaitForSpeech(FollowUp)
```

开始事件和对应的句首 PCM 放在同一批结果中。应用按顺序处理；句尾、停止或失败后，放弃该批剩余项，避免旧声音进入下一轮。

## 3. 音频任务：麦克风的数据从哪里来

先读 [audio_format.h](../../client/include/boompi/audio/audio_format.h) 和 [audio_frames.h](../../client/include/boompi/audio/audio_frames.h)：20 ms 是一帧，采样率决定每帧样本数，声道数决定采样时刻包含多少个值。

然后打开 [audio_tasks.h](../../client/include/boompi/audio/audio_tasks.h) 与 [audio_tasks.cpp](../../client/src/audio/audio_tasks.cpp)。Start 启动两个任务；任务函数放在内部辅助函数之前。

```text
ReadMicrophoneTask
  1. 在帧边界处理检测器复位、播放武装请求
  2. ReadCapture20ms：读取 48 kHz 四通道原始帧
  3. ProcessCapture20ms：输出 16 kHz 单声道与检测结果
  4. SendCaptureToQueue：放入采集队列，通知等待者

主线程 ReadProcessedFrame：取出已处理的采集帧
```

采集队列保存 80 ms，满时明确报告断点。队列只负责线程交接；是否开始一句话由 VoiceAudio 决定。

## 4. 一帧声音怎样经过处理

打开 [audio_pipeline.cpp](../../client/src/platform/rv1106/audio_pipeline.cpp)，直接读 ProcessCapture20ms 的四步：

```text
AudioConverter::ConvertCapture  四通道联合降采样，取双麦与 refL
RockchipVoiceDsp::Process       3A 处理，输出单声道并对齐元数据
SpeechDetector::Detect         唤醒检测与 VAD
SpeechDetector::GateNearVoice  根据真实播放参考屏蔽预热和尾音
```

每一步接收上一阶段的结果。进一步阅读分别进入 [audio_converter.cpp](../../client/src/platform/rv1106/audio_converter.cpp)、[rockchip_voice_dsp.cpp](../../client/src/platform/rv1106/rockchip_voice_dsp.cpp)、[speech_detector.cpp](../../client/src/platform/rv1106/speech_detector.cpp)。

声卡读写在 [alsa_audio.cpp](../../client/src/platform/rv1106/alsa_audio.cpp)。旧 Snowboy C++ ABI 只在 [snowboy_legacy_bridge.cpp](../../client/src/platform/rv1106/snowboy_legacy_bridge.cpp) 隔离。板级预置在 [board_voice_profile.h](../../client/src/platform/rv1106/board_voice_profile.h)，本轮没有重新标定。

## 5. VoiceAudio：一帧怎样成为一句话

打开 [voice_audio.cpp](../../client/src/audio/voice_audio.cpp)：

```text
ProcessEvents → ReadAndProcessCaptureFrame → ProcessCaptureFrame
  正常听音：ProcessListeningFrame
  正在播放：ProcessBargeFrame
```

正常听音时保存最近 500 ms。确认开口后，EmitBufferedSpeech 先放 SpeechStart，再放保留的 PCM；之后实时交付每一帧，VAD 收尾时标记 end。追问需要重新累计 400 ms 近讲。

第二遍再看插话的四个阶段：WaitCandidate → WaitReferenceLow → WaitEchoTail → ConfirmNearSpeech。先确认候选，短暂静音，等硬件参考和房间尾音退去，再确认人声。确认后交付 Barge 和保留的 PCM，应用分配新 generation，以 START|SUPERSEDE 替换旧回答。

## 6. 回复怎样从队列到扬声器

回到 audio_tasks.cpp 的 PlaySpeakerTask：

```text
等待播放条件
  → 停止请求到达时先清理旧播放
  → 首播准备声卡
  → 从回复队列取一帧，释放队列锁
  → Render20ms：升采样、音量/限幅、复制双声道、写声卡
  → 最后一次写完后 drain，失败或插话则 drop
```

原有首播 180 ms 缓存、欠载后 40 ms 重缓冲、30 ms 抖动宽限和 1.5 s 队列容量保留。网络 END 表示不会再来新数据，PlaybackDone 才表示实际尾播已完成。

## 7. 网络、显示、摄像头

- [voice_link.cpp](../../client/src/network/voice_link.cpp)：NetworkTask 管理准备网卡、连接和重连；ProcessConnection 收消息、发送排队音频/STOP、维护心跳。单帧格式读 [voice_codec.cpp](../../client/src/network/voice_codec.cpp)，协议读 [protocol-v2.md](../../protocol/protocol-v2.md)。
- [device_ui.cpp](../../client/src/ui/device_ui.cpp)：DisplayTask 初始化显示，循环取状态/字幕/图像，再处理 LVGL 和触摸。页面布局读 [lvgl_screen.cpp](../../client/src/ui/lvgl_screen.cpp)，硬件读 [display_touch.cpp](../../client/src/platform/rv1106/display_touch.cpp)。
- [camera_capture.cpp](../../client/src/ui/camera_capture.cpp)：CapturePreviewTask 创建管线，拼出完整像素帧，通知显示任务，退出时回收管线。

最后再回应用看 App_CheckTimeout、App_HandleAudioFault、App_StopAndListen 和 App_GoOffline，理解超时、缺帧、手动停止和断线怎样结束当前轮。

现有验证入口保留在 client/tests/ 与 scripts/teaching_lab.py；主机测试证明覆盖到的逻辑，不代替 RV1106 交叉构建与真板语音验收。
