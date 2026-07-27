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

帧槽、SPSC lease、sequence/discontinuity 和 consumer gate 的具体 ownership 见
[音频运行时边界](audio-runtime.md)。

## 音频边界

目标硬件链路是 Codec/ALSA 48 kHz 全双工，候选四通道采集为双麦加双路数字播放参考。Rockchip 3A 按 16 kHz adapter 接入，处理后的 16 kHz mono 同时供 Snowboy、VAD 和服务端上行使用。Qwen 下行默认按 24 kHz mono 标记，板端转为 48 kHz 后再执行音量、提示音混合和限幅。

下列事项仍是 P0 验证闸门：

- Mode1 四通道是否在当前 DTS、驱动和 ALSA 配置下成立。
- 实际通道顺序、极性、数字参考采样位置与时钟关系。
- Rockchip 3A ABI、帧布局和算法能力。
- Snowboy 与目标 libc/libstdc++/ARM ABI 的兼容性。

Mode1 失败时只能进入有记录的软件参考方案评审，不能同时混用硬件和软件 reference。

P2 当前已实现 48 kHz capture/16 kHz mono 的固定帧契约、预分配 SPSC ownership、
producer sequence、consumer continuity gate、可配置四通道解交织/极性、四路
48→16 kHz FIR 和 generation-safe 采集前端编排。它们已通过 host 测试与 RV1106
交叉编译，但尚未连接 ALSA、Rockchip 3A、Snowboy 或任何真实 DSP backend；
Mode1 slot 顺序和实时率仍需 HIL。

## 状态与取消

会话状态和网络状态相互正交。application actor 创建 `turn_id` 和 `epoch`；网络重连、取消或打断后递增 generation，所有迟到帧必须被丢弃。

播放期间约 80 ms 有效近端语音先触发 duck，约 160 ms 确认后清空本地 TTS/字幕并请求服务端取消。新 utterance 必须包含至少 500 ms AEC 后 pre-roll。服务端若不能可靠把文本映射到实际播放位置，应删除整个被打断 assistant turn，不能把未播放全文留入上下文。

## 网络与信任

设备按“缓存 endpoint、UDP 发现、手动 IP”寻找本地服务端。UDP 发现未经认证；首次连接必须在显式 pairing 状态，以屏幕六位码确认当前 TLS SPKI 和 pairing transcript。之后固定 SPKI，并使用独立 device token。

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
