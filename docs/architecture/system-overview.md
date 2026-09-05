# boomPI 教学版 v2 架构

服务端作为一个配置工具部署。客户端主课程围绕 VoiceApp、VoiceAudio、VoiceLink、DeviceUi 四个具体边界展开。

逐步补写和验证见[分关实验](../teaching/README.md)。DeviceUi只协调页面、快照和资源；SPI屏幕、GT911触摸及其恢复顺序由私有DisplayTouch拥有。VoiceAudio用Process推进音频处理，用ListenMode区分唤醒后的开口与回答后的追问。

```text
main → VoiceApp（唯一六状态）
         ├─ VoiceAudio → AudioEngine → RV1106 ALSA / 3A / Snowboy / VAD
         ├─ VoiceLink  → network_setup + WSS + v2 codec
         └─ DeviceUi   → LVGL / SPI / GT911 + CameraCapture
```

application 是唯一 generation 分配者；网络线程只校验 wire 顺序并过滤旧代。音频只决定“是否为近讲、是否结束、是否物理播完”，不决定网络轮次或追问窗口。UI 显示快照不复制对话状态。

主状态为 Offline、Idle、Listening、Uploading、Waiting、Speaking。Listening 同时承载唤醒后的 6 s 开口窗口与播放后的 3 s 追问窗口；VoiceAudio 保留各自声学准入策略。Speaking 一直持续到物理 PlaybackDone，网络 done 不会提前宣布扬声器已停止。

v2 每个连接只有一个活动 generation。首 PCM 的 START 创建轮次，END 提交；SUPERSEDE 同时撤回上轮未听完的回答。STOP 退休当前工作且不等待 ACK。所有入站文本、PCM 和播放完成都携带 generation，避免旧工作污染新问题。严格协议见 [protocol-v2.md](../../protocol/protocol-v2.md)。

客户端长期执行上下文为 application、capture、playback、VoiceLink、UI；摄像头按需增加一个。capture/playback 保持 SCHED_FIFO 40/30，失败时警告。网络、显示、文件操作不进入 ALSA 热路径。

API Key 只在服务端；TLS身份与SPKI继续复用。教学口令仅适用于可信局域网，发现并不认证。默认不保存原始PCM或完整对话。

课程入口见 [客户端README](../../client/README.md)，实现记录见 [teaching-refactor.md](teaching-refactor.md)。Host 测试、真板 ABI 与真人声学分别验收，不能互相替代。

产品源码和Host替身由CMake选源，业务与驱动中不混合平台条件分支。本轮整理和统计见 [readability-cleanup.md](readability-cleanup.md)。
