# 音频运行时边界

## 当前实现

2026-08-03 的板端教学候选在音频主线上只保留三个产品边界和一个私有板级实现：

- `VoiceClient`（`application/`）：语音状态机、重连、turn、pre-roll 和打断编排。
- `AudioEngine`（`audio/`）：唯一播放线程、固定 TTS 环和播放控制。
- `VoiceTransport`（`network/`）：持久 WSS、TLS SPKI 固定、协议身份/序号校验和收发；
  当前 property_tree 解析器的 JSON 类型/重复键限制仍须替换或补齐。
- 私有 `AudioBackend`（`platform/rv1106/`）：由 `AudioEngine` 独占，内聚 ALSA 全双工、
  重采样、Rockchip 3A、Snowboy、WebRTC VAD 和近讲判定，不向 application 暴露。

语音核心生产 C/C++ 共 18 个文件、2643 ELOC，其中 280 ELOC 是
Rockchip/Snowboy vendor 集成，产品逻辑为 2363 ELOC；UI 显示层另计 1926 ELOC，不占语音核心预算。UI 和网络启动不介入逐帧 PCM 路径。
旧 `manual_single_turn`、独立 capture pump、
通用 EventBus、自写 WSS/parser、renderer/resampler/gain 框架和 playback
control/committer/worker 已删除；不得为了恢复历史测试结构重新引入。

## 实际数据流

```text
ALSA capture: 48 kHz / S16_LE / Mode1 4ch / 20 ms
  -> [mic0,mic1,refL,refR]
  -> phase-aligned 48 -> 16 kHz conversion
  -> select [mic0,mic1,refL], discard duplicate refR
  -> Rockchip 3A
  -> Snowboy + WebRTC VAD
  -> WSS: 16 kHz / S16_LE / mono

WSS TTS: 24 kHz / S16_LE / mono
  -> 180 ms initial prebuffer (short EOS exception) / fixed 1.5 s hard limit
  -> 24 -> 48 kHz
  -> gain + int16 饱和钳位（barge 一次性静音用 SetPlaybackScale(0)，无渐变 duck）
  -> ALSA playback: 48 kHz / S16_LE / stereo
  -> Codec Mode1 digital loopback -> refL/refR capture slots
```

ALSA 精确协商为 capture `960/1920`、playback `960/3840` frame period/buffer。采集线程阻塞读取完整 20 ms
帧后推入固定 4 槽（80 ms）capture 环，主 actor 每轮消费一帧；actor 落后超限即显式断帧
（`actor_overrun`）而不是静默丢帧。重连和云端等待期间采集持续运行，避免硬件 overrun。

## 固定容量

- capture 环：4 × 20 ms；actor 落后 80 ms 即显式断帧，不静默丢弃。
- 唤醒前卷：25 帧，即 500 ms。
- 网络事件环：64 项；溢出视为连接失效，不覆盖旧事件。
- TTS 队列：75 × 20 ms，即 1.5 s；满时取消当前响应，不无限堆积；只有取消发送本身失败时才重连。
- 不存在软件播放参考环。硬件 Mode1 在同一个 capture frame 提供 `refL/refR`，生产 3A 只消费
  `refL`；首次有效硬参考后
  保留 600 ms 3A 自适应 warm-up，期间不允许参考泄漏触发打断。
- 单个上行 PCM：最多 640 bytes；单个下行 PCM：最多 960 bytes。
- WebSocket 消息：最多 64 KiB 加协议头，发送端同时检查库内待发送字节数。

实时 PCM 路径使用对象内固定数组，不做逐帧 heap 分配。控制 JSON 和 WebSocket 库内部
仍可分配内存，但不在 ALSA 逐帧处理内创建无界容器。

## 执行上下文与所有权

当前源码创建五个工作线程，加上运行 `VoiceClient::Run` 的主线程（唯一会话 actor），共六个
客户端自有长期执行上下文：

| 上下文 | 独占职责 |
| --- | --- |
| 主线程（会话 actor） | 帧循环、状态机、turn/epoch、pre-roll、重连调度；业务状态唯一写者 |
| 采集线程 | 独占 ALSA capture 与 3A/Snowboy/VAD，把成帧推入 4 槽 capture 环 |
| 播放线程 | 激活期间独占 TTS 渲染、gain/饱和钳位和 ALSA playback |
| WebSocket service 线程 | TLS/WSS I/O；回调只向 64 项事件环提交结果 |
| UI 线程 | 合并低频状态刷新、SPI 写屏和 GT911 触摸，不处理 PCM |

另有网络启动线程（DHCP/Wi-Fi、UDP 发现、endpoint 持久化）在 `VoiceClient` 内长期运行；
配网二维码期间会临时增加一个 camera 工作线程。板端实测总线程数（含链接依赖内部线程）
尚需重新采集；依赖库线程不拥有 boomPI 业务状态。若后续出现更多依赖线程，必须重新记录
来源、优先级和退出行为。

业务状态只由主线程（会话 actor）修改。采集/播放线程只碰自己的环，不切换 turn；网络回调
不直接调用状态机。播放开始前，
actor 只在 `active=false` 且受 `AudioEngine` mutex 保护时复位播放重采样相位。
`Close` 先通知工作线程退出并 join，再释放 ALSA、DSP、Snowboy、VAD 和 WSS 资源。

## 唤醒、追问与打断

- Snowboy 命中后重置 Snowboy/VAD 历史，最多等待 6 s 用户开口。
- VAD 连续静音 700 ms 后提交；单轮语音硬上限 60 s。
- 正常回复结束后进入 3 s 免唤醒追问。
- 播放时先保持原音量连续确认 6 帧（120 ms）近端候选，再一次性静音；不使用中间音量。
  随后最多等待 15 帧取得连续 3 帧低 reference，等待 3 帧（60 ms）清除声学尾音、重置
  listener，并以连续 3 帧（60 ms）重新确认近端语音。只有二次确认成功才执行
  `DropPlayback`、清空本地 TTS 和发送 cancel；cancel ACK 本身不能启动新 turn，新 turn
  还需有效 VAD start、连续 20 帧（400 ms）低 reference，并受 500 ms 准入窗口约束。
  参考未降低或二次确认失败时恢复播放并冷却 15 帧（300 ms）。
- 自然播放结束不走上述主动探针：backend 先抑制 15 帧（300 ms）尾音并重置 VAD，
  follow-up 随后必须连续 20 帧（400 ms）才开始新轮。

播放 reference 来自 Mode1 硬件数字回采，不使用未播放的原始 TTS 包，也不维护软件时间线。
生产 `near_voice` 使用 3A 后 WebRTC VAD；raw mic DC/底噪较高，禁止使用 raw RMS 近讲门。
供应商公开 ABI 没有 DTD 事件，`wakeup_status` 不是 DTD。自然播放结束后的 300 ms 尾音窗
禁止触发并在末端重置 VAD；主动打断保留已经确认的近讲 VAD，避免短打断词丢失结束事件。
主动硬参考探针是应用层防自激 containment 候选：由于它故意静音扬声器，探针区间不构成 AEC 效果证据，
也不应纳入 ERLE 或 AEC 评分。当前生产 DSP mask 为 `1109`，启用 FastAEC、AES、ANR、
Dereverberation 和 STDT，vendor AGC 关闭；`ALC31/ref1/delay0` 只是当前候选参数。

## 故障语义

- capture xrun 或四通道重采样错位会建立 capture discontinuity，并重置 3A/Snowboy/VAD 历史；
  播放 xrun 在原块上恢复，硬参考仍以实际回采为准。无法恢复的播放错误终止当前播放并上报失败，
  旧音频不得跨 turn 继续发送。
- WSS 断线使当前 turn 失败；重连退避为 1、2、4、8、16、30 s，连接动作不阻塞采集。
- PCM 序号、turn/stream/response/epoch 或采样率不匹配时拒绝数据，不做实时音频重传。
- 正常结束 drain；打断或错误 drop，避免旧 TTS 在下一轮继续播放。播放线程必须把正常结束、
  主动丢弃和 ALSA/重采样失败区分开，application 不得把硬件失败当作正常 follow-up。

## 验证边界

当前职责候选的交叉构建、板端加载、Rockchip 3A/Snowboy 初始化、持久 WSS、真人首轮问答、
3 s 追问、完整播放和播放中打断已经通过。受控 double-talk、量化延迟、噪声误触发和断网恢复
仍以目标板专项验收为准。
最新同类无人声/嘈杂环境回归为：AGC ON `n=5`，`confirmed=4/5`、`follow=5/5`、
`attempts=119`；AGC OFF 累计 `n=10`，`confirmed=2/10`、`follow=3/10`、`attempts=43`。
关闭 AGC 明显改善但尚未充分解决误触发；环境噪声仍是混杂因素。
Mode1 保留四通道 `[mic0,mic1,refL,refR]` 采集；生产 direct 3A 只接收
`[mic0,mic1,refL]`，在 vendor 边界丢弃重复的 `refR`。启动/退出证据见
[P0 Mode1 硬件播放参考验证记录](../test/p0-mode1-hard-reference-validation-20260801.md)。真人
double-talk、最终壳体 ERLE/残余回声和最大音量仍待验收。
