# 音频运行时边界

## 当前实现

2026-07-31 的板端候选只保留三个产品对象：

- `Client`：语音状态机、重连、唤醒、VAD、turn 和打断。
- `AudioEngine`：ALSA 全双工、重采样、Rockchip 3A、Snowboy、WebRTC VAD、播放和参考路径。
- `Transport`：持久 WSS、TLS SPKI 固定、协议校验和收发。

整套自写生产 C++ 共 15 个文件、1904 ELOC。旧 `manual_single_turn`、独立 capture pump、
通用 EventBus、自写 WSS/parser、renderer/resampler/gain 框架和 playback
control/committer/worker 已删除；不得为了恢复历史测试结构重新引入。

## 实际数据流

```text
ALSA capture: 48 kHz / S16_LE / stereo / 20 ms
  -> 48 -> 16 kHz stereo
  -> left mic + right mic + aligned playback reference
  -> Rockchip 3A
  -> Snowboy + WebRTC VAD
  -> WSS: 16 kHz / S16_LE / mono

WSS TTS: 24 kHz / S16_LE / mono
  -> fixed 1.5 s queue
  -> 24 -> 48 kHz
  -> gain + limiter + optional duck
  -> ALSA playback: 48 kHz / S16_LE / stereo
  -> final played PCM -> 16 kHz aligned reference -> Rockchip 3A
```

ALSA 精确协商为 960 frame/period、3840 frame buffer。采集控制线程阻塞读取完整 20 ms
帧，不再经过中间 capture 队列；重连和云端等待期间仍持续读取，避免硬件 overrun。

## 固定容量

- 唤醒前卷：25 帧，即 500 ms。
- 网络事件环：64 项；溢出视为连接失效，不覆盖旧事件。
- TTS 队列：75 × 20 ms，即 1.5 s；满时当前连接失败，不无限堆积。
- 播放参考环：16 帧；正常以 3 帧，即 60 ms lead 开始消费。
- 单个上行 PCM：最多 640 bytes；单个下行 PCM：最多 960 bytes。
- WebSocket 消息：最多 64 KiB 加协议头，发送端同时检查库内待发送字节数。

实时 PCM 路径使用对象内固定数组，不做逐帧 heap 分配。控制 JSON 和 WebSocket 库内部
仍可分配内存，但不在 ALSA 逐帧处理内创建无界容器。

## 执行上下文与所有权

当前源码只创建两个工作线程，加上调用 `Client::Run` 的主线程，共三个客户端自有长期执行
上下文：

| 上下文 | 独占职责 |
| --- | --- |
| 控制/采集线程 | ALSA capture、DSP、Snowboy/VAD、状态机、turn 和重连调度 |
| 播放线程 | TTS 队列、重采样、gain/limiter、ALSA playback 和 reference 发布 |
| WebSocket service 线程 | TLS/WSS I/O；回调只向 64 项事件环提交结果 |

真板进程当前观测为 4 个线程；额外一个由链接依赖内部创建，不拥有 boomPI 业务状态。若后续
出现更多依赖线程，必须重新记录来源、优先级和退出行为。

业务状态只由控制/采集线程修改。播放线程不切换 turn，网络回调不直接调用状态机。
`Close` 先通知工作线程退出并 join，再释放 ALSA、DSP、Snowboy、VAD 和 WSS 资源。

## 唤醒、追问与打断

- Snowboy 命中后重置 Snowboy/VAD 历史，最多等待 6 s 用户开口。
- VAD 连续静音 700 ms 后提交；单轮语音硬上限 60 s。
- 正常回复结束后进入 3 s 免唤醒追问。
- 播放时近端语音累计 80 ms 先 duck，累计 160 ms 后 `DropPlayback`、清空本地 TTS、
  向服务端发送 cancel，并把已经采集的近端语音继续作为新 utterance。

播放 reference 来自最终写入 ALSA 的 PCM，不使用未播放的原始 TTS 包。能量门只抑制
残余回声造成的误打断，是 vendor AEC 的保护层，不替代 AEC。

## 故障语义

- capture/playback xrun、重采样错位或 reference 不连续都会建立 discontinuity，并重置
  前端历史，旧音频不得跨 turn 继续发送。
- WSS 断线使当前 turn 失败；重连退避为 1、2、4、8、16、30 s，连接动作不阻塞采集。
- PCM 序号、turn/stream/response/epoch 或采样率不匹配时拒绝数据，不做实时音频重传。
- 正常结束 drain；打断或错误 drop，避免旧 TTS 在下一轮继续播放。

## 验证边界

交叉构建、板端加载、Rockchip 3A/Snowboy 初始化、持久 WSS 和空闲运行已经通过。真人
首轮问答、3 s 追问、长回复、播放中打断、噪声误触发及断网恢复仍以目标板人工验收为准。
当前证据见 [板端客户端 2000 ELOC 收敛与启动记录](../test/client-under-2000-refactor-20260731.md)。
