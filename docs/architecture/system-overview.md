# boomPI 系统架构摘要

## 文档状态

当前教学候选的语音核心包含 18 个生产 C/C++ 文件、2643 ELOC，其中 Rockchip/Snowboy vendor 集成
280 ELOC，产品逻辑 2363 ELOC；UI 显示层（LVGL 渲染与 ST7789P3/GT911 驱动）另计 1926 ELOC，不占语音核心预算。语音人工体验、严格交叉构建和板端启动已通过；屏幕/触摸、
网络启动、UDP 发现和二维码配网已进入同一候选。受控双讲、量化延迟、真实 Wi-Fi 配网和长期
稳定性仍待完成。
实现状态以测试和真实板端记录为准；根目录 `AGENTS.md` 是更高优先级的开发契约。

## 部署边界

```text
RV1106 device                                      Local host

ALSA capture -> [private AudioBackend] -> AudioEngine -> VoiceClient
        ^                  |                 |             |
        |              3A/VAD/AEC       fixed queues   VoiceTransport
ALSA playback -> Mode1 hard ref -> AudioBackend             |
                                                      boompi-server -> Qwen Singapore

GT911 -> DeviceUi <- VoiceClient -> ST7789P3
Ethernet/Wi-Fi -> NetworkBootstrap -> discovered WSS endpoint
```

- RV1106 当前客户端负责实时音频、唤醒、VAD、打断、本地语音状态机、显示、触摸和网络启动。
- Go 服务端负责设备 session、Qwen adapter、当前会话上下文、TLS 身份和 UDP 发现。
- 板端不保存 Qwen Key，也不直接连接 Qwen。
- 企业配对、守护进程和签名更新属于后续产品阶段，不进入当前教学版 C++。

## 客户端所有权

源码按仓库既定职责放置：

| 目录 | 当前职责 |
| --- | --- |
| `client/src/application/` | 唯一会话 actor、pre-roll、turn/epoch、超时和打断状态转换 |
| `client/src/audio/` | 唯一公开音频门面、播放线程、TTS 固定环和播放结果 |
| `client/src/network/` | WSS/TLS、v1 收发、以太网/Wi-Fi 优先级、UDP 发现与 endpoint 持久化 |
| `client/src/platform/rv1106/` | `AudioEngine` 私有的 ALSA、重采样、Rockchip 3A、Snowboy C ABI、VAD 和近讲门控 |
| `client/src/ui/` | ST7789P3 刷新、GT911 触摸、表情、两行字幕、音量和亮度手势 |

不存在另一套 `App/Driver/Inf` 平行目录，也没有恢复已删除的通用 worker 或 backend 工厂。

当前源码创建五个工作线程，加上主线程（唯一会话 actor），共六个客户端自有长期执行上下文。
板端实测总线程数（含链接依赖内部线程）尚需重新采集；依赖库线程不拥有业务状态：

| 执行上下文 | 独占资源与职责 |
| --- | --- |
| 主线程（会话 actor） | 帧循环、状态机、turn/epoch、500 ms pre-roll、重连调度；业务状态唯一写者 |
| 采集线程 | ALSA capture、DSP、Snowboy/VAD；成帧推入固定 4 槽 capture 环 |
| 播放线程 | 固定 TTS 队列、重采样、gain/饱和钳位和 ALSA playback |
| WebSocket service 线程 | TLS/WSS I/O；回调只向固定 64 项事件环提交结果 |
| UI 线程 | 合并状态刷新、SPI 写屏和 GT911 触摸；只把用户动作交回主状态机 |
| 网络启动线程 | DHCP/Wi-Fi 启动、UDP 发现与 endpoint 持久化；服务端晚启动时持续重试 |

业务状态只能由主线程（会话 actor）修改。实时 PCM 使用对象内固定数组；采集帧、播放 PCM 与网络事件通过
预分配有界环传递，不允许逐帧动态分配或无界等待。

有界队列、discontinuity barrier 和 consumer handoff 的具体 ownership 见
[音频运行时边界](audio-runtime.md)；Rockchip 3A/Snowboy 与 vendor 依赖闸门见
[音频后端契约与依赖闸门](audio-backends.md)。

## 音频边界

`AudioEngine` 是 application 唯一可见的音频接口；其私有 `AudioBackend` 直接打开
48 kHz/S16_LE Mode1 四通道 capture `[mic0,mic1,refL,refR]`
和双通道 playback。20 ms capture 保持四通道相位完成 48→16 kHz；
`RockchipVoiceDsp` 在 vendor 输入边界固定选择 `[mic0,mic1,refL]`，丢弃重复的 `refR`，
其 mono 输出供 Snowboy、VAD 和 16 kHz 上行。
24 kHz mono TTS 进入 75 帧/1.5 s 固定队列，经 24→48 kHz、gain/饱和钳位后播放；最终写入
ALSA 的 PCM 由 Codec Mode1 数字回采到同帧 `refL/refR`，不再维护 software reference ring 或
60 ms lead。采集帧经固定 4 槽（80 ms）capture 环交给 actor，落后超限显式断帧；采集在网络连接、
云端等待和播放期间持续运行。xrun 或四通道
重采样错位会建立 discontinuity 并重置前端历史。

direct 3A 固定为 `init(16000,16,2,1)`、768-short 输入和 512-byte mono 输出；项目 profile
使用 mask `1109`，启用 FastAEC、AES、ANR、Dereverberation 和 STDT，vendor AGC 关闭；
STDT 为 `0.70/0.50`、`model_aec_en=0`。`ALC31/ref1/delay0` 是当前候选参数，不是最终声学
定案。供应商公开 ABI 没有 DTD 事件，
`wakeup_status` 不是 DTD；因此 `near_voice` 只使用 3A 后 VAD。首次有效硬参考后的
600 ms warm-up 和自然结束后的 300 ms 尾音窗禁止触发；主动打断则保留近讲 VAD 历史。

最新同类无人声/嘈杂环境 A/B 为：AGC ON `n=5`，`confirmed=4/5`、`follow=5/5`、
`attempts=119`；AGC OFF 累计 `n=10`，`confirmed=2/10`、`follow=3/10`、`attempts=43`。
关闭 AGC 明显降低误触发但仍不充分；当前环境噪声未受控，真人 double-talk 尚未验证。

2026-07-31 已删除从未进入真实客户端 ELF 的通用 capture/DSP frontend、playback
control/committer/worker、软件 ledger 和旧 ALSA adapter。当前取消直接由会话状态机执行：
确认近端语音后 `DropPlayback`、清空固定 TTS 队列、发送云端 cancel，并把预卷作为下一轮
utterance 开头。不得为了恢复历史测试结构重新引入这些抽象。

下列事项仍需目标板验收：

- 两个 capture slot 的物理左右、极性和最终壳体下的双麦声学表现。
- 最终壳体下硬件 reference 的残余回声、double-talk 与最大音量表现。
- Rockchip 3A 的算法实时率和错误恢复；raw MPI 仍只是独立 HIL 候选。
- 正常 TTS 结束后 3 s follow-up 在嘈杂环境中的残余误触发问题。

当前 DTB 的 `TRCM clk-trcm=1` 只表示共享 TX 时钟，不能独自证明四通道。第三块板的正交信号
HIL 已确认 Mode1 顺序和 refL/refR 相关性；该结论不能外推到其他 BSP。详细证据见
[P0 Mode1 硬件播放参考验证记录](../test/p0-mode1-hard-reference-validation-20260801.md)。

## 状态与取消

会话状态和网络状态相互正交。application actor 创建 `turn_id` 和 `epoch`；网络重连、取消或打断后递增 generation，所有迟到帧必须被丢弃。

播放期间先以原音量连续确认 6 帧（120 ms）近端候选，再一次性静音；不使用中间音量。
随后最多等待 15 帧取得连续 3 帧低 reference，清尾 3 帧（60 ms）、重置 listener，并以
连续 3 帧（60 ms）二次确认近端语音。成功后才清空本地 TTS 并请求服务端取消；新 turn
另需有效 VAD start 和连续 20 帧（400 ms）低 reference，取消确认后的窗口上限为 500 ms。
失败恢复播放并冷却 15 帧（300 ms）。自然播放结束时，backend 独立抑制 15 帧（300 ms）尾音，
随后 follow-up 以连续 20 帧（400 ms）确认新一轮近讲。该主动硬参考探针是
防自激的 containment 候选，不证明 AEC 已通过；其故意静音区间不得用于 AEC
效果评分。新 utterance 携带当前连续可用、最多 500 ms 的 AEC 后 pre-roll；字幕按 response
generation 删除。服务端若不能可靠把文本映射到实际播放位置，应删除整个
被打断 assistant turn，不能把未播放全文留入上下文。

## 网络与信任

显式配置的 endpoint 不发起发现；未显式配置时广播固定 UDP 请求。响应主机只取 UDP 来源 IP；
首次成功发现采用 TOFU 保存 SPKI，已有缓存时只接受相同 SPKI 的新地址，否则回退缓存地址。
后续 TLS 握手仍必须匹配该 SPKI。客户端 `hello` 还携带教学版共享令牌，服务端在打开付费
provider 会话前验证。此方案仅适合可信局域网，不声称提供企业级设备身份或抗恶意局域网攻击。

WSS 承载 JSON 控制帧和二进制 PCM；心跳由服务端每 10 s 驱动，客户端自动回 pong。
音频、Snowboy 和 UI 在网络工作线程之前打开；无缓存 endpoint 或服务端晚启动时，板端保持离线状态并继续发现，
不再因三次失败停机。协议细节见 `protocol/protocol-v1.md`。断线后当前 turn 失败，实时语音不做应用层重传。

## 守护与更新

教学版使用 `/usr/sbin/boompi-clientctl` 启停前台客户端：异常退出后总计最多启动三次，手工更新只保留
一个 `.bak`。它不是常驻 C++ supervisor，也不实现 A/B OTA、远程刷机或签名发布；BSP、内核
和系统镜像仍由人工烧录。

## 隐私边界

- API Key 只存在于服务端私有 `config.yaml` 或进程环境，不下发到板端。
- 默认不持久化 PCM、播放参考或完整会话文本。
- 日志只记录脱敏状态、耗时、计数、错误码和 buffer 水位。
- 诊断包不默认包含录音、完整对话、token、证书私钥或 Wi-Fi 凭据。
