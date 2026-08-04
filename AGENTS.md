# boomPI 仓库开发约定

本文是 boomPI 仓库的根级开发契约，适用于仓库内所有目录。更深层目录如果以后增加自己的 `AGENTS.md`，只能细化本文件，不能绕过这里的产品、安全、实时性和硬件约束。

本文中的“必须”“禁止”“不得”是强约束；“建议”可以在有实测数据或 ADR（Architecture Decision Record）支持时调整。用户最新的明确要求优先于本文，但发生架构变化时必须同步更新本文和相关文档。

## 0. 当前教学版冻结范围（2026-08-03，优先于全文其余路线图）

当前目标不是企业级量产平台，而是一套学生可以读懂、配置 Qwen API Key 后即可运行的
完整教学版本。下列约束覆盖本文后续与之冲突的旧路线图：

- 板端只保留已经跑通的双麦、Rockchip 3A、Snowboy、VAD、WSS、流式 TTS、打断和三秒追问主线。
- 本版补齐最小屏幕状态/字幕/触摸、以太网优先、已保存 Wi-Fi 接入、简单局域网发现和配置持久化。
- Go 服务端提供跨平台单二进制；除 Qwen API Key 外均给出可运行默认值，并提供三步启动说明。
- 首次 Wi-Fi 二维码只负责把 SSID/密码交给板端；不引入账户系统、设备授权或云端控制面。
- 使用现有共享令牌和 SPKI 固定即可；六位码配对、多设备管理、复杂 flow-credit、工具系统、
  独立 C++ supervisor、签名 A/B 更新、掉电事务和 24 小时 soak 均后置。
- 教学版仅保留缓冲区边界、超时、秘密不落日志和硬件安全等最低防线，不为未验证场景增加框架。
- 板端音频产品胶水（composition root、状态机、音频、WSS 和语音配置）设计目标为
  **不超过 2500 ELOC**，CI 硬上限为 **2800 ELOC**；Rockchip/Snowboy vendor ABI
  适配单列且不得超过 **300 ELOC**，合并后的音频主线硬上限为 **3100 ELOC**。LVGL UI
  与网络启动继续独立统计；禁止通过挪文件、压行或删除有价值注释规避统计。
- 新功能先复用 BSP 已验证的设备节点、命令和成熟库；在出现第二种真实后端前，不增加工厂、
  虚接口、插件、通用 worker 或占位模块。

本文如提到六位码配对、独立 supervisor、签名 A/B 更新或完整企业发布闸门，必须明确标为
未来路线；它们不属于当前教学版实现，也不是交付阻断项。

## 1. 项目目标与第一版边界

boomPI 是面向 RV1106 自研板的本地服务型语音 AI 产品：板端运行 C++ 客户端，本地电脑运行跨平台 Go 服务端，服务端默认访问 Qwen 中国内地（北京）区。第一版形态是“小智”式全双工语音聊天机器人。

第一版必须包含：

- 双模拟麦克风采集、扬声器播放、播放参考、AEC、降噪、波束形成、增益管理和 VAD。
- 本地 Snowboy 英文唤醒词；第一版固定使用 Snowboy 引擎，不得静默替换。
- Qwen 语音对话；默认使用实时 ASR、`qwen3.6-flash` 对话和流式 TTS 的组合链路，
  `qwen3.5-omni-plus-realtime` 直连作为可选模式，模型名必须可配置。
- 普通话优先，允许中英文混说；回复默认使用简体中文。
- 任意语音打断 TTS、三秒连续对话、流式字幕和表情状态。
- 以太网和 Wi-Fi；优先以太网，首次 Wi-Fi 使用二维码配网。
- ST7789P3 横屏 UI、GT911 触摸、音量/亮度控制、配网和离线状态。
- 本地自动发现、WSS、服务端公钥 SPKI 固定和教学局域网共享设备令牌。
- Go 服务端单文件发布体验，学生配置环境变量和 `config.yaml` 后即可运行。

第一版明确不包含：

- SC3336 摄像头业务和多模态视频链路。协议可以保留能力字段，但不得把摄像头依赖带入第一版运行路径。
- 在线音乐服务或版权音乐聚合。仅保留未来 `MediaPlayer` 能力边界和 TODO。
- 长期记忆、用户画像或跨会话持久化对话。
- 动态 DLL/so 后端插件、容器部署要求、Windows Service 强制安装。
- 六位码配对、多设备管理、工具调用、独立 supervisor、签名 A/B 更新和自动回滚。
- 通过语音直接执行 Shell、SSH、文件删除、刷机或软件升级。

## 2. 已确认的产品行为

- 英文唤醒词第一版使用 Snowboy 可用模型；唤醒后不播放提示音。
- 唤醒词后可以紧接命令，不要求停顿。唤醒前音频预留 500 ms pre-roll。
- 播放期间的打断必须保留并发送当前连续可用、最多 500 ms 的 AEC 后语音预卷；当前主动
  硬参考探针确认打断后，把该预卷作为新用户 utterance 的开头，不能吃掉句首。
- 唤醒后等待首次说话 6 秒；正常结束说话静音阈值 700 ms；单次用户语音最长 60 秒。
- TTS 结束后进入 3 秒 `FOLLOW_UP`，期间无需再次唤醒。
- 播放期间先以原音量要求连续 6 帧（120 ms）近端候选，随后一次性静音；不再使用 30%
  中间音量。静音后最多等待 15 帧取得连续 3 帧低 reference，再清尾 3 帧（60 ms）、重置
  listener，并以连续 3 帧（60 ms）重新确认近端语音；确认后才清空播放队列并取消云端回答。
  播放取消和新用户 turn 分为两个决定：新 turn 还必须出现有效 VAD start，并满足连续 20 帧
  （400 ms）低 reference，取消确认后的准入窗口最多 500 ms。任一探针阶段失败须恢复播放并
  冷却 15 帧（300 ms）。自然播放结束走另一条路径：
  backend 先抑制 15 帧（300 ms）尾音，follow-up 随后必须连续 20 帧（400 ms）才开始新轮。
  该主动探针是防自激的 containment 候选，不是 AEC 效果结论；取消
  时延及真人双讲灵敏度必须在安静可控环境重新测量。
- 打断后删除当前未播放/未完成的 AI 字幕，并截断服务端和 Qwen 中对应的未播放回答，防止上下文错误。
- 当前会话最多保留 20 轮或约 24K tokens，先到者生效；超过后删除最旧的完整轮次，不自动总结为长期记忆。
- 会话空闲 30 分钟、板端/服务端重启或连接重建后清空上下文。用户说“清空对话”也立即重置。
- 首响应等待 15 秒时给出本地状态提示，30 秒仍无有效响应则取消本轮。
- 默认音量为 60，范围 0–100，修改后持久化。第一版不提供独立静音模式。
- 网络断开时丢弃当前轮次，不缓存并补发过期语音；本地唤醒、UI 和离线提示仍工作。
- 默认女声方向是自然、年轻、不过度卖萌。具体 Qwen voice 必须在真实扬声器上试听至少三个候选后再固化。

## 2.1 当前板端实现边界（2026-08-03，优先于后文路线图）

现役 `boompi-client` 的音频主线包含 15 个生产 C/C++ 文件、3066 ELOC，其中
Rockchip/Snowboy vendor 集成 280 ELOC，音频产品逻辑 2786 ELOC。此次增长主要来自把状态转换、
错误恢复和线程交接从压缩语句展开为可教学阅读的结构，不得为回到设计目标重新压行。LVGL UI 与网络启动不进入
这项音频预算，也不得挪入音频目录规避独立审查。计数口径是非空行减去纯 `//` 注释，完整范围由
`scripts/tests/test_client_source_contract.py` 固定；2026-08-01 的
`docs/test/client-responsibility-layout-20260801.md` 保留为纯语音阶段历史快照。后文关于完整
supervisor、update、target 分层和发布架构的内容是未来路线图，不授权提前恢复占位层。

- 生产目标按仓库既定职责落位：`application/voice_client` 独占会话状态，
  `audio/audio_engine` 管理播放线程和有界 TTS 环，`network/voice_transport` 管理
  持久 WSS/TLS 与会话身份/序号校验，`platform/rv1106/audio_backend` 管理 ALSA、重采样、
  Snowboy/VAD/近讲判定，并只调用一次 Rockchip 3A；`network/network_bootstrap` 只处理链路
  优先级、已保存 Wi-Fi 与服务发现，`ui/device_ui` 只处理当前屏幕和触摸；其余只有环境配置、
  Snowboy C ABI 桥和 composition root。
- `AudioBackend` 是 `AudioEngine` 的私有板级实现，不是 application 可见的第二套接口；
  在出现第二种已验证硬件后端前，不建立工厂、虚基类或通用 backend 层。
- 以上职责目录是当前实现，不是恢复旧的通用框架。不得另建 `App/Driver/Inf` 平行目录，
  也不得用目录重排为理由增加 worker、虚接口、工厂或占位 target。
- 禁止恢复已删除的 `manual_single_turn`、自写 WebSocket/TLS/JSON、重复 wire protocol、
  通用 capture/playback/control/committer/worker、生产侧 host fake 音频和未接入产品的 update
  占位实现。测试目录可保留只实现现有硬件边界的薄 fake，用于确定性驱动真实状态机和队列；
  不得把它链接进 `boompi-client` 或计入板端能力。当前 UI 与配网是实际硬件功能，不是抽象框架。
- WebSocketpp、Boost、OpenSSL、ALSA、libswresample、Rockchip 3A、Snowboy 和 WebRTC VAD
  作为外部实现使用；不得复制它们的功能到自写生产层，也不得把自写业务代码移进
  `third_party/` 规避计数。
- 增加音频产品 C/C++ 前必须先删减或用实测证明必要性：产品胶水以 2500 ELOC 为设计目标、
  CI 硬上限为 2800 ELOC；vendor ABI 单列且不得超过 300 ELOC，音频主线总量不得超过
  3100 ELOC。LVGL、网络启动、辅助运维与
  配网脚本分别计数；未来 supervisor 不得挤入音频预算。

## 3. 硬件基线与事实来源

目标板是 RV1106 Linux/Buildroot 自研板，已完成扬声器、两个动态 PCM slot、以太网、Wi-Fi、ST7789P3、GT911 和 SC3336 的单项 bring-up。第三块板已确认 Mode1 四通道顺序为 `[mic0,mic1,refL,refR]`，但两个 mic slot 尚未完成最终物理左右命名。单项和硬件参考通过不代表量产声学、真人 double-talk 或长时间稳定性已经通过。

硬件相关修改必须遵守：

- 用户最后确认的 `netlist.json` 是当前主板网表基线；更早的 `full_netlist (4).csv` 和 `netlist (5).enet` 视为主板历史资料，不得用于新的引脚或电源结论。`netlist (3).json` 与双 16P 屏幕/转接板资料相关，只能在核明板卡和连接器范围后用于该子系统，不能覆盖主板基线。
- 仓库最终必须用 `docs/hardware/` 下的硬件矩阵、引脚表和验证记录承接网表结论；硬件测试记录使用中文并带采集时间戳、板卡/镜像版本和测试条件。产品代码不得依赖开发机下载目录的绝对路径。
- 修改 GPIO、pinmux、Codec、I2S/TDM、LCD、触摸、Wi-Fi 或设备树前，先对照最新网表、数据手册、当前 DTS 和板端运行结果。
- 涉及刷镜像、改分区、改设备树、持久修改启动项或擦除板端数据时，必须取得用户对该次操作的明确授权。先做只读探测，不因“可能需要”就直接烧录。
- 板端行为以真实 SSH 探测和实测为准；不得根据另一个 RV1106 示例板猜测设备节点、声卡编号或 GPIO。

当前音频开发方向（2026-07-27）：

- 先接通并测量幸狐/Rockchip BSP 已提供的 `rk_mpi_ai`/`rk_mpi_ao`、ALSA PCM、
  Rockchip VQE 和 `libaec_bf_process.so` 的 `rkaudio_preprocess_*`，再决定生产模块边界。不得继续为尚未验证的行为增加
  playback/control/committer/worker 等通用层。
- 2026-07-31 已删除未进入板端客户端的通用 queue、playback control/committer/worker、
  capture/DSP 前端和旧 ALSA adapter；历史验证记录可保留，但不得恢复这些生产抽象来承接
  未经实测的行为，也不得把 host fake、ALSA `null` 或交叉链接写成板端能力。
- 第一项板端闭环是同一时刻运行的 48 kHz capture/playback，其后用可辨识信号确认实际
  通道数、双麦 slot、数字 reference slot、极性、延迟和时钟关系，最后才接 16 kHz 3A。
- 过去文档把“Mode1 四通道”写成一个假设，现已拆开：当前 DTB 的
  `rockchip,clk-trcm=<1>` 只表示 TX/RX 共享 TX LRCK/BCLK 的 TRCM 时钟模式，不证明
  四通道或数字 reference；`rk_mpi_ai_test` 启用 AI VQE 时请求的
  `I2STDM Digital Loopback Mode=Mode2` 又是独立的 mixer loopback 选择。两者都不能替代
  当前镜像的通道相关性 HIL。
- Rockit AI VQE 和直接调用 `libaec_bf_process.so` 是两条候选 vendor 路径。直接 3A 的
  固定帧与 input/output 长度已由匹配 SDK 关闭；物理 packing/slot、错误恢复、依赖和实时率
  仍须实测。两条路径禁止同时叠加，也不得根据库名推断内部算法。
- 允许独立的、显式 opt-in 的板端探针 target 直接调用 vendor API。探针只用于关闭事实
  问题，不需要先经过产品通用接口；测试产物和用户录音不提交仓库。

2026-08-01 当前已验证并约束生产实现：

- `I2STDM Digital Loopback Mode=Mode1` 提供 48 kHz/S16_LE 四通道 capture
  `[mic0,mic1,refL,refR]`；capture period/buffer 为 `960/1920`，双通道 playback 为
  `960/3840`，全双工窗口无 xrun。997 Hz→`refL`、1499 Hz→`refR` 的相关系数均为 `0.9983`。
- 物理扬声器主要使用 DAC-L；数字参考到麦克风的声学到达约 `14–17 ms`，仅为阈值法近似。
  raw mic DC/底噪较高，不得使用 raw RMS 判定近讲。
- direct 3A 固定为 `init(16000,16,2,1)`，输入 768 shorts/1536 bytes，输出 512 bytes；
  当前生产 DSP profile mask 为 `1109`，启用 FastAEC、AES、ANR、Dereverberation 和 STDT，
  禁用 vendor AGC；`model_aec_en=0`，不启用 software delay。`ALC31/ref1/delay0` 是当前候选，
  不是最终声学参数。
- 生产链路只使用 Mode1 硬件参考，不再维护软件 reference ring 或 60 ms lead。公开 ABI 没有
  DTD 事件，`wakeup_status` 不是 DTD；`near_voice` 只使用 3A 后 VAD。首次有效硬参考后保留
  600 ms warm-up。自然播放结束隔离 300 ms 尾音并在末端重置 VAD；主动打断必须保留已经
  确认的近讲 VAD 生命周期，直到产生语音结束事件。
- 同类无人声/嘈杂环境回归中，vendor AGC 开启时 `n=5`：`confirmed=4/5`、`follow=5/5`、
  `attempts=119`；关闭 AGC 后累计 `n=10`：`confirmed=2/10`、`follow=3/10`、`attempts=43`。
  AGC OFF 明显改善误触发，但仍不充分；环境噪声未受控且真人双讲尚未验证，不得据此宣布
  AEC 或打断验收通过。主动硬参考探针期间因故意静音，相关采样也不得计入 AEC 效果评分。
- 以上只关闭通道、ABI 和有限无人工冒烟；最终壳体 ERLE、残余回声、真人 double-talk、
  最大音量和长期实时率仍须验收。证据见
  `docs/test/p0-mode1-hard-reference-validation-20260801.md`。

机械声学基线：

- 两只麦克风在正面横向排列，声学孔中心距 `35 mm ± 0.5 mm`。
- 扬声器声学中心位于双麦中点正下方，垂直中心距推荐 80 mm，最低 60 mm；左右声程差应小于 2 mm。
- 主要拾音目标是设备正前方约 0.5 m 的用户；波束参数必须按最终壳体和实测几何校准。
- 两只麦的孔、网布、声道长度和密封结构必须对称，声道长度差目标小于 0.5 mm。
- 扬声器使用独立密闭音腔和软质减振密封，不与麦克风共享泄漏腔体。
- 如果量产壳体或 PCB 的实测尺寸不同，先记录实际几何尺寸，再调波束形成；不得继续使用名义尺寸假装一致。
- 扬声器和功放不得削顶。AEC 不能可靠消除功放、扬声器或壳体引入的非线性失真。

## 4. 仓库与目录边界

当前仓库结构如下。新代码先进入现有职责目录；没有第二种真实实现前，不新增平行分层、工厂或占位目录：

```text
boomPI/
  AGENTS.md
  README.md
  CMakeLists.txt
  CMakePresets.json
  client/
    apps/
      boompi_client/main.cpp
      boompi_ui_simulator/main.cpp
    include/boompi/{application,audio,config,network,platform/rv1106,ui}/
    src/{application,audio,config,network,platform/rv1106,ui}/
    tests/{fixtures,hil,link,smoke,support,unit}/
    assets/
    cmake/toolchains/rv1106.cmake
    scripts/
  server/
    cmd/boompi-server/main.go
    internal/{app,backend,config,discovery,identity,logging,protocol,session,transport}/
    configs/config.example.yaml
  protocol/
    protocol-v1.md
    fixtures/
  docs/
    architecture/
    hardware/
    test/
  scripts/
  third_party/
```

当前 CMake target 与真实产物对应：

- `boompi_client`：唯一板端产品可执行文件，安装名为 `boompi-client`。
- `boompi_lvgl_ui`、`boompi_snowboy_bridge`：产品目标直接使用的薄集成 target。
- `boompi_ui_simulator`：显式打开桌面预览时构建，不进入板端安装包。
- `boompi_voice_client_config_test`、`boompi_voice_client_harness` 和 Unix 下的
  `boompi_audio_engine_harness`：只在 Host 测试配置中构建。
- `boompi_protocol_json_test`、`boompi_aec_loop_hil` 以及 vendor link/HIL target：显式构建的
  契约或真板探针，不进入默认安装，也不代表硬件效果已经通过。

`main.cpp` 只能作为 composition root：解析三个现役命令、加载配置、保存显式 endpoint、安装信号
处理并调用 `RunVoiceClient`。不得把音频循环、协议解析、业务状态机或测试 WAV 逻辑塞进 `main.cpp`。

当前产品依赖方向是：

```text
main (composition root)
  -> application/voice_client
       -> audio/audio_engine -> platform/rv1106/audio_backend -> ALSA/vendor SDK
       -> network/voice_transport -> WebSocketpp/OpenSSL/cJSON
       -> network/network_bootstrap
       -> ui/device_ui -> LVGL/ST7789P3/GT911
```

底层模块不得反向引用 application。当前严格 JSON/PCM 编解码作为 `VoiceTransport` 的私有实现，
wire schema 由 `protocol/protocol-v1.md` 约束；在出现第二种 transport 前，不为形式分层新增 protocol
目录或公共框架。UI 回调不得直接修改会话状态；板级适配不得包含产品对话逻辑。

## 5. 客户端状态与线程所有权

会话状态只由 application actor 修改；网络流程通过 endpoint/连接事件驱动，不另建可被多线程
共同修改的业务状态机：

```text
ConversationState:
  WAITING_FOR_WAKE -> WAITING_FOR_SPEECH -> CAPTURING_SPEECH
  -> WAITING_FOR_RESPONSE -> PLAYING_RESPONSE -> DRAINING_PLAYBACK
  -> WAITING_FOR_FOLLOW_UP
  interrupt: CANCEL_PENDING -> BARGE_ADMISSION

Network flow:
  NO_LINK -> DISCOVERING -> CONNECTING -> ONLINE -> BACKOFF
```

AP/STA 配网生命周期由 `boompi-clientctl provision` 独占，不塞进语音会话枚举。不要把网络、配网和
对话状态的所有组合展开为一个巨大枚举。

启动顺序必须让显示、音频、Snowboy 和本地状态机先进入可用状态，再由网络 worker 在后台发现/连接服务端。服务端离线或 DNS/路由异常不得阻塞本地启动。

当前正常语音与显示共有六个客户端自有长期执行上下文；打开摄像头后再增加一个：

1. 主 application actor：唯一转换会话状态、创建/取消 turn，并从 `AudioEngine` 取 capture frame。
2. capture/DSP 线程：独占 ALSA capture、解交织、重采样、Rockchip 3A、Snowboy/VAD 和帧连续性。
3. playback 线程：独占 TTS 固定环、重采样、音量/限幅和 ALSA playback。
4. WSS/ASIO 线程：负责 TLS/WebSocket I/O，只通过固定事件环向 actor 交付结果。
5. network bootstrap 线程：负责链路准备、UDP discovery 和 endpoint 持久化，不拥有会话状态。
6. UI 线程：唯一访问 LVGL、ST7789P3 和 GT911，只交换模型快照与触摸动作。

capture/DSP 线程通过 `AudioEngine` 内部 `4 × 20 ms` 有界队列把 AEC 后帧交给 application actor；
actor 取帧后再直接提交 websocketpp 有界发送缓冲，没有独立上行 PCM 网络队列。控制/下行事件走
固定 64 项 `EventQueue`，TTS 下行走 `AudioEngine` 的 75 帧固定环。不得为了形式统一增加
EventBus、通用 worker 或第二套音频队列。

并发规则：

- capture 队列和 TTS 队列都使用对象内固定数组；固定 mutex `EventQueue` 只承载 WSS 控制/下行
  事件，不得扩成全局音频总线。
- 实时录音、DSP 和播放热路径禁止每帧 `new`、`malloc`、`std::vector` 扩容、文件 I/O、网络 I/O 或无界锁等待。
- 音频帧必须带单调时钟时间戳、`sequence`、`stream_id` 和 `epoch`。IDLE/唤醒前的常开 capture frame 可以使用 `turn_id=0`；进入 utterance 或网络传输后必须绑定有效 `turn_id`。重连/取消后旧 epoch 的帧和事件必须丢弃。
- 队列满时不得阻塞录音线程。捕获到帧丢失或序号断裂时标记 discontinuity；会破坏 AEC/语义的情况应取消当前 turn，而不是继续发送伪连续音频。
- 当前停止顺序必须保持显式：actor 停止接受新 turn，关闭 WSS，`AudioEngine::Close` 唤醒并回收
  capture/playback，停止并 join network bootstrap，最后由对象析构关闭 UI 和底层资源。
- 不得用 `sleep` 猜测线程已经启动或退出。使用事件、条件变量、项目自定义 stop flag/cancellation token 等 C++17 可用的明确同步手段。
- 当前 capture/playback 分别申请 `SCHED_FIFO 40/30`，失败时记录一次警告并继续；UI 线程使用
  `nice(5)`。修改这些值前必须在单核 RV1106 上复测 XRUN、队列水位和 UI 饥饿，不能继续抬高优先级掩盖阻塞。

## 6. 音频数据契约

第一版固定 20 ms 音频帧：48 kHz 时每通道 960 个采样，16 kHz 时每通道 320 个采样。协议和缓冲区必须使用显式采样率、通道数、位宽和时长，禁止依赖隐含全局值。

目标链路：

```text
direct ALSA 48 kHz full duplex, Mode1
  -> capture [mic0,mic1,refL,refR], S16_LE, period/buffer 960/1920
  -> phase-aligned deinterleave + anti-alias resample 48 kHz -> 16 kHz
  -> Rockchip 3A adapter once: 2 mic + REF-L (REF-R remains captured but is discarded here)
  -> VAD + Snowboy + 16 kHz S16_LE mono uplink

Qwen 24 kHz S16_LE mono downlink
  -> current fixed 180 ms initial jitter prebuffer (short EOS exception)
  -> resample 24 kHz -> 48 kHz
  -> volume + limiter + speaker mix
  -> ALSA playback
```

强约束：

- direct ALSA 的 Mode1 四通道、双声道播放和 48 kHz 全双工已验证；不得把该结论外推为
  raw rk_mpi 已通过，也不得把数字参考描述成扬声器之后的模拟回采。mic0/mic1 的最终物理左右、
  极性和量产声学仍须在最终壳体中复核。
- Rockchip 3A 当前按仅支持 8/16 kHz 处理。未经头文件、ABI 和板端测试证明，不得让 vendor AEC 直接吃 48 kHz，也不得在文档中宣称支持。
- 当前生产实现固定使用 Mode1 硬件参考。只有后续目标镜像证明硬件参考不可用并完成方案评审后，
  才能重新设计软件参考；不得恢复旧 software reference ring/60 ms lead，也不得直接用收到的
  原始 TTS 包作为 AEC 参考。
- 硬件 Codec 回采和软件播放参考同一时刻只能启用一种，禁止把两种 reference 叠加后交给 AEC。
- AEC、NS、BF 和启用的增益处理每一级只运行一次。当前生产 profile 已禁用 vendor AGC，
  外层不得再无依据叠加自动增益；若后续重新启用任一增益模块，必须先完成同条件 A/B 和声学验收。
- `RockchipVoiceDsp` 只暴露板端实测所需的固定帧输入、输出和重置边界；vendor 实际算法顺序
  由匹配 BSP 的 adapter 负责，不得为统一接口重建未使用的通用 DSP engine 层，也不得把
  推测的内部顺序写成事实。
- 第一版板端到服务端使用 16 kHz、16-bit、mono 原始 PCM，不使用 Opus。播放下行按 provider 实际格式标记，默认 Qwen 24 kHz PCM，板端统一转 48 kHz。
- Snowboy 和上行网络消费同一份 AEC 后 16 kHz 音频。当前在同一 actor 中先完成本地检测，再把
  PCM 交给 websocketpp 有界发送缓冲；只有实测 send 阻塞影响 capture 时才拆成独立有界队列。
- Snowboy 只通过私有 C ABI bridge 隔离旧 libstdc++ ABI，不为单一实现增加 `WakeWordEngine`
  虚接口。接入前检查 ARM ISA、hard/soft float、动态加载器、libc、libstdc++/`GLIBCXX`、模型
  加载和实时率；第二种唤醒引擎真正进入产品后再按实测抽象。功能跑通后仍需至少 30 分钟
  稳定性及 CPU/RSS/最坏帧耗时测试。
- ALSA card/device、通道 map 和 mixer 控件必须来自配置及板端探测，禁止硬编码示例板编号。
- 默认音量 60；所有增益和音量映射必须限幅。检测到播放削顶时优先降低链路增益，不通过提高 AEC 强度掩盖硬件失真。

缓冲和背压：

- 当前没有独立上行 PCM 队列；websocketpp 待发送字节上限按约 800 ms 音频约束，超限取消当前
  turn，不补发陈旧语音。若后续拆出上行队列，其容量仍不得超过 800 ms。
- 当前 TTS 固定等待 9 × 20 ms，即 180 ms 后首播，短 EOS 例外，硬上限 1.5 秒；取得真实
  jitter 分布后再评估 120–400 ms 自适应，不得把路线图写成现状。
- credit/window 尚未实现；落地后服务端无 credit 时暂停下发，provider-to-board 队列也必须
  有界。两端均满时取消 response 并报告拥塞，禁止丢弃中间 PCM 或无界缓存。
- 打断、turn cancel、epoch 变化时立即清空对应 TTS 和字幕队列，不等待正常播放结束。
- 长回答必须边生成边播放，不得把完整回答缓存后才开始播放。
- 下行欠载时使用短淡出/静音并上报 underrun；不得重播旧 PCM 填洞。

## 7. 板端与服务端协议

板端只连接本地 boomPI server，不直接持有或访问 Qwen API Key。默认端口：

- TCP/WSS `17806`
- UDP discovery `17807`

客户端先建立以太网或已保存 Wi-Fi 链路；配置了显式 server IP/SPKI 时直接使用该 endpoint，
否则执行 UDP 自动发现并以已保存的 SPKI 约束回退。网络切换不得在一次有效 turn 中间无故撕毁连接。

协议约束：

- 一条持久 WSS 连接承载 JSON 控制事件和二进制 PCM 帧。
- 控制帧必须使用正式 JSON 解析器，做类型、长度、必填字段、枚举、协议版本和 capability 校验；禁止通过字符串搜索解析 JSON。
- PCM 使用定义清楚的二进制 header，至少含 version、header/payload length、format、sequence、timestamp、device/session/turn/stream/epoch 标识。字段逐个序列化并声明字节序，不得直接发送 packed C++ struct。
- 当前 v1 不上报播放位置；打断或尾播取消时发送精确 `{}`，服务端删除整个未听完的 assistant turn。
- 每类帧和字符串必须有显式最大长度；未知消息返回明确错误或忽略，不得越界或导致状态隐式变化。
- 连接重建采用 1、2、4、8、16、30 秒有界退避。当前 turn 直接失败，旧 turn/epoch 返回包全部丢弃。
- WSS 在无业务流量时默认每 10 秒发送 ping/heartbeat，约 30 秒未收到 pong/有效消息即判定 half-open 并重连；数值可配置但必须有 deadline。
- 音频不做应用层重传；可靠连接恢复后开始新 turn，避免把延迟语音当作实时语音。
- 协议 v1 携带固定版本、严格 payload schema 和 turn/stream/epoch；当前服务端只允许一台活跃设备。

当前教学版安全边界：

- Go 服务端首次启动自动生成本地 TLS 证书和密钥；私钥仅保存在服务端状态目录并限制权限。
- 板端固定服务端公钥 SPKI SHA-256；相同密钥续签证书不破坏连接，密钥变化必须由用户重新配置。
- 教学版使用两端一致的共享设备令牌，只适用于可信局域网；令牌和 SPKI 均不得写入普通日志。
- 普通连接严禁全局关闭证书验证。任何 `SSL_VERIFY_NONE`、无条件信任自签证书或明文音频 WebSocket 都禁止进入生产构建。
- 每块板首次启动生成并持久保存随机 UUID。MAC 地址只用于网络链路和诊断，不得充当 device identity。
- 六位码配对、随机 per-device token 和撤销列表是未来产品路线，不得在当前教学版恢复占位字段或空模块。

## 8. Go 服务端

服务端目标是 Windows、Linux、macOS 可运行的单个 `boompi-server` 可执行文件，优先使用纯 Go 依赖并保持 `CGO_ENABLED=0` 可构建。第一版以前台终端程序运行，不强制安装系统服务。

服务端入口只负责配置、信号和依赖装配，业务放在 `internal/`。每个设备由一个串行 Session Actor 独占状态，WebSocket reader、writer 和 Qwen provider 使用独立 goroutine，通过有界 channel 交互。

后端接口：

- `ConversationBackend`/`ConversationSession` 是服务端唯一 provider 边界；Session Actor 只看统一事件。
- `conversation_mode: intelligence` 是默认链路：实时 ASR 预连，未就绪或失败时有界回退 batch ASR，
  随后流式对话模型与连续 `server_commit` TTS。
- `conversation_mode: realtime` 选择 Omni Realtime 直连；两个实现由 composition root 的明确分支选择，
  不建立 registry、动态插件或第二套 ASR/LLM/TTS 公共接口。
- 生产二进制不得包含 `MockBackend`。测试替身只能位于测试包或 `boompi_test_support`。

Qwen 默认配置：

- region: China (Beijing)，配置值 `china-beijing`
- conversation mode: `intelligence`
- ASR: realtime 预连，batch 模型 `qwen3-asr-flash` 作为回退
- reasoning model: `qwen3.6-flash`，reasoning effort `none`
- TTS: `qwen3-tts-flash-realtime`，voice `Cherry`
- optional direct model: `qwen3.5-omni-plus-realtime`
- interaction: manual turn detection，由板端 VAD commit
- input: 16 kHz S16_LE mono PCM
- output: 24 kHz S16_LE mono PCM
- search: `off`，允许配置为 `auto`
- language: 普通话优先并允许中英文混说

Provider 名称、endpoint、模型、voice 和事件字段可能变化，必须封装在 Qwen adapter 内，并在实现时重新核对官方文档。不要把 Qwen event 直接泄漏为板端协议。

系统提示词和 persona 放在服务端 `config.yaml`。配置变化只对下一次新会话生效，不得在当前会话中途无提示改变人设。

当前教学版不提供工具调用。天气、闹钟、设备控制和其他工具只有在出现真实产品需求、明确 allowlist
和板端执行边界后才能加入；任何工具都不得执行任意命令、SSH、任意文件路径、刷机或升级。

用户在播放或 ALSA 尾播阶段打断时，server 必须先 fence 旧 epoch、取消 provider、阻止旧 delta
继续下发，并删除整个被打断的 assistant turn。当前 v1 没有可靠的文字/音频/已播放位置对齐，
不得把用户未听到的完整回答留进下一轮上下文。

## 9. UI、触摸和配网

显示基线是 ST7789P3 `320x240` 横屏和 GT911 触摸：

- LVGL 提供统一桌面、语音、摄像头、配网和占位应用页；天气、资源、闹钟和 YOLO 未接入时必须明确显示“待接入”，禁止伪造在线数据。
- 语音页显示两行流式字幕以及待机、聆听、思考、说话、尾播、开心、断网和错误状态。
- 点击桌面“小智”进入并唤醒；播放和尾播阶段点击语音主区域会打断。竖滑调音量、横滑控制当前二值背光。
- UI 使用简体中文；协议字段、配置键和开发日志使用英文。
- 所有 LVGL 对象只由 UI 线程访问；application 只提交状态/文本快照并消费有界动作队列。不得为了 UI 引入 Qt 或通用 display 工厂。

Wi-Fi 首次配置流程：

1. 无有效 Wi-Fi 配置且无以太网时，`boompi-clientctl` 在语音客户端启动前进入临时 AP `boomPI-Setup`。
2. 屏幕展示含 AP/配网页地址的二维码。
3. 手机进入本地 captive portal，提交 SSID 和密码。
4. 原子保存配置、关闭 AP、重启语音客户端、连接 Wi-Fi并发现服务端。

运行中的 LVGL Wi-Fi 页只展示同一配网入口和操作说明，不伪造当前 SSID、IP、信号或频段；
真正切换 AP/STA 生命周期仍由 `boompi-clientctl provision` 独占，避免与语音进程抢占 wlan0 和屏幕。
以太网可用时跳过 Wi-Fi 强制配网。SSID、密码、token 和证书私钥不得出现在普通日志或崩溃包中。

错误、断网和音量操作可以使用短本地提示声，但所有提示声都必须进入最终播放参考路径；唤醒本身保持无提示音。

## 10. 配置、日志与隐私

服务端使用人类可编辑的 `config.yaml`。非敏感配置优先级为 CLI -> environment -> YAML -> defaults。
教学部署可在本机私有 `config.yaml` 填写 `qwen_api_key`，也可用优先级更高的
`DASHSCOPE_API_KEY`；专属 Workspace 只从 `DASHSCOPE_WORKSPACE_ID` 读取。配置检查和日志都不得
打印 Key、Workspace ID 或设备令牌，真实 `config.yaml` 不得提交 Git。

客户端的配网结果、音量、亮度、UUID、endpoint、SPKI pin 和 token 使用原子写入的持久配置；POSIX 配置目录权限至少 `0700`、敏感文件至少 `0600`，Windows 服务端使用当前用户最小 ACL。建议复用协议 JSON 解析能力，避免仅为板端配置引入第二套大型解析器。配置损坏时回退到上一个有效副本或安全默认值，不带着半写文件启动。

默认隐私策略：

- 不保存原始麦克风录音、播放参考或 AEC 后语音。
- 对话文本只在当前会话内存中存在，重启、重连或 30 分钟空闲后删除。
- 普通日志只记录状态、耗时、计数、错误码、缓冲水位和脱敏 ID，不记录完整转写、提示词、token、密码或音频。
- 日志不得记录 Authorization header、SSID、MAC、完整 UUID、设备 token、证书私钥或 Wi-Fi 密码；provider 错误对象先经过统一脱敏。
- release 默认关闭 core dump，避免崩溃文件包含 PCM、文本或 secret。
- 调试音频必须由显式开发开关启用，限定时长、目录和容量，权限至少为 `0600` 并在 UI/日志中明显标识；不得提交录音到 Git。
- 主动导出的诊断包默认也不包含录音和完整对话文本。
- 不进行默认遥测或崩溃自动上传。

任何曾暴露在聊天、日志或 Git 历史中的 API Key 都视为失效，必须吊销并重新生成。示例配置只能使用明显占位符。

## 11. 当前部署与未来产品路线

教学版安装为 `/userdata/boompi/bin/boompi-client`、`/usr/sbin/boompi-clientctl` 和
BusyBox 启动脚本。`boompi-clientctl` 只提供有限三次重启、1 MiB 日志轮转、手工二进制替换及
一个 `.bak` 回退；它不是独立 supervisor，也不宣称签名 A/B、掉电事务或 watchdog 健康闭环。

独立 supervisor、签名双槽更新、硬件 watchdog、多设备管理和公网 OTA 都是未来产品路线，
不属于当前教学版。没有真实需求、板端设备节点和故障注入证据前，不得为这些能力恢复占位 target、
空 manifest、通用进程框架或服务端管理通道。完整 BSP/系统镜像仍只由人工烧录；语音模型和工具
不得触发刷机或系统升级。

## 12. 编码规范

客户端使用 C++17；服务端使用 Go 当前项目锁定版本。代码标识符、文件名、配置键、协议字段和日志字段使用英文。中文注释只用于解释硬件时序、单位、声道映射、并发所有权、阈值来源、安全意图和不明显的设计原因，不复述代码。

C++ 规则：

- 公开模块使用成对 `.h/.cpp`，私有 helper 留在实现文件或匿名 namespace。
- 项目是 C++17，线程取消使用显式 stop flag、eventfd 或 condition variable；不得误用 C++20 `std::jthread`/`std::stop_token`。
- 使用 RAII 表达文件描述符、ALSA handle、线程和 socket 生命周期；预期运行错误使用明确 `Status`/error 类型，不用异常替代正常控制流。
- 协议、采样、长度和计数使用固定宽度整数；时间和物理量在名字中带单位，如 `_ms`、`_samples`、`_hz`、`_bytes`。
- 头文件禁止 `using namespace`，避免可变全局状态，依赖通过构造或显式配置注入。
- 检查系统调用、ALSA、文件、解析、网络、线程和内存返回值；所有阻塞操作都有 timeout 或取消路径。
- 不在实时热路径输出逐帧日志。日志库自身不得持有会被音频线程无界等待的锁。
- 供应商 SDK 错误码在 platform adapter 转换，不向 application 泄漏整套 vendor API。

Go 规则：

- goroutine 必须有 owner、退出条件和 `context.Context`；不得启动无法回收的后台 goroutine。
- 一个 WebSocket writer 独占写连接；业务通过有界 channel 发送，避免并发写。
- 错误增加上下文但不包含 secret；取消、超时和 provider 错误应可分类。
- 所有配置字段有校验和安全默认值；启动时提供 `--check-config`，但不得打印 API Key。
- 使用 `gofmt`、`go test ./...` 和 `go vet ./...`。

## 13. CMake、依赖和构建卫生

- 使用 target-based CMake 和 `target_compile_features(... cxx_std_17)`；禁止全局污染 include/link flags。
- 脚手架应提供 `host-debug`、`host-release` 和唯一现役板端入口 `rv1106-candidate`；板端入口固定
  为 Debug + `-O2` 并打开完整语音能力。个人绝对路径只通过环境变量或未跟踪的
  `CMakeUserPresets.json` 注入，仓库 preset 不得包含绝对路径或 secret。
- CMake 内部 target 使用下划线命名；当前唯一板端产品可执行文件通过 `OUTPUT_NAME` 安装为 `boompi-client`。
- 源文件显式列出，不用 `file(GLOB ...)` 隐式收集生产源文件。
- RV1106 vendor 库通过具有明确 include、library、ABI 和依赖的 `IMPORTED` targets 接入，不把 SDK 链接目录设为 `PUBLIC`。
- 不回退查找相邻私人仓库或绝对路径。工具链和 SDK 位置通过 cache variable/environment 明确传入。
- 默认构建不得联网下载依赖。小型开源依赖可以锁定版本并保留许可证；大型/供应商二进制只记录来源、校验和和接入方式，不随意提交。
- 仓库本身的开源许可证尚未由用户确定，不得擅自新增许可证声明。Snowboy runtime/model 和 Rockchip 二进制在确认再分发许可前不得提交；外部安装必须记录来源和 SHA-256。
- 第三方代码关闭项目级 `-Werror`，自有代码在 host CI 使用严格警告。ASan/UBSan/TSan 仅用于 host，不用于 RV1106 发布包。
- 禁止提交 `build*`、`CMakeFiles`、`.o/.obj/.elf/.bin/.map`、日志、录音、模型缓存、SDK dump、证书私钥和生成的 `.exe`。

脚手架完成后的标准验证入口应优先保持为：

```text
cmake --preset host-debug
cmake --build --preset host-debug --parallel
ctest --preset host-debug --output-on-failure

cmake --preset rv1106-candidate
cmake --build --preset rv1106-candidate --parallel

cd server
go test ./...
go vet ./...
go build -trimpath -o <OUTPUT_DIR>/boompi-server[.exe] ./cmd/boompi-server
```

如果实际 RV1106 SDK 对 CMake 版本或 sysroot 有限制，先记录实测约束再调整命令，不伪造“已构建成功”。

## 14. 测试策略与验收

### P1 快速迭代规则（2026-07-28 用户指令）

当前第一优先级是尽快得到可在 RV1106 上直接运行的纵向语音闭环。下方完整测试矩阵是
里程碑合并与发布验收要求，不再要求每个小提交全部执行。日常开发默认只做：

1. 编译本次修改的 target；
2. 执行一个直接覆盖本次主路径的 happy-path 单测或 smoke；
3. 涉及板端 I/O 时执行一次有界真机 smoke，并记录退出码和最小必要现象。

日常提交暂缓新增穷举边界测试、重复 fake、race/sanitizer、压力测试、长稳测试和非必要
证明型文档；这些工作集中到里程碑合并前或发布前执行。只有内存越界、协议长度、TLS/密钥、
线程停止、硬件写入和不可逆操作等高风险路径仍须随实现保留最小防护。不得用“加测试”为由
继续扩展尚未进入产品运行时的底层抽象。

每完成一个可运行纵向里程碑，再统一运行一次相关 Host suite 和一轮板端 smoke。测试失败只
阻塞受影响路径，不扩大成全仓重构。原始录音、秘密、构建产物和危险硬件操作的安全约束不变。

生产代码不提供 Mock 后端，但 `client/tests/support` 和 Go `_test.go` 可以使用只为测试编译的 deterministic fake。测试替身不得进入发布 target。

当前 Host 自动测试覆盖：

- C++ 客户端配置解析，以及显式 endpoint、捕获序号空洞、drain 打断、取消乱序、背压、重连、
  最长输入和首响应计时等 application 状态机场景。
- `AudioEngine` 的短尾包、启动缓冲、抖动、序号空洞、listener reset 和有界停止。
- C++ 严格 JSON/PCM 契约；Go 侧协议、session actor、有界队列、provider 取消、连续 TTS、
  transport、discovery、TLS 与配置测试；Go 协议测试读取共享 fixture。
- Python 源码职责/ELOC 契约、共享协议 fixture 结构、vendor CMake、ELF 和只读探针脚本测试。
- 默认测试不访问真实 Qwen 或消耗付费额度；live provider 测试必须显式开启并从环境变量读取凭据。

板端 smoke/HIL 必须按顺序执行：

1. 确认 CPU ABI、libc、sysroot、ALSA、TLS 和运行库版本。
2. 验证 48 kHz 全双工及所有实际声卡/mixer 参数。
3. 验证 TRCM 时钟下的真实 capture 通道数、loopback mixer mode、DAC reference slot 和
   双麦极性；没有四通道或硬件 reference 时记录证据并进入软件参考方案评审。
4. 验证 Rockchip 3A 头文件/ABI、双麦输入、16 kHz 实时率和错误路径。
5. 独立验证 Snowboy 动态库、模型加载、唤醒率和 CPU 占用。
6. 验证播放、录音、AEC 后录音，再验证唤醒、VAD、三秒追问和播放中打断；功能通过前不进行长时间压力测试。
7. 验证以太网、已保存 Wi-Fi、AP 配网、UDP discovery、WSS/SPKI/共享令牌和断网恢复。
8. 验证横屏刷新、触摸坐标、二值背光、语音/配网/摄像头页面和各状态表情。
9. 最后记录有界稳定性窗口、日志轮转、XRUN/overrun/core、CPU/RSS 和退出耗时。

必须记录分段延迟：唤醒、VAD 结束、ASR 首字、LLM 首 token、TTS 首包、扬声器首声和打断静音。除已确定的打断目标外，云端首响先建立实测基线，不提前编造 SLA。

AEC/BF 验收必须在最终壳体、双麦 35 mm 和扬声器安装完成后进行。保存测试条件、距离、角度、扬声器音量、背景噪声、ERLE/残余回声及 double-talk 主观结果；没有测量数据时只写“未验证”。

## 15. 开发顺序

现阶段双麦、单硬件参考、Rockchip 3A、Snowboy、VAD、WSS、Qwen、流式 TTS、打断、
三秒追问、LVGL/触摸、摄像头预览和首次配网均已有纵向实现。后续顺序固定为：

1. 先修复确定性状态机、取消、背压、停止和协议问题，并用 host harness 锁住回归。
2. 执行唯一 `rv1106-candidate` 严格交叉构建，确认 ELF/ABI 和依赖 pin；部署后保持客户端关闭。
3. 人工在同音量条件验收唤醒、VAD 句首、首响、连续播放、尾播打断、真人 double-talk 和三秒追问。
4. 再采集分段延迟、XRUN/overrun/core、CPU/RSS 和最终壳体声学数据，按证据微调参数。
5. 最后接入真实天气/资源数据和 YOLO；SC3336 预览通过不等于视觉 AI 已完成。

每步只修改最小负责层。声学参数必须等人工 HIL，不得用 host fake 宣称通过；未来 pairing、工具、
supervisor 和 OTA 不进入当前阻断路径。

## 16. Agent 工作规则与完成标准

开始任务前：

- 确认当前目录是独立 boomPI 仓库，而不是外层 `rk` 仓库。
- 阅读相关 README、设计文档、最新硬件记录和当前 `git status`；保留用户已有修改。
- 涉及板端时先做只读探测，确认设备在线和目标路径；不在日志中输出 SSH 私钥、Wi-Fi 密码、API Key 或 token。
- 对模型名、API 事件、SDK/库版本等会变化的事实，使用官方文档和实际环境复核。
- 如果 P0 假设失败，报告证据并更新设计，不通过硬编码、关闭 TLS 校验或跳过测试来“跑通”。

修改代码时：

- 改最小负责层；同步更新头文件、CMake/Go module、配置示例、协议文档和测试。
- 所有 wire/ring buffer/显示坐标操作都有边界检查；所有 timeout、单位和 overflow 策略显式命名。
- 新依赖必须说明用途、版本、许可证、体积和 RV1106 兼容性。
- 不提交秘密、用户录音、构建产物或未经许可的 vendor/model 二进制。
- Agent 分支使用 `codex/<topic>`；禁止 force push、改写用户历史或覆盖无关改动。
- 除非用户明确要求，完成文件修改和验证后不得自行 commit 或 push。

任务完成必须同时满足：

- 相关 host build/test 通过，或者明确说明缺失工具链和可执行的人工验证路径。
- 涉及 RV1106 行为时提供真实板端验证结果；host 测试不能冒充硬件验证。
- 新线程、队列、配置字段、协议字段和状态转换均有 ownership、边界、timeout 和错误路径。
- 文档与实现一致，日志不泄密，Git diff 不包含无关文件。
- 最终报告列出修改文件、验证命令、未验证硬件假设、已知限制以及是否 commit/push。
