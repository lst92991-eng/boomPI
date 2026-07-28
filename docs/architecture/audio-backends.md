# 音频后端契约与依赖闸门

## 本阶段状态

当前方向是 vendor backend 先接通、最小闭环、再按实测拆分。此前建立的
`AudioDspEngine`/`WakeWordEngine`、renderer、playback control/committer/worker 和 ALSA
playback adapter 继续保留，但暂停增加新抽象或 runtime 组成。它们的 host fake、ALSA
`null` 和交叉链接结果只证明相应软件边界，不能证明板端 AEC、全双工或实时率。

本次集成还保留了已提交的 `AudioDspFrameBridge16k`、`VadUtteranceController`、
`Rockchip3aAudioDspAdapter`、Snowboy legacy bridge/adapter，以及 RV1106 ALSA 全双工
adapter/smoke。它们仍未由 `boompi-client` composition root 组装，不能当作产品运行时已接通。

下一项实现直接面向匹配 BSP 的 `rk_mpi_ai`/`rk_mpi_ao`、ALSA PCM、Rockchip
VQE 和 `libaec_bf_process.so` 探针。详细只读证据见
[P0 vendor 音频证据基线](../test/p0-vendor-audio-inventory-20260727.md)；MPI 头文件、依赖和
当时 21 个入口的交叉链接证据见
[Rockchip MPI 音频交叉链接验证记录](../test/p0-rockchip-mpi-link-validation-20260727.md)。

真实状态必须区分为三层：

1. **核心契约已实现**：固定帧、generation、错误分类和 fail-closed 行为。
2. **依赖候选可配置**：只有显式启用并通过路径与 SHA-256 检查时才创建 imported target。
3. **feasibility adapter 已隔离**：raw MPI 仍只有离线/交叉构建；Rockchip 3A 全零输入和
   Snowboy 正向离线探针已有板端记录，但真实通道、异常安全、产品 worker 与完整 HIL 未完成。

## 数据流与帧布局

```text
rk_mpi or ALSA: 48 kHz / S16_LE / measured channel count
  -> measured layout: dual-mic | mic+reference | 2mic+reference(s)
  -> explicit map and 48 -> 16 kHz conversion
  -> one verified Rockchip 3A path
  -> 16 kHz mono for VAD / wake / uplink
```

现有 `CaptureDspFrontend`/`DspCaptureFrame16k` 实现了四逻辑平面候选，但不再将其视为
生产输入前提。当前 DTB 的 `TRCM clk-trcm=1` 只是共享 TX 时钟；vendor AI VQE 样例请求的
loopback Mode2 是独立 mixer 选择。物理 slot、极性、reference 数量和 packing 必须由
板端相关性测试确定后再适配，不能把某一路麦克风伪装成 AEC 后 mono。

## ALSA playback adapter

本节记录已经完成的历史实现和审计边界；vendor 最小闭环完成前不继续为它增加 runner、
mailbox、control 或 worker 层。

P2f-c-a 增加了两层播放平台边界：跨平台 `PcmPlaybackSink48k` 只负责设备状态校验、
mono 到显式一/二声道的固定 scratch 映射和 portable result；Linux
`AlsaPcmPlaybackDevice` 独占一个以 `SND_PCM_NONBLOCK` 打开的 playback handle。
设备名、声道、period、buffer、start threshold 和 avail-min 都由冷路径配置提供；
adapter 不选择示例板号、不改 mixer，并关闭 libasound 的 automatic resample/channel/format
转换。exact 回读只约束所打开 named PCM 的 API 边界；如果配置选择 `default`、`plug`、
`plughw` 或其他插件，插件内部仍可能转换。产品 HIL 必须另行记录解析后的直接硬件路径，
不能把 named-PCM exact 冒充 Codec/I2S 硬件路径 exact。

每次写入固定按以下顺序推进：

1. 写前 status 必须是 PREPARED 或 RUNNING，delay 在 0–1.5 秒边界内。
2. 最多调用一次 `snd_pcm_writei`，正返回的 partial prefix 立即成为不可逆事实。
3. 正返回后从同一 device 取得 `CLOCK_MONOTONIC` write-complete 时间，再取一次 status。
4. 只有写后状态为 RUNNING、单调时钟域一致，且 status timestamp 位于 write-complete
   与 status-return 后观测时间之间时，才把 timing 标为 `kAlsaStatus`；写后仍是 PREPARED
   表示尚未跨过 start threshold，不能预言起播时间。
5. 任一写后错误都保留正 accepted count，返回 intentionally malformed positive result，
   由 committer 精确推进一次后取消；不发布伪 AEC reference，也绝不重放该 prefix。

`EAGAIN/EINTR/EPIPE/ESTRPIPE/ENODEV|ENXIO|ENOTTY` 分别映射为 would-block、
interrupted、xrun、suspended 和 device-lost，原负 errno 保留用于有界诊断。热路径禁止
调用 `snd_pcm_recover`：恢复不能绕过 PCM incarnation；xrun/suspend 统一由 actor 推进
fence 后执行现有 `Drop -> incarnation++ -> Prepare` 事务。

`BOOMPI_ENABLE_ALSA_PLAYBACK` 在普通 host 默认关闭、RV1106 target 默认开启。启用后，
默认 ALL 构建会链接一个不安装、不自动执行的 `boompi_alsa_playback_link_check`，以便在
tests-off 的交叉构建中也解析 adapter、ALSA 和单调时钟符号；它只证明链接兼容。Linux
还可显式运行 `alsa-null-accepted-only` smoke；`null` 会丢弃数字 PCM，只能证明
libasound API 与 accepted 边界，不能证明 hardware presentation、played 或 audible。
当前 composition root 还没有创建该 device/sink，也没有实际 playback runner。

## AudioDspEngine 契约

`AudioDspEngineConfig` 必须显式选择一种 reference source 和一种 layout：

- `kHardwareCapture`：reference 来自经 HIL 确认的 Codec/I2S/TDM 数字回采平面。
- `kSoftwarePlayback`：reference 来自最终播放链路中已被 playback sink 接受的数字 PCM。
- layout 只能是 mono-left、mono-right 或 stereo，具体选择必须来自真实 API 和硬件证据。

同一时刻只能启用一种 reference source。软件 reference 不能使用收到的原始 TTS 包，
必须位于 jitter、重采样、音量、duck、混音和 limiter 之后；partial device write 只能
贡献已接受的 prefix，不能补零伪装成完整帧。P2f-b-a 的 `AcceptedRenderQueue` 只提供
portable accepted ledger；P2f-c-a 的真实 ALSA adapter 尚未由 runtime 组成，也未接到
AEC consumer。accepted 也不
等于 presented、played 或 audible，预计 presentation 时间不能冒充硬件完成证据。
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

## Rockchip MPI raw AI/AO 候选与链接边界

匹配 BSP 的真实交叉链接已关闭最小 raw 生命周期的编译和动态符号解析：一次
`SYS_Init/Exit`；AI 的 `SetPubAttr`、`Enable`、`SetChnParam`、`EnableChn`、`GetFrame`、
`ReleaseFrame`、`DisableChn`、`Disable`；AO 的 `SetPubAttr`、`Enable`、`SetChnParams`、
`EnableChn`、`SendFrame`、`WaitEos`、`DisableChn`、`Disable`；以及 AO caller buffer 与
AI capture frame 访问所需的 `SYS_CreateMB`、`MB_Handle2VirAddr`、`RK_MPI_MB_GetSize` 和
`MB_ReleaseMB`。`WaitEos` 只服务有限播放
探针的有界 drain。

这个最小面故意不含 `SYS_Bind/UnBind`、VQE、resample、AMIX/mixer、volume/mute/track、
AENC/ADEC 或声音检测接口。raw AI/AO 由 CPU 侧 `GetFrame/SendFrame` 闭合，不建立编码媒体图；
VQE、采样率转换和 mixer 只能在独立 HIL 证明需要后显式加入，不能因为 vendor 样例调用过就
扩大生产依赖。

`librockit.so` 的链接闭包为 `librockchip_mpp.so.1`、`librga.so`、libstdc++、libgcc 和
uClibc。CMake 的 MPP/RGA 哈希固定 `media/out/lib` 中的未 strip 链接候选；OEM 中经过
strip、哈希不同的副本不能作为 CMake 输入。2026-07-27 的历史 link-check 是 ELF32 ARM
EABI5 hard-float/uClibc，保留 Rockit/MPP/RGA `NEEDED` 与 21 个 `UND`，无
`RPATH/RUNPATH`；完整证据和哈希边界见
[2026-07-27 MPI 音频交叉链接验证记录](../test/p0-rockchip-mpi-link-validation-20260727.md)。
当前 link-check 与 HIL 因精确加入 `RK_MPI_MB_GetSize` 扩展为 22 个 `UND`，并已完成真实
RV1106 交叉构建，证据见
[2026-07-28 MPI HIL 构建验证记录](../test/p0-rockchip-mpi-hil-build-validation-20260728.md)。
这些 target 均不安装、不自动运行，也不证明板端 loader、PCM、全双工或通道布局。

同轮板端 schema v2 只读探针仅看到一个 capture PCM、一个 playback PCM、Rockit、AI/AO
test 和直接 3A 库存在，并看到 VQE JSON 缺失。探针未打开 PCM 或调用 vendor API；随后
物理链路断开，因此 raw MPI HIL 只有离线/交叉构建证据，板端执行仍未完成。

下一步先运行独立的 direct ALSA 有界工具，不经过现有 production adapter：首轮请求
48 kHz/S16_LE/2ch、480-frame period 和 4 periods，数字播放固定为全零，并把指定 DAC enum
切到 Off 后回读。工具默认 dry-run，板端执行需要三重 opt-in、PCM 两次占用检查、单调时钟
重叠、精确字节数、连续 dmesg delta 和 mixer 恢复。详细契约见
[直接 ALSA 全双工 HIL 指南](../test/p0-alsa-full-duplex-hil-guide.md)。它尚未在板端运行，
也不是新的 playback/control/worker 抽象。

raw MPI 对照同样保持为独立的显式探针：`BOOMPI_BUILD_ROCKCHIP_MPI_AUDIO_HIL` 默认关闭，
target 为 `EXCLUDE_FROM_ALL`，不安装、不进入 CTest，也不被任何普通 target 依赖。首轮固定
48 kHz/vendor 16-bit/stereo、AI 6 秒和 AO 4 秒数字静音；只有 AI/AO 在连续 30 个 100 ms
bucket 内都出现成功调用，且这些 common bucket 均无任一侧错误，才形成至少 3 秒的本地并发
证据。AI 只读取 handle、capacity 和 metadata，不复制 PCM。执行前的两次 `/proc/*/fd`
只读扫描只是 snapshot-only 占用快照，不是排他预留，也不能消除扫描后的竞争；外层仍须
持有音频服务锁，防止检查后出现新使用者。

探针本地 `probe_status` 只关闭 raw transport、frame/MB ownership、EOS 和 cleanup 事实；
`full_hil_status` 保持 `not_evaluated`。完整 HIL 还需外层 watchdog、连续可比较的 dmesg 前后
快照、残留进程/设备状态检查以及明确的板卡和镜像记录。即便完整 HIL 通过，也不得据此宣布
S16_LE、双麦 packing、reference slot 或可听播放。详细边界见
[Rockchip MPI 原始音频 HIL 指南](../test/p0-rockchip-mpi-audio-hil-guide.md)。该探针当前尚未上板运行。

## Rockchip 3A 候选与未决契约

匹配 BSP 已确认 `rkaudio_preprocess_init`、`rkaudio_preprocess_short` 和拼写如此的
`rkaudio_preprocess_destory` 三个入口，精确实现库为 `libaec_bf_process.so`，目标 ABI 是
ARMv7 hard-float/uClibc。匹配 header/PDF/wrapper 和 binary 已确定 16 kHz、16-bit、
256 samples（16 ms）；`src_chan=2, ref_chan=1` 时输入为 768 shorts，默认 ref-last，BF
mono 成功返回 512 bytes，非法尺寸返回 0，init 失败返回 null。直接 API 由调用方构造
`RKAUDIOParam`，不读取 VQE JSON；Rockit/RockAA file mode 才解析 JSON。AEC、BF、ANR、
AGC 等模块可能组合启用，因此外层不得叠加同类处理；`wakeup_status` 也不是产品 VAD。

这些仍是 SDK 契约候选而不是板端事实。进入生产 adapter 前必须关闭：

- direct API 的逻辑输入是 interleaved；板端物理 capture slot 如何映射到
  `[mic0,mic1,ref]`，以及 reference tap、极性和延迟仍需 HIL。
- 处理失败和设备 discontinuity 后是否必须 destroy/re-init。
- `RKAUDIOParam` ownership、线程限制和释放顺序，以及 file mode 资源的目标安装路径。
- 算法延迟、CPU/RSS、单帧最坏耗时和持续实时率。

匹配工具链的真实符号 link-check 已通过；全零输入 ABI 探针和 platform adapter 换代探针也已
在板端执行，但只证明固定 16 ms 调用、精确返回长度、destroy/init reset 和受控零输入路径。
详见 [3A 交叉链接记录](../test/p0-rockchip-3a-link-validation-20260727.md)、
[3A 离线 ABI 探针](../test/rockchip-3a-offline-probe-20260727.md)和
[3A adapter 探针](../test/rockchip-3a-adapter-probe-20260727.md)。真实 packing、声学效果、
持续实时率和完整错误域仍为“未验证”。

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

`VadUtteranceController` 已实现外部 VAD 结果驱动的 500 ms pre-roll、6 秒首次说话等待、
700 ms 尾静音、60 秒上限及 80/160 ms duck/打断意图，但尚无真实 VAD detector/worker。
Snowboy adapter 的固定模型正向离线探针已通过；缺失模型会使旧 runtime 直接终止进程，
因此该 adapter 保持 Debug-only、`EXCLUDE_FROM_ALL` 且不进入产品 runtime。

## 外部依赖闸门

`BOOMPI_ENABLE_ROCKCHIP_MPI_AUDIO`、`BOOMPI_ENABLE_ROCKCHIP_3A` 和
`BOOMPI_ENABLE_SNOWBOY` 默认均为 `OFF`。默认 host、CI 和 RV1106 核心构建不读取
私人 SDK 路径，也不会下载或链接 vendor 二进制。仅把
`BOOMPI_TARGET_RV1106` 设为 `ON` 不足以越过闸门；CMake 还会核对真实 cross compile、
Linux/ARM target、固定 RV1106 GNU compiler 和包含 uClibc loader 的显式 sysroot。

显式启用时必须提供绝对路径：

- Rockchip：`BOOMPI_ROCKCHIP_3A_INCLUDE_DIR`、
  `BOOMPI_ROCKCHIP_3A_AEC_LIBRARY`、`BOOMPI_ROCKCHIP_3A_COMMON_LIBRARY`、
  `BOOMPI_ROCKCHIP_3A_DETECT_LIBRARY`、`BOOMPI_ROCKCHIP_3A_CONFIG_FILE`。
- Rockchip MPI：`BOOMPI_ROCKCHIP_MPI_INCLUDE_DIR`、
  `BOOMPI_ROCKCHIP_MPI_ROCKIT_LIBRARY`、`BOOMPI_ROCKCHIP_MPI_MPP_LIBRARY` 和
  `BOOMPI_ROCKCHIP_MPI_RGA_LIBRARY`。
- Snowboy：`BOOMPI_SNOWBOY_INCLUDE_DIR`、`BOOMPI_SNOWBOY_LIBRARY`、
  `BOOMPI_SNOWBOY_RESOURCE_FILE`、`BOOMPI_SNOWBOY_MODEL_FILE` 和
  `BOOMPI_OPENBLAS_LIBRARY`。

CMake 对每个输入执行存在性、文件类型和固定 SHA-256 检查，任一缺失或不匹配立即
停止 configure。固定值来自 [P0 可行性报告](../test/p0-feasibility-report-20260725.md)
和 [vendor 音频证据基线](../test/p0-vendor-audio-inventory-20260727.md)中的审计输入，但它们
只是可行性候选，不是发布批准。必须显式设置
`BOOMPI_ALLOW_FEASIBILITY_AUDIO_VENDOR_INPUTS=ON`，并把生成器限制为 Debug-only，
才会创建 imported targets；Release/RelWithDebInfo/MinSizeRel
和包含其他 configuration 的多配置生成器都会被拒绝。通过该闸门不等于 adapter 已经
实现、模型可以加载或板端实时率已经通过。

显式启用 Rockchip feasibility 输入时，tests-off 默认 ALL 会构建不安装、不自动执行的
`boompi_rockchip_3a_link_check` 或 `boompi_rockchip_mpi_audio_link_check`。3A target 以匹配
header 的函数类型引用 init/process/destroy；当前 MPI target 引用上述 22 个 raw MPI 入口，
并显式建模 Rockit→MPP/RGA 依赖。两者在同一 tests-off 默认 ALL 构建中共存通过。target
均关闭 build RPATH，私有 BSP 路径不进入公共接口；结果仍不证明板端 loader、初始化、音频
处理或实时率，证据见 [3A 交叉链接验证记录](../test/p0-rockchip-3a-link-validation-20260727.md)、
[历史 MPI 音频交叉链接验证记录](../test/p0-rockchip-mpi-link-validation-20260727.md)与
[当前 MPI HIL 构建验证记录](../test/p0-rockchip-mpi-hil-build-validation-20260728.md)。

当前 OpenBLAS archive 含多线程实现，只完成了 ABI/link 候选验证。发布接入前必须按
单线程配置重建、记录来源与许可、生成新的 SHA-256，并替换本模块 pin；不得用
feasibility opt-in 绕过发布审查。

Snowboy、OpenBLAS、Rockchip 库、资源和模型继续保留在仓库外；来源、版本、许可、
SHA-256 和再分发范围全部确认前禁止提交二进制。显式路径不得写入 preset、安装包日志
或 target 的 PUBLIC 接口。

Snowboy 候选静态库使用旧 libstdc++ 字符串 ABI。当前实现由一个私有 C bridge TU
包含 `snowboy-detect.h` 并私有设置 `_GLIBCXX_USE_CXX11_ABI=0`；bridge 外只传固定宽度
整数、PCM 指针、长度和不透明 handle，不传 `std::string`、异常、RTTI 或 Snowboy
对象。所有异常必须在 bridge 内转换为错误码，旧 ABI 定义不得扩散到 `boompi_audio_core`
或应用。v1 不使用动态插件，也不因兼容失败擅自改成多进程或替换唤醒引擎。

## 后续验证顺序

1. 先用当前可用单麦完成客户端单轮录音、WSS 上行、Qwen 下行和播放的纵向闭环。
2. 再接入已实现的 VAD/pre-roll 控制核心和三秒连续监听，然后完成 TTS 打断。
3. 之后把 Snowboy adapter 接入受控 worker，并解决旧 runtime 的异常安全问题。
4. 最后继续关闭双麦/reference packing、Rockchip 3A 实时率和最终壳体 AEC 声学验证。

真实 adapter 或 HIL 记录完成前，README、UI capability 和测试报告都不得写“DSP 已接通”
“Snowboy 可用”或“AEC/唤醒已通过”。
