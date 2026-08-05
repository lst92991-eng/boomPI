# boomPI 系统架构

## 范围

本文只描述当前教学版实现。架构事实以源码、唯一交叉构建入口和本轮验证输出为准。

boomPI 由 RV1106 板端客户端和局域网内的 Go 服务端组成：

```text
RV1106                                                    本地服务端

ALSA capture -> AudioBackend -> AudioEngine -> VoiceClient -> VoiceTransport
      ^              |              |              |              |
      |          3A / wake / VAD  有界队列      会话状态机       WSS/TLS
ALSA playback <- 24->48 kHz <- TTS ring <-------------------------+
      |
      +---- Codec Mode1 数字回采 ----> refL/refR

GT911 -> DeviceUi/LVGL <- VoiceClient -> ST7789P3
Ethernet/Wi-Fi -> NetworkBootstrap -> UDP discovery / cached endpoint

Go server -> 设备会话、当前对话、Qwen 中国区适配、TLS 身份
```

板端不保存 Qwen API Key，也不直接访问 Qwen。服务端使用中国大陆区域配置；设备只连接
局域网内的 `boompi-server`。

## 代码职责

| 目录 | 唯一职责 |
| --- | --- |
| `client/src/application/` | 会话状态机、turn/epoch、500 ms pre-roll、追问、取消和打断编排 |
| `client/src/audio/` | application 可见的音频门面、capture handoff、TTS 有界队列和播放线程 |
| `client/src/platform/rv1106/` | ALSA、重采样、Rockchip 3A、Snowboy C ABI、WebRTC VAD 和近讲判定 |
| `client/src/network/` | WSS/TLS、协议 v1、以太网/Wi-Fi 启动、UDP 发现和 endpoint 缓存 |
| `client/src/ui/` | LVGL、ST7789P3、GT911、状态页面、音量控制和 SC3336 本地预览 |
| `server/internal/` | 配置、设备会话、Qwen provider、WSS transport、发现和 TLS 身份 |
| `protocol/` | 客户端和服务端共同遵守的 wire contract 与 fixture |

`VoiceClient` 是唯一修改业务状态的 actor。音频线程、WebSocket 回调、UI 和网络启动代码只
提交事件，不直接切换 turn，也不共享另一套业务状态机。

## 执行上下文

正常语音和显示在线时，客户端有六个自有长期执行上下文；进入摄像头页面后增加一个：

| 上下文 | 调度与所有权 |
| --- | --- |
| application actor | 主线程；唯一拥有会话状态、turn、epoch 和重连编排 |
| capture/DSP | `SCHED_FIFO 40`；独占 ALSA capture、3A、Snowboy、VAD 和采集队列生产端 |
| playback | `SCHED_FIFO 30`；独占 TTS 渲染、增益/限幅和 ALSA playback |
| WebSocket service | TLS/WSS I/O；回调只写入 64 项固定事件环 |
| NetworkBootstrap | DHCP、Wi-Fi、UDP discovery 和 endpoint 持久化 |
| UI | 普通调度、`nice +5`；触摸和合并刷新，不进入逐帧 PCM 路径 |
| camera（按需） | 仅在摄像头页运行，退出页面即回收 |

实时 PCM 使用固定数组和有界队列。代码不允许在逐帧路径中创建无界容器，也不允许 UI、
网络或摄像头阻塞 capture/DSP 线程。

## 音频契约

板端固定打开 Codec `Mode1`：

- capture：48 kHz、S16_LE、4 通道、20 ms，布局为
  `[mic0,mic1,refL,refR]`，ALSA period/buffer 为 `960/1920` frames；
- playback：48 kHz、S16_LE、双通道，ALSA period/buffer 为 `960/3840` frames；
- TTS：服务端下发 24 kHz、S16_LE、mono，板端重采样到 48 kHz 后复制到左右声道；
- 3A：四通道保持同帧重采样到 16 kHz，只把 `[mic0,mic1,refL]` 送入 Rockchip 3A，
  在 vendor 边界丢弃高度相关的 `refR`；
- 上行：3A 输出的 16 kHz、S16_LE、mono 同时供 Snowboy、WebRTC VAD 和 WSS 使用。

当前默认参数是 Snowboy `0.7`、VAD admission `-35 dBFS`、播放中近讲门限
`-25 dBFS`。这些值是运行配置，不是声学指标；调整后必须重新完成真板测试。

详细队列、故障和打断时序见 [音频运行时](audio-runtime.md)，vendor ABI 与依赖边界见
[音频后端](audio-backends.md)。

## 状态、网络与 UI

应用状态包含待机、监听、思考、播放、drain、追问和离线。每个 turn 都带 generation；断线、
取消或打断后递增 generation，旧响应、旧字幕和旧 PCM 不得进入新 turn。播放中确认近讲后，
客户端停止剩余 TTS、发送 cancel，并把保留的 AEC 后 pre-roll 作为新一轮开头。

显式 endpoint 优先，并只保留在 `client.env`，不再复制到 discovery 缓存。没有显式 endpoint
时使用 UDP discovery；首次发现保存服务端 SPKI，之后地址可以变化，但 TLS 身份必须与已保存
pin 一致。以太网优先，Wi-Fi 作为备用；Wi-Fi 页面提供配网入口。

LVGL 负责 320×240 桌面、静态小智状态表情、字幕、实时音量、网络和摄像头页面。UI 刷屏与
触摸不拥有音频状态；点击小智只向 application actor 提交一次用户动作。

## 安全与证据边界

- API Key、TLS 私钥和 Wi-Fi 凭据不得写入日志或提交到仓库。固定教学口令由两端代码内部
  使用，学生不配置，也不能把它当成公网身份认证。
- 默认不持久化 PCM、播放参考或完整对话文本。
- Host 测试只能证明状态机、协议、ABI packing 和脚本控制流，不能证明真实 AEC、远场效果或
  声学稳定性。
- 交叉构建只能证明目标 ABI 和依赖闭包；Mode1 布局、AEC、自激、double-talk、触摸和摄像头
  必须在目标板验证。
- 当前可复现命令和验收边界统一记录在 [Host 与板端验证](../test/host-validation.md)。
