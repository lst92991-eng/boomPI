# Qwen Voice Loop HIL 快照（2026-07-29）

- 记录时间：2026-07-29 21:01:58 +08:00（Asia/Shanghai，以 Windows host 时钟为准）
- 快照标记：`QWEN-VOICE-LOOP-HIL-SNAPSHOT-20260729-VOICE9`
- 板卡：第三块 RV1106 自研板
- 阶段结论：**真实 Qwen 语音整链路曾跑通，但当前版本仍有阻断体验的问题**
- 发布属性：**HIL 调试快照，不是 production release**

## 本快照固定的版本

板端使用 `voice9` HIL 客户端：

```text
build type:              Debug
CMAKE_CXX_FLAGS_DEBUG:   -O2 -g
boompi-client SHA-256:   4c5cb968bad32cdc2d3a4a757321bf9f19d1ae0c20eebc22de156883fa31765a
```

这里的 `-O2` 只用于真实板卡上的性能对照和问题定位；构建仍属于 Debug/HIL，不能据此宣称
已完成 Release 构建、发布验收或长期稳定性验证。

服务端已改为 Qwen 官方 `server_commit` 连续 TTS 方式，当前测试所用候选/运行二进制为：

```text
boompi-intelligence-server SHA-256:
83a371fc8d751a21914679eee6cec2d7cf0e8abc53d802e4348e35f1271b96e5

build: go1.26.5, linux/amd64, trimpath=true, CGO_ENABLED=1, glibc dynamic executable
```

哈希只用于识别本轮二进制，不替代源码提交、构建参数、依赖锁定或发布签名。
该 Ubuntu 候选不是跨平台静态单文件发布产物。

本轮 Ubuntu 实机服务使用的非敏感关键配置为：

```text
region:                 china-beijing
conversation_mode:      intelligence
asr_model:              qwen3-asr-flash
reasoning_model:        qwen3.7-max
reasoning_effort:       medium
tts_model:              qwen3-tts-flash-realtime
tts_voice:              Cherry
first_response_timeout: 2m
```

仓库当时的 `configs/config.example.yaml` 仍以目标市场的 `singapore` 区域作为可复制模板；因此它与
本轮 HIL 的区域并不相同。复现实机现象时必须显式改成上面的区域和模型配置，不能把示例文件
直接称为本轮运行配置。API Key、workspace ID 和设备 token 仍只能由环境注入。

## 运行拓扑

本轮不是板卡直接访问 Qwen，实际数据路径为：

```text
第三块 RV1106 板
  -> 板端本地端口
  -> Windows 主机上的 SSH tunnel
  -> Ubuntu 中的 boomPI Go 服务端
  -> Qwen ASR / 对话模型 / TTS
  -> Ubuntu 服务端
  -> Windows SSH tunnel
  -> RV1106 ALSA playback
```

本记录不包含 API Key、设备 token、TLS 私钥、Wi-Fi 密码、完整设备身份或 SSH 凭据。

## 已有真实 HIL 证据

在第三块真实板卡上已经观察到以下主路径事件，并曾完成从说话到扬声器回答的完整闭环：

1. Snowboy 检测到唤醒词；
2. VAD 检测到语音开始和语音结束；
3. 客户端提交 voice turn；
4. 服务端接入真实 Qwen ASR、对话和 TTS；
5. 板端收到下行 PCM，并通过 ALSA 扬声器播放回答。

因此，本快照证明 Snowboy、VAD、板端上传、服务端 Qwen 调用、下行传输和 ALSA 播放这条
纵向链路**曾经跑通**。它不证明每一帧都连续，也不证明延迟、音质、双麦、AEC、打断、
3 秒 follow-up 或长时间稳定性已经达到产品要求。

本轮服务端将逐短片段独立提交 TTS 的方式调整为 `server_commit` 连续 TTS，目标是减少
片段之间的突发和空洞；板端 `voice9` 同时使用 Debug + `-O2`，用于排除未优化客户端在
RV1106 上无法实时消费音频这一变量。两项变更已经进入本快照，但仍不能解释或关闭下面的
现存问题。

## 当前已知且未解决的问题

### 1. 输入存在丢帧

现场体验和日志表明上行输入存在丢帧或不连续现象。当前尚未取得足够的逐阶段时间戳、
sequence discontinuity、采集队列水位和服务端收包统计，无法确认丢帧发生在：

- ALSA capture 或板端采集/DSP 热路径；
- 板端上行队列或 WebSocket 发送；
- SSH tunnel；
- Ubuntu 服务端接收、转发或 Qwen ASR 输入侧。

因此根因状态为：**未定位**。在补齐端到端 sequence 与单调时钟证据前，不把该问题归因于
某一个模块，也不以静音填充或伪造连续序号掩盖丢帧。

### 2. 接收音频和首音延迟很高

板端从提交 voice turn 到收到/播放回答音频的等待明显偏高。当前还没有完整记录 VAD 结束、
ASR 首字、LLM 首 token、TTS 首包、板端首个下行 PCM 和扬声器首声的分段时间，因此无法
区分延迟主要来自：

- 上行音频是否完整、ASR 是否等待额外输入；
- Qwen ASR、LLM 或 TTS provider；
- 服务端 TTS 编排及 provider-to-board 转发；
- SSH tunnel/WSS 调度；
- 板端初始预缓冲、重采样或 ALSA 启动。

服务端改成连续 TTS 后仍需重新采集上述分段时间。当前结论仍是：**延迟高，根因未定位**。

### 3. 历史播放故障仍需回归关闭

此前真实运行中出现过：

```text
ALSA playback write failed: Broken pipe
playback jitter queue is full
```

前者对应 ALSA `EPIPE`/xrun，后者说明播放抖动队列在某次运行中达到容量上限。`voice9` 的
Debug + `-O2` 和服务端连续 TTS 变更可能改变故障频率，但本快照没有足够的重复测试证明
二者已经关闭。后续必须结合 ALSA recovery 计数、队列高水位、入包间隔和消费者写入耗时
一起复测，不能只以“能听到回答”判定通过。

## 下一轮最小定位要求

下一次真板复测应只补齐定位所需的低频汇总指标，不在音频热路径逐帧打印日志：

- 上行 capture、发送和服务端接收的帧数、首末 sequence、discontinuity 数；
- VAD 结束、ASR 首字、LLM 首 token、TTS 首包、板端首包、扬声器首声的单调时间戳；
- 下行包最大间隔、超过 100 ms/300 ms 的间隔次数；
- 播放队列高水位、欠载/重新预缓冲次数、ALSA xrun/recovery 次数；
- 板端单次 ALSA write 的最大耗时及超过 20 ms 的次数。

先用同一问题完成一轮有界复测，再决定修复服务端、隧道/WSS、板端采集或播放路径，避免
同时扩大音频框架而失去回归定位能力。

## 验收边界

当前状态只可标为：

```text
Snowboy wake:              已观察通过
VAD speech start/end:      已观察通过
Qwen ASR/LLM/TTS 闭环:     曾跑通
连续 TTS server_commit:    已进入当前服务端候选
板端 Debug + O2:           已进入 voice9 HIL 候选
输入连续性:                未通过，存在丢帧
接收/首音延迟:             未通过，偏高且根因未定位
播放稳定性:                未关闭，历史有 EPIPE 和 jitter queue full
production release:        否
```

只有输入连续性、分段延迟和播放队列/ALSA 稳定性在同一真实拓扑上取得可重复数据并通过后，
才能把这份 HIL 快照提升为候选发布验证记录。
