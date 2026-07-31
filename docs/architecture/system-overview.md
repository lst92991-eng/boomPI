# boomPI 系统架构摘要

## 文档状态

本文记录第一版已经确认的架构边界，不代表对应模块已经实现。实现状态应以测试、CI 和真实板端记录为准。根目录 `AGENTS.md` 是更高优先级的开发契约。

## 部署边界

```text
RV1106 device                                      Local host

capture -> DSP -> wake/VAD -> application          boompi-server
   ^                         |                           |
   |                         +---- WSS -----------------+
playback <- jitter <- network                            |
UI/touch <- UI model                              Qwen Singapore
```

- RV1106 客户端负责实时音频、唤醒、VAD、打断、本地状态机、显示和触摸。
- Go 服务端负责设备 session、Qwen adapter、工具 allowlist、会话上下文和应用包分发。
- 板端不保存 Qwen Key，也不直接连接 Qwen。
- 云端不可用时，本地启动、唤醒、UI 和离线提示仍应工作。

## 客户端所有权

下表是完整产品的职责/所有权地图，不要求在 vendor 最小闭环前创建七个线程或对应的
通用层。实际阻塞与实时率测出后，再用最少执行上下文承载这些职责。

| 执行上下文 | 独占资源与职责 |
| --- | --- |
| Capture | ALSA capture handle、采集 sequence 和源时间戳 |
| DSP | 解交织、重采样、AEC/NS/BF/AGC、连续性检查 |
| Wake/VAD | Snowboy、VAD、500 ms pre-roll、打断候选 |
| Playback | TTS buffer、播放重采样、最终增益/限幅、ALSA playback、参考路径 |
| Network | discovery、WSS、收发、credit/backpressure、重连 |
| Application actor | 会话状态、turn/response generation、取消和工具编排 |
| UI | 所有 LVGL/display 对象与触摸事件转换 |

跨线程 PCM 使用预分配有界队列；控制消息使用有界 EventBus。业务状态只能由 application actor 修改。实时音频路径不做文件或网络 I/O，不允许每帧动态分配或无界等待。

有界队列、discontinuity barrier 和 consumer handoff 的具体 ownership 见
[音频运行时边界](audio-runtime.md)；Rockchip 3A/Snowboy 与 vendor 依赖闸门见
[音频后端契约与依赖闸门](audio-backends.md)。

## 音频边界

当前真实运行路径由 `AlsaSingleTurnIo` 直接打开 48 kHz/S16_LE/2 ch capture 和 playback。
20 ms capture 经 48→16 kHz 处理后进入 `RockchipVoiceDsp`，输出 mono 同时供 Snowboy、
VAD 和服务端上行。Qwen 下行是 24 kHz mono，经 renderer/resampler/gain 变为 48 kHz，
随后写入同一 ALSA playback handle；最终播放 PCM 的软件 reference 再送入 3A 和打断门控。

运行时使用 160 ms capture bridge、1.28 s 上限的 playback jitter queue、120 ms 首播预缓冲
和有界 playback reference queue。capture producer 在录音结束、提交网络 turn 和 TTS 播放
期间持续运行；消费者先交接再执行阻塞控制操作。队列溢出或 ALSA recovery 会建立 sticky
discontinuity，直到 DSP reset 和下一消费者就绪，旧帧不得跨 turn 继续发送。

2026-07-31 已删除从未进入真实客户端 ELF 的通用 capture/DSP frontend、playback
control/committer/worker、软件 ledger 和旧 ALSA adapter。当前取消直接由会话状态机执行：
确认近端语音后 `DropPlayback`、清空 jitter queue、发送云端 cancel，并把预卷作为下一轮
utterance 开头。不得为了恢复历史测试结构重新引入这些抽象。

下列事项仍需目标板验收：

- 两个 capture slot 的物理左右、极性和最终壳体下的双麦声学表现。
- 软件 playback reference 的固定延迟、残余回声、double-talk 与最大音量表现。
- Rockchip 3A 的算法实时率和错误恢复；raw MPI 仍只是独立 HIL 候选。
- 正常 TTS 结束后 3 s follow-up 在嘈杂/AGC 环境中的误触发问题。

当前 DTB 的 `TRCM clk-trcm=1` 只表示共享 TX 时钟，vendor Mode2 是独立 mixer 控制；两者
都不证明四通道数字 reference。通过 host fake、依赖 configure 或一次听到声音，也不能替代
上述 HIL。

## 状态与取消

会话状态和网络状态相互正交。application actor 创建 `turn_id` 和 `epoch`；网络重连、取消或打断后递增 generation，所有迟到帧必须被丢弃。

播放期间约 80 ms 有效近端语音先触发 duck，约 160 ms 确认后清空本地 TTS/字幕并请求服务端取消。新 utterance 必须包含至少 500 ms AEC 后 pre-roll。服务端若不能可靠把文本映射到实际播放位置，应删除整个被打断 assistant turn，不能把未播放全文留入上下文。

## 网络与信任

设备按“缓存 endpoint、UDP 发现、手动 IP”寻找本地服务端。UDP 发现未经认证；首次连接必须在显式 pairing 状态，以屏幕六位码确认当前 TLS SPKI 和 pairing transcript。之后固定 SPKI，并使用独立 device token。

在完整 pairing 落地前，当前开发服务端要求 `hello` 携带环境变量人工下发的共享设备令牌，并在打开云端 provider 会话前验证；该措施只用于受控局域网联调，不能替代每设备令牌和 SPKI 配对。

WSS 承载 JSON 控制帧和二进制 PCM。协议细节见 `protocol/protocol-v1.md`。断线后当前 turn 失败，实时语音不做应用层重传。

## 守护与更新

```text
BusyBox init
  -> boompi-supervisor
       -> active slot/bin/boompi-client --foreground
```

supervisor 属于稳定系统层，不随普通应用包更新。局域网应用更新写入 inactive slot，验证离线 release key 生成的签名后切换 pending slot；候选连续启动失败三次回滚。BSP、内核和系统镜像第一版仍由人工烧录。

## 隐私边界

- API Key 只存在于服务端进程环境。
- 默认不持久化 PCM、播放参考或完整会话文本。
- 日志只记录脱敏状态、耗时、计数、错误码和 buffer 水位。
- 诊断包不默认包含录音、完整对话、token、证书私钥或 Wi-Fi 凭据。
