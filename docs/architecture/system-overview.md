# 当前boomPI结构

```text
ALSA原始输入 → voice_input中的转换/3A → 有界交接
  → App_Process中的wake/VAD/speech → voice_net START/PCM/END
voice_net TEXT/AUDIO/DONE → playback队列/ALSA → Drained → 追问
UI/触摸/音量/配网/摄像头沿用原有模块
```

主流程只有四个用户业务状态，网络拥有连接和上传事实。检测与策略由应用线程独占，删除跨输入线程的业务命令握手。语句策略集中在speech，VAD是只返回逐块人声/无声/错误的薄封装。

语音输入线程保持与网络独立；播放快照与PCM同批交付，防止排队后读取未来状态。48k硬件配置、3A块适配和Snowboy旧ABI不凭预算删掉。代码、预算超出和验证边界见[本轮记录](../test/budget-refactor.md)。

线协议为[BPV4](../../protocol/protocol-v4.md)：固定文本控制、12字节PCM头、双向16kHz。没有旧JSON双栈。服务端仍是只配置Key的配套程序，课程不扩展为Go或云端编排开发。
