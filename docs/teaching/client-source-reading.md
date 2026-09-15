# 当前客户端源码阅读入口

先读[按数据流阅读](audio-pipeline.md)，再读[分关实验](README.md)。

1. `client/apps/boompi_client/main.cpp`：配置、App_Init、循环App_Process、App_Close。
2. `client/src/application/voice_client.cpp`：收回复直接播放，取采集帧交speech，START/PCM/END上传，触摸/音量和超时。
3. `client/src/audio/voice_input.cpp`：真实采集线程中的ALSA→转换→3A→wake→vad。
4. `client/src/audio/speech.cpp`：借用原PCM的准入结果，不持有设备或线程。
5. `client/src/audio/playback.cpp`：自己的队列、转换、声卡及取消/尾播。
6. `client/src/network/voice_net.cpp`：真实WSS，私有状态与线程，不包装旧VoiceLink。
7. `client/src/platform/rv1106/`：只在需要理解硬件/第三方细节时深入。

UI/触摸/摄像头继续沿用原有模块；服务端保持Key-only配套黑箱。旧结构与旧API由Git历史保存，不保留可调用兼容层。协议见[BPV4](../../protocol/protocol-v4.md)。
