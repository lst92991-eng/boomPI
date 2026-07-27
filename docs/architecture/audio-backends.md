# 音频后端契约与依赖闸门

## 本阶段状态

P2e-a 建立了 host 可验证的 `AudioDspEngine`、`WakeWordEngine` 接口、失败关闭实现、
测试 target 专用的确定性 fake，以及外部依赖的 CMake 配置闸门。后续已增加
allocation-free 的 `AudioDspFrameBridge16k`、portable `VadUtteranceController`，以及
Debug-only Rockchip 3A 和 Snowboy feasibility adapter/probe。仓库中仍没有真实 VAD
detector、wake/VAD worker 或可用于发布的 Snowboy runtime；产品 runtime 也没有调用
这些 vendor API。当前代码与证据只能
证明帧整形、适配器结果映射和受控板端正向探针，不能证明 AEC、波束形成、产品唤醒
或持续实时率已经可用。

真实状态必须区分为三层：

1. **核心契约已实现**：固定帧、generation、错误分类和 fail-closed 行为。
2. **依赖候选可配置**：只有显式启用并通过路径与 SHA-256 检查时才创建 imported target。
3. **feasibility adapter 已隔离**：Rockchip adapter 的零输入 ABI/换代探针已执行；
   Snowboy 正向离线探针已执行，但异常安全不满足产品要求。两者均未接产品 worker，
   完整 HIL 尚未完成。

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
- `kSoftwarePlayback`：reference 来自最终播放链路中已被 playback sink 接受的数字 PCM。
- layout 只能是 mono-left、mono-right 或 stereo，具体选择必须来自真实 API 和硬件证据。

同一时刻只能启用一种 reference source。软件 reference 不能使用收到的原始 TTS 包，
必须位于 jitter、重采样、音量、duck、混音和 limiter 之后；partial device write 只能
贡献已接受的 prefix，不能补零伪装成完整帧。P2f-b-a 的 `AcceptedRenderQueue` 只提供
portable accepted ledger；RV1106 ALSA adapter 已在独立静音 smoke 中证明设备可同时
推进 capture/playback，但迁移后的 committer 组合尚未板端重跑，ledger 也未接到 AEC
consumer。accepted 不等于 presented、played 或 audible，预计 presentation 时间不能
冒充硬件完成证据。
adapter 若返回 malformed positive count，committer 会把请求范围内可能已接受的样本
保守推进且不重放，但禁止用不可信 timing 发布 reference，并要求取消。control result
的零初始化值必须是 invalid/unset；accepted sequence 不得回绕，耗尽后即使完成
`Drop`/`Prepare` 也只能请求 terminal runtime restart。

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

## Rockchip 3A 已核对边界与阻断

2026-07-27 已用匹配 BSP commit `994243753789` 的固定头文件/共享库完成 Debug-only
交叉链接，并在目标板 `/tmp` 运行两轮纯内存全零输入探针。板端三库 SHA-256 与 P0 pin
一致；探针 ELF 为 ARM EABI5 hard-float/uClibc，无 `RPATH/RUNPATH`。详细条件、命令、
脱敏结果和限制见 [Rockchip 3A 离线 ABI 探针记录](../test/rockchip-3a-offline-probe-20260727.md)。

匹配头文件、vendor 指南、导出符号、反汇编和板端返回值已经关闭以下软件 ABI：

- 一次调用固定处理 16 ms；16 kHz 时每通道 256 samples。
- 输入为 interleaved S16。`input_size` 是所有 source/reference 通道合计的 `int16`
  sample 数；`2 mic + 2 ref` 时必须为 1024，1023 返回 0。
- 成功返回 mono 输出 byte 数；16 kHz/16 ms 返回 512。platform adapter 必须先把 byte
  数转换为 sample 数，再让 bridge 要求精确返回值，
  不能把所有非负值都解释为成功。
- mic/reference 的逻辑前后位置由 `pos`/`ref_pos` 表达，`Array_list` 可重排；这不证明
  ALSA Mode1 的物理 slot、极性或 reference 采样点。
- 初始化成功返回非空 handle。头文件和导出符号没有独立 reset；当前只验证了
  `destory`、参数释放和重新初始化。`rkaudio_param_set` 不能冒充历史状态 reset。
- 当前 preprocess 入口使用 `RKAUDIOParam` 结构配置。板端没有找到 pinned JSON，但
  struct-param probe 可初始化；不能外推其他 Rockchip 模式不需要 JSON。
- 探针直接依赖 AEC/BF 与 common 库，不加载 detect。全零输入只证明 ABI、尺寸、调用和
  guard，不证明 AEC、ANR、BF、AGC 的效果或实际内部顺序。

vendor 16 ms 与核心 20 ms 不能一对一映射：80 ms 内分别是 5 个 vendor 块与 4 个
核心帧。`AudioDspFrameBridge16k` 已把该比率作为独立、有界状态机实现，明确表达
`kNeedMoreInput`/`kOutputAvailable`、最早缓存输入的 metadata 归属以及
generation/fault 清理。`Rockchip3aAudioDspAdapter` 在其上实现固定
`MIC-L/MIC-R/REF-L/REF-R` 交织、精确 1024-sample 输入、512-byte 输出检查和
destroy/init reset；Host fake 覆盖 1/1/1/2 backend block 调用节奏、0/1/1/2 输出节奏、
1,000 帧逐样本守恒、错误清空和换代。板端全零输入也完成两代各 4 输入/5 vendor 调用/
4 输出。它不改变 `AudioDspEngine`，也不声明物理 packing、完整错误域或声学效果已验证；
连续非零输入 HIL 完成前 Release 仍保持 fail-closed。详细证据见
[Rockchip 3A 可行性平台适配器离线探针记录](../test/rockchip-3a-adapter-probe-20260727.md)。

仍未验证真实双麦/reference packing、声学参数、返回值的完整错误域、算法延迟、
CPU/RSS、单帧最坏耗时、持续实时率、故障恢复和声学效果。vendor 指南虽列出多个
采样率，本轮只执行 16 kHz，不能据此宣称 48 kHz 已通过。

## WakeWordEngine 契约

`WakeWordEngine` 固定消费完整的 16 kHz、20 ms、mono、S16_LE、320-sample
`Mono16kFrame`。`Process` 返回无字符串的 POD 结果：decision、规范化 error、
一基 keyword index，以及可选的 `score_milli`。

- `score_milli` 只有在 `score_available=true` 时有效，范围为 0–1000。
- Snowboy API 不提供置信度，因此当前 feasibility adapter 返回
  `score_available=false`、`score_milli=0`；sensitivity 不是检测分数。
- Snowboy `RunDetection()` 的 `-2` 只映射为 diagnostic `kSilence`，不能作为产品 VAD。
- `-1`、异常、未知负值或越界 keyword index 都必须转换为 backend error 并失败关闭。
- `Reset(kVadSegmentEnded)` 只重置 detector 的 segment 状态；它不替代 generation gate。
- reset 结果只有 `reset=true/error=None` 或 `reset=false/error!=None` 两种一致状态。

generation、sequence/timestamp 连续性、500 ms pre-roll 和产品 VAD 都属于单独的
wake/VAD worker，而不是 engine。当前 portable 控制核心已经固定以下边界：

- worker 独占 engine、`VadUtteranceController` 和输入 SPSC consumer；所有方法都由该
  owner 串行调用。
- 旧 epoch/stream 帧不进入 detector、VAD 或 pre-roll。
- discontinuity、丢帧或 backend error 清除 pre-roll、锁存 fault，并要求新 generation。
- 500 ms pre-roll 是 AEC 后 mono 的 25 个 20 ms 固定帧；不能持久化原始录音。
- 播放期间 80 ms duck、160 ms 打断确认和 700 ms 尾静音由独立产品 VAD 实现，
  不能使用 Snowboy silence 或 wake detection 代替。
- 控制核心只接受注入的 speech/silence，不包含或虚构能量阈值算法；其上行输出固定为
  40 帧（800 ms），消费者跟不上时清空并取消 turn，不补发陈旧 PCM。
- 首语音等待上限为 6 秒，utterance 上限为 60 秒。打断确认结果同时携带 cancel response
  和 clear playback queue，且排队的 utterance 开头始终包含完整 25 帧 pre-roll。

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
才会创建 imported targets。启用 Rockchip 时还会定义显式构建、`EXCLUDE_FROM_ALL`
且不安装的 `boompi_rockchip_3a_probe` 和 `boompi_rockchip_3a_adapter_probe`；启用
Snowboy 时只在相同 Debug feasibility
闸门内创建私有 legacy bridge、platform adapter 和显式构建、`EXCLUDE_FROM_ALL`、
不安装的 `boompi_snowboy_offline_probe`。Release/RelWithDebInfo/MinSizeRel 和包含其他
configuration 的多配置生成器都会被拒绝。通过该闸门或 probe 不等于产品 adapter
获批、模型错误路径安全或持续板端实时率已经通过。

当前 OpenBLAS archive 含多线程实现，只完成了 ABI/link 候选验证。发布接入前必须按
单线程配置重建、记录来源与许可、生成新的 SHA-256，并替换本模块 pin；不得用
feasibility opt-in 绕过发布审查。

Snowboy、OpenBLAS、Rockchip 库、资源和模型继续保留在仓库外；来源、版本、许可、
SHA-256 和再分发范围全部确认前禁止提交二进制。显式路径不得写入 preset、安装包日志
或 target 的 PUBLIC 接口。

Snowboy 候选静态库使用旧 libstdc++ 字符串 ABI。当前只有一个私有 C bridge TU
包含 `snowboy-detect.h` 并私有设置 `_GLIBCXX_USE_CXX11_ABI=0`；bridge 外只传固定宽度
整数、PCM 指针、长度和不透明 handle，不传 `std::string`、异常、RTTI 或 Snowboy
对象。普通 backend 返回与可传播异常会被转换为错误码，旧 ABI 定义不扩散到
`boompi_audio_core` 或应用；但缺失模型的实测结果是库内直接 `terminate`，证明当前
archive 不能作为安全的同进程边界。v1 不使用动态插件，也不因兼容失败擅自改成多进程
或替换唤醒引擎。

## 后续验证顺序

1. 把已通过零输入探针的 Rockchip platform adapter 接入单线程 DSP worker 与 reset ACK。
2. 在板端验证真实 16 kHz 双麦/reference packing、mono 输出、算法延迟、CPU/RSS 和
   实时率。
3. 获取或重建错误可返回且使用单线程 OpenBLAS 的 Snowboy runtime，再把已实现的
   feasibility adapter 接入单线程 wake worker。
4. 在已完成默认模型正向离线检测的基础上，验证真实麦克风准确率、误唤醒、漏唤醒
   和最坏帧耗时，再进行至少
   30 分钟稳定性测试。
5. 把已实现的 500 ms pre-roll/分段/打断控制核心接入单线程 worker 和真实独立 VAD，
   再验证播放清理 ACK 与上行顺序；功能通过前不进行压力测试。

真实 adapter 或 HIL 记录完成前，README、UI capability 和测试报告都不得写“DSP 已接通”
“Snowboy 可用”或“AEC/唤醒已通过”。
