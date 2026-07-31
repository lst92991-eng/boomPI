# 音频后端契约与依赖闸门

## 本阶段状态

当前方向是 vendor backend 先接通、最小闭环、再按实测拆分。2026-07-31 已删除未进入
真实 `boompi-client` 的通用 capture/DSP 前端、playback control/committer/worker、软件
ledger 和旧 ALSA playback adapter；历史 host 与交叉链接结果仍保存在测试记录中，但不再
构成当前架构。

当前板端运行链路只保留真实使用的边界：`AlsaSingleTurnIo`、`RockchipVoiceDsp`、
`SnowboyWakeWordEngine`、WebRTC VAD、24→48 kHz renderer/resampler/gain 与 WSS 协议。
raw `rk_mpi_ai`/`rk_mpi_ao` 仍是独立 HIL 候选，不接入产品进程。详细只读证据见
[P0 vendor 音频证据基线](../test/p0-vendor-audio-inventory-20260727.md)；MPI 头文件、依赖和
当时 21 个入口的交叉链接证据见
[Rockchip MPI 音频交叉链接验证记录](../test/p0-rockchip-mpi-link-validation-20260727.md)。

## 数据流与帧布局

```text
rk_mpi or ALSA: 48 kHz / S16_LE / measured channel count
  -> measured layout: dual-mic | mic+reference | 2mic+reference(s)
  -> explicit map and 48 -> 16 kHz conversion
  -> one verified Rockchip 3A path
  -> 16 kHz mono for VAD / wake / uplink
```

当前 DTB 的 `TRCM clk-trcm=1` 只是共享 TX 时钟；vendor AI VQE 样例请求的 loopback
Mode2 是独立 mixer 选择。物理 slot、极性、reference 数量和 packing 必须由板端相关性
测试确定，不能从示例或类型名推断。

## 直接 ALSA 全双工边界

`AlsaSingleTurnIo` 同时拥有 capture/playback 两个 nonblocking handle，并精确协商
48 kHz、S16_LE、2 ch、960-frame period 和 3840-frame buffer。它不选择声卡、不改 mixer、
不拥有线程；`manual_single_turn.cpp` 负责 capture pump、播放和 turn 生命周期。

- `Capture20Ms` 只发布完整 20 ms period；恢复过 xrun 的帧显式标为 discontinuity。
- `WriteMono48k` 在固定 scratch 中复制为双声道，处理 bounded wait/partial write，并限制
  playback recovery 次数。
- `DropPlayback` 用于打断，`DrainPlayback` 用于正常结束；两者都有明确超时或错误路径。
- Linux host 的 `alsa-null-api-flow-only` 只证明这条现役 API 流程可打开、读写和收尾；
  `null` 不证明 Codec、I2S、扬声器、时钟或声学表现。

旧 `PcmPlaybackSink48k`/`AlsaPcmPlaybackDevice` 与配套 control/committer/worker 已从生产
树和测试矩阵删除。其 2026-07-27 结果仅是历史实现证据。

## Rockchip 3A 运行边界

`RockchipVoiceDsp` 是当前唯一产品 3A adapter。它使用匹配 BSP 的固定 16 kHz 帧配置和
真实 vendor ABI；外层不再维护通用 `AudioDspEngine`、channel mapper 或 capture frontend。
输入布局、reference 来源、延迟与功能位仍必须由目标板 HIL 确认。discontinuity、xrun 或
turn 取消时，运行时必须重置 vendor 历史，不能继续发送伪连续音频。

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
test 和直接 3A 库存在，并看到 VQE JSON 缺失。2026-07-28 SSH 恢复后的专用只读 preflight
又确认：PCM owner 为 0，但运行中的 `rkipc` 持有 22 个 `/dev/mpi/*` FD，并加载 Rockit/MPP/
rkaudio；因此 raw MPI HIL 必须阻断，板端 ARM ELF 仍未执行。完整证据见
[MPI HIL 只读前置验证记录](../test/p0-rockchip-mpi-audio-preflight-20260728.md)。

独立的 direct ALSA 有界工具已在当前板运行，不经过现有 production adapter：实际接受
48 kHz/S16_LE/2ch、480-frame period 和 4 periods，数字播放固定为全零，并把指定 DAC enum
切到 Off 后回读。两轮均取得约 3.94 秒重叠、精确 capture 长度、精确 playback 输入文件、
`aplay` 返回 0、clean dmesg delta 和 mixer 恢复。当前板仍运行旧
`RV1106-Atguigu`/`SingadcL` 镜像；临时 `DiffadcLR` 对照中两个 slot 均出现非恒定且无
满幅饱和值的样本，但正确自定义 BSP、物理左右/极性和 reference 仍未关闭。详细契约和结果分别见
[直接 ALSA 全双工 HIL 指南](../test/p0-alsa-full-duplex-hil-guide.md)与
[2026-07-28 真板验证记录](../test/p0-alsa-full-duplex-validation-20260728.md)。该工具不是新的
playback/control/worker 抽象。

raw MPI 对照同样保持为独立的显式探针：`BOOMPI_BUILD_ROCKCHIP_MPI_AUDIO_HIL` 默认关闭，
target 为 `EXCLUDE_FROM_ALL`，不安装、不进入 CTest，也不被任何普通 target 依赖。首轮固定
48 kHz/vendor 16-bit/stereo、AI 6 秒和 AO 4 秒数字静音；只有 AI/AO 在连续 30 个 100 ms
bucket 内都出现成功调用，且这些 common bucket 均无任一侧错误，才形成至少 3 秒的本地并发
证据。AI 只读取 handle、capacity 和 metadata，不复制 PCM。执行前的两次 `/proc/*/fd`
只读扫描现已覆盖配置的 PCM 与全部 `/dev/mpi/*`，避免 `rkipc` 只持 MPI FD 时被错误放行；
它仍只是 snapshot-only 快照，不是排他预留，也不能消除扫描后的竞争。外层必须提供当前
镜像上可正向证明的 maintenance/exclusivity，不能把只有 boomPI 遵守的 `flock` 当成服务锁。

探针本地 `probe_status` 只关闭 raw transport、frame/MB ownership、EOS 和 cleanup 事实；
`full_hil_status` 保持 `not_evaluated`。完整 HIL 还需外层 watchdog、未来已验证的连续
kernel-log evidence、残留进程/设备状态检查以及明确的板卡和镜像记录。即便完整 HIL 通过，也不得据此宣布
S16_LE、双麦 packing、reference slot 或可听播放。详细边界见
[Rockchip MPI 原始音频 HIL 指南](../test/p0-rockchip-mpi-audio-hil-guide.md)。当前 BusyBox dmesg
没有 follow，`/dev/kmsg` stream 语义未验证；OEM stop 链又会 killall rkipc/udhcpc 并停止全部
OEM service，所以当前只能保持 `safe_to_execute=false`，不得自动 stop/start 后强跑探针。

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

匹配工具链的真实符号 link-check 已通过；它只关闭编译、链接和三个动态入口的解析，详见
[2026-07-27 Rockchip 3A 交叉链接验证记录](../test/p0-rockchip-3a-link-validation-20260727.md)。
显式固定帧 HIL 又把 pinned profile、`init(16000,16,2,1)`、单帧 768-short 输入、
512-byte 成功返回和 handle→parameter tree 清理顺序固化为不安装、不自动执行的
`EXCLUDE_FROM_ALL` target。HIL ELF 不直接 `NEEDED` vendor 库；按规定从外层清理 loader override
后，无参数 dry-run 不主动加载 vendor 库，
只有双 opt-in 与安全前置检查通过后才从固定 `/oem/usr/lib/libaec_bf_process.so` 路径 `dlopen`
并解析固定入口；loader override 环境必须为空，运行前仍须单独核对目标文件哈希。Linux fake 6/6
与匹配 RV1106 交叉构建已通过，详见
[2026-07-29 构建验证](../test/p0-rockchip-3a-hil-build-validation-20260729.md)。该 ELF 尚未在板端
执行，下一步仍须在正确镜像和物理 layout 关闭后逐项取得真实返回。没有证据时状态只能写
“未验证”。

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
`boompi_rockchip_3a_link_check` 或 `boompi_rockchip_mpi_audio_link_check`。额外显式开启
`BOOMPI_BUILD_ROCKCHIP_3A_HIL` 或 MPI HIL 选项只创建对应的 `EXCLUDE_FROM_ALL` target；
仍须按 target 名构建，且不会 install、注册 CTest 或自动运行。3A link-check 以匹配
header 的函数类型引用 init/process/destroy；当前 MPI target 引用上述 22 个 raw MPI 入口，
并显式建模 Rockit→MPP/RGA 依赖。两者在同一 tests-off 默认 ALL 构建中共存通过。target
均关闭 build RPATH，私有 BSP 路径不进入公共接口。3A 固定帧 HIL 由已有 direct link-check
保留链接证据，自身只在执行门后动态解析 vendor 入口；清理 loader override 后的 dry-run 不会
主动加载 vendor binary。结果
仍不证明板端 loader、初始化、音频
处理或实时率，证据见 [3A 交叉链接验证记录](../test/p0-rockchip-3a-link-validation-20260727.md)、
[3A HIL 构建验证](../test/p0-rockchip-3a-hil-build-validation-20260729.md)、
[历史 MPI 音频交叉链接验证记录](../test/p0-rockchip-mpi-link-validation-20260727.md)与
[当前 MPI HIL 构建验证记录](../test/p0-rockchip-mpi-hil-build-validation-20260728.md)。

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

1. direct ALSA 48 kHz transport 全双工已通过；先对齐目标自定义 BSP，再完成通道相关性
   HIL。rk_mpi 最小生命周期的真实交叉链接已通过，但真实执行仍被 `rkipc` owner 阻断。
2. 直接 3A link-check、固定帧 HIL 的 Linux fake 和目标交叉构建已通过；下一步在正确镜像上
   关闭物理 slot 映射、真实加载/返回、错误恢复、mono 输出、算法延迟、CPU/RSS 和实时率。
3. 实现私有 Snowboy legacy bridge、启动期模型/格式校验和单线程 wake worker。
4. 验证目标英文模型的加载、准确率、误唤醒、漏唤醒和最坏帧耗时，再进行至少
   30 分钟稳定性测试。
5. 最后接入 500 ms pre-roll、独立 VAD 和播放打断闭环；功能通过前不进行压力测试。

真实 adapter 或 HIL 记录完成前，README、UI capability 和测试报告都不得写“DSP 已接通”
“Snowboy 可用”或“AEC/唤醒已通过”。
