# 音频后端契约与依赖闸门

## 本阶段状态

P2e-a 只建立 host 可验证的 `AudioDspEngine`、`WakeWordEngine` 接口、失败关闭实现、
测试 target 专用的确定性 fake，以及外部依赖的 CMake 配置闸门。仓库中还没有
Rockchip 3A adapter、Snowboy adapter、wake/VAD worker 或真实模型加载代码，也没有
调用板端 vendor API。接口、fake 和依赖文件已纳入 host 构建与验证范围，只能证明
类型、状态和失败路径可测试，不能证明 AEC、波束形成、唤醒或实时率已经可用。

真实状态必须区分为三层：

1. **核心契约已实现**：固定帧、generation、错误分类和 fail-closed 行为。
2. **依赖候选可配置**：只有显式启用并通过路径与 SHA-256 检查时才创建 imported target。
3. **真实 adapter 与 HIL 未完成**：尚未把任何 vendor 函数接入运行时，也未在板端处理音频。

## 数据流与帧布局

```text
CaptureFrame: 48 kHz / 20 ms / 4ch interleaved
  -> CaptureDspFrontend
  -> DspCaptureFrame16k: 4 x 320 planar
       [MIC-L, MIC-R, REF-L, REF-R]
  -> AudioDspEngine
  -> Mono16kFrame: 16 kHz / 20 ms / mono / 320 samples
       +-> wake/VAD 专用有界队列
       `-> uplink 专用有界队列
```

该目标数据路径要求输入为四通道；通用 `CaptureFrame` 类型仍允许承载两通道帧。

`DspCaptureFrame16k` 的四个平面只定义逻辑角色，不声明真实 Mode1 slot 顺序。物理
slot 与极性仍由板端实测后通过 `ChannelMap` 配置。`Mono16kFrame` 是未来真实 3A 的
目标输出；当前 unavailable engine 不会把某一路麦克风直接伪装成 AEC 后 mono。

## AudioDspEngine 契约

`AudioDspEngineConfig` 必须显式选择一种 reference source 和一种 layout：

- `kHardwareCapture`：reference 来自经 HIL 确认的 Codec/I2S/TDM 数字回采平面。
- `kSoftwarePlayback`：reference 来自最终播放链路中已实际提交的数字 PCM。
- layout 只能是 mono-left、mono-right 或 stereo，具体选择必须来自真实 API 和硬件证据。

同一时刻只能启用一种 reference source。软件 reference 不能使用收到的原始 TTS 包，
必须位于 jitter、重采样、音量、duck、混音和 limiter 之后；partial ALSA write 只能
贡献已接受的 prefix，不能补零伪装成完整帧。

v1 配置要求一次性启用完整的 AEC、NS、BF 和 AGC feature mask。这一约束只防止外层
遗漏或重复叠加处理，不代表已经知道 Rockchip 库内部的算法顺序，也不证明四项能力
能用当前候选库同时工作。

engine 由一个 DSP worker 独占并串行执行：

- `Configure` 只用于冷路径，失败不得形成半配置状态。
- `Arm(epoch, stream_id)` 要求非零 epoch 严格大于该实例曾接受的全部 epoch，且
  `stream_id` 非零；成功后清除 backend 历史。epoch 耗尽时必须重建 engine。
- 相同或更旧 epoch 即使更换 stream 也不能通过重复 `Arm` 清除 fault。
- `Disarm` 使历史失效；旧 epoch/stream 帧必须在调用 backend 前被丢弃。
- `Process` 只接受四平面 16 kHz 输入，成功时产生一个 metadata 原样继承的
  `Mono16kFrame`。
- 非成功返回时输出 header 无效，调用方不得读取 PCM。
- discontinuity 或 backend failure 会要求 application actor 换新 generation，不能
  继续发送伪连续音频。

`UnavailableAudioDspEngine` 是默认失败关闭实现：配置和 Arm 明确返回不支持，处理时
从不输出未经处理的麦克风 PCM。`FakeAudioDspEngine` 只编入测试 target；它用于验证
编排、generation 和错误恢复，不实现 AEC/NS/BF/AGC，也不得进入发布产物。

## Rockchip 3A 未决 API

P0 仅证明一组头文件/库是目标 ABI 候选。本阶段没有足够证据回答下列问题，因此实现
不得根据库名、示例板或函数直觉猜测：

- `input_size` 的单位是 bytes、单通道 samples、总 samples 还是固定帧数。
- 输入是 planar 还是 interleaved，麦克风/reference 的 packing 与顺序是什么。
- mono/stereo reference 如何表达，左右 reference 是否都被真实算法消费。
- 返回值是成功码、输出长度、检测状态还是负错误码；错误后是否必须重新初始化。
- 初始化结构、配置文件、模型资源的 ownership、寿命、线程限制和释放顺序。
- AEC、NS、BF、AGC 是否组合在一个入口、内部顺序、是否允许分别关闭。
- 16 kHz 帧长、算法延迟、reset 语义、CPU/RSS 和单帧最坏耗时。

进入真实 adapter 前必须以匹配 BSP 的头文件、可链接 smoke、脱敏输入和板端返回值记录
逐项关闭这些问题。没有证据时状态只能写“未验证”。

## WakeWordEngine 契约

`WakeWordEngine` 固定消费完整的 16 kHz、20 ms、mono、S16_LE、320-sample
`Mono16kFrame`。`Process` 返回无字符串的 POD 结果：decision、规范化 error、
一基 keyword index，以及可选的 `score_milli`。

- `score_milli` 只有在 `score_available=true` 时有效，范围为 0–1000。
- Snowboy API 不提供置信度，因此未来 Snowboy adapter 必须返回
  `score_available=false`、`score_milli=0`；sensitivity 不是检测分数。
- Snowboy `RunDetection()` 的 `-2` 只映射为 diagnostic `kSilence`，不能作为产品 VAD。
- `-1`、异常、未知负值或越界 keyword index 都必须转换为 backend error 并失败关闭。
- `Reset(kVadSegmentEnded)` 只重置 detector 的 segment 状态；它不替代 generation gate。
- reset 结果只有 `reset=true/error=None` 或 `reset=false/error!=None` 两种一致状态。

generation、sequence/timestamp 连续性、500 ms pre-roll 和产品 VAD 都属于单独的
wake/VAD worker，而不是 engine：

- worker 独占 engine、自己的 `FrameContinuityGate` 和输入 SPSC consumer。
- 旧 epoch/stream 帧不进入 detector、VAD 或 pre-roll。
- discontinuity、丢帧或 backend error 清除 pre-roll、锁存 fault，并要求新 generation。
- 500 ms pre-roll 是 AEC 后 mono 的 25 个 20 ms 固定帧；不能持久化原始录音。
- 播放期间 80 ms duck、160 ms 打断确认和 700 ms 尾静音由独立产品 VAD 实现，
  不能使用 Snowboy silence 或 wake detection 代替。

`UnavailableWakeWordEngine` 对合法输入明确返回 backend unavailable，对非法格式返回
invalid frame，永远不报告 silence、activity 或 detection。`FakeWakeWordEngine` 只在
测试 target 中脚本化确定性结果和 reset 错误，不加载模型，也不是生产 fallback。

## 外部依赖闸门

`BOOMPI_ENABLE_ROCKCHIP_3A` 和 `BOOMPI_ENABLE_SNOWBOY` 默认均为 `OFF`。默认 host、
CI 和 RV1106 核心构建不读取私人 SDK 路径，也不会下载或链接 vendor 二进制。仅把
`BOOMPI_TARGET_RV1106` 设为 `ON` 不足以越过闸门；CMake 还会核对真实 cross compile、
Linux/ARM target、固定 RV1106 GNU compiler 和包含 uClibc loader 的显式 sysroot。

显式启用时必须提供绝对路径：

- Rockchip：`BOOMPI_ROCKCHIP_3A_INCLUDE_DIR`、
  `BOOMPI_ROCKCHIP_3A_AEC_LIBRARY`、`BOOMPI_ROCKCHIP_3A_COMMON_LIBRARY`、
  `BOOMPI_ROCKCHIP_3A_DETECT_LIBRARY`、`BOOMPI_ROCKCHIP_3A_CONFIG_FILE`。
- Snowboy：`BOOMPI_SNOWBOY_INCLUDE_DIR`、`BOOMPI_SNOWBOY_LIBRARY`、
  `BOOMPI_SNOWBOY_RESOURCE_FILE`、`BOOMPI_SNOWBOY_MODEL_FILE` 和
  `BOOMPI_OPENBLAS_LIBRARY`。

CMake 对每个输入执行存在性、文件类型和固定 SHA-256 检查，任一缺失或不匹配立即
停止 configure。固定值来自 [P0 可行性报告](../test/p0-feasibility-report-20260725.md)
中的审计基线，但它们只是可行性候选，不是发布批准。必须显式设置
`BOOMPI_ALLOW_FEASIBILITY_AUDIO_VENDOR_INPUTS=ON`，并把生成器限制为 Debug-only，
才会创建供私有 link probe 使用的 imported targets；Release/RelWithDebInfo/MinSizeRel
和包含其他 configuration 的多配置生成器都会被拒绝。通过该闸门不等于 adapter 已经
实现、模型可以加载或板端实时率已经通过。

当前 OpenBLAS archive 含多线程实现，只完成了 ABI/link 候选验证。发布接入前必须按
单线程配置重建、记录来源与许可、生成新的 SHA-256，并替换本模块 pin；不得用
feasibility opt-in 绕过发布审查。

Snowboy、OpenBLAS、Rockchip 库、资源和模型继续保留在仓库外；来源、版本、许可、
SHA-256 和再分发范围全部确认前禁止提交二进制。显式路径不得写入 preset、安装包日志
或 target 的 PUBLIC 接口。

Snowboy 候选静态库使用旧 libstdc++ 字符串 ABI。未来只能由一个私有 C bridge TU
包含 `snowboy-detect.h` 并私有设置 `_GLIBCXX_USE_CXX11_ABI=0`；bridge 外只传固定宽度
整数、PCM 指针、长度和不透明 handle，不传 `std::string`、异常、RTTI 或 Snowboy
对象。所有异常必须在 bridge 内转换为错误码，旧 ABI 定义不得扩散到 `boompi_audio_core`
或应用。v1 不使用动态插件，也不因兼容失败擅自改成多进程或替换唤醒引擎。

## 后续验证顺序

1. 用匹配 BSP 的 Rockchip 头文件关闭 `input_size`、packing、return 和 reset 等 API
   问题，再实现独立 adapter 与错误转换。
2. 在板端验证 16 kHz 双麦/reference 输入、mono 输出、算法延迟、CPU/RSS 和实时率。
3. 实现私有 Snowboy legacy bridge、启动期模型/格式校验和单线程 wake worker。
4. 验证目标英文模型的加载、准确率、误唤醒、漏唤醒和最坏帧耗时，再进行至少
   30 分钟稳定性测试。
5. 最后接入 500 ms pre-roll、独立 VAD 和播放打断闭环；功能通过前不进行压力测试。

真实 adapter 或 HIL 记录完成前，README、UI capability 和测试报告都不得写“DSP 已接通”
“Snowboy 可用”或“AEC/唤醒已通过”。
