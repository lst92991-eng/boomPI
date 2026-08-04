# 音频后端契约与依赖闸门

## 本阶段状态

当前方向是 vendor backend 先接通、最小闭环、再按实测拆分。2026-07-31 已删除未进入
真实 `boompi-client` 的通用 capture/DSP 前端、playback control/committer/worker、软件
ledger 和旧 ALSA playback adapter；历史 host 与交叉链接结果仍保存在测试记录中，但不再
构成当前架构。

当前板端运行链路只保留真实使用的边界：`AudioEngine` 是 application 唯一音频接口，管理
播放线程及有界队列；其私有 `AudioBackend` 内聚直接 ALSA、`RockchipVoiceDsp`、Snowboy legacy bridge、WebRTC VAD、
24→48 kHz 重采样和 gain/limiter；`VoiceClient` 与 `VoiceTransport`
分别负责状态机和 WSS/TLS 会话。
raw `rk_mpi_ai`/`rk_mpi_ao` 仍是独立 HIL 候选，不接入产品进程。详细只读证据见
[P0 vendor 音频证据基线](../test/p0-vendor-audio-inventory-20260727.md)；MPI 头文件、依赖和
当时 21 个入口的交叉链接证据见
[Rockchip MPI 音频交叉链接验证记录](../test/p0-rockchip-mpi-link-validation-20260727.md)。

## 数据流与帧布局

```text
direct ALSA Mode1: 48 kHz / S16_LE / 4 capture channels
  -> [mic0,mic1,refL,refR]
  -> phase-aligned 48 -> 16 kHz conversion
  -> one Rockchip 3A path
  -> 16 kHz mono for VAD / wake / uplink
```

当前 DTB 的 `TRCM clk-trcm=1` 只是共享 TX 时钟，不能独自证明布局。第三块板的相关性 HIL
已经确认运行时 mixer `Mode1` 对应 `[mic0,mic1,refL,refR]`；997 Hz→`refL`、1499 Hz→`refR`
的相关系数均为 `0.9983`。这个结论仅适用于当前板/镜像，不能从 vendor 示例外推到其他 BSP。

## 直接 ALSA 全双工边界

私有 `AudioBackend` 先临时设置并回读 Mode1，再重新打开 capture/playback 两个 blocking handle，
精确协商 48 kHz、S16_LE；capture 为 4 ch、960-frame period、1920-frame buffer，playback
为 2 ch、960-frame period、3840-frame buffer。控制/采集线程读取完整 20 ms 帧；独立播放
线程从固定 1.5 s 队列消费 TTS。独立 capture/DSP 线程持续排空 ALSA，并通过固定
`4 × 20 ms` capture actor queue 提交处理完成的帧；队列溢出时丢弃陈旧帧并显式标记
discontinuity，不静默拼接时间轴。

- capture xrun 恢复后显式报告 discontinuity，并重置 DSP、Snowboy 和 VAD 历史。
- playback 在固定 scratch 中转换为双声道并处理 partial write；可恢复 xrun 重新提交当前块，
  Mode1 回采仍反映实际 DAC 数据；不可恢复错误结束本轮播放，不伪造 capture discontinuity。
- 正常关闭和半初始化失败均恢复原 loopback mixer；测试结束状态为 `Disabled`。
- `DropPlayback` 用于打断或失败；`EndPlayback` 用于正常 EOS，最终由播放线程有界收尾。
- 这条生产路径只在匹配 RV1106 BSP 的交叉工具链和真板上构建运行，不提供虚假的 host
  ALSA null 等价实现。

旧 `PcmPlaybackSink48k`/`AlsaPcmPlaybackDevice` 与配套 control/committer/worker 已从生产
树和测试矩阵删除。其 2026-07-27 结果仅是历史实现证据。

## Rockchip 3A 运行边界

`RockchipVoiceDsp` 是当前唯一产品 3A adapter。它使用匹配 BSP 的固定 16 kHz 帧配置和
真实 vendor ABI；外层不再维护通用 `AudioDspEngine`、channel mapper 或 capture frontend。
当前板已确认输入布局、reference 来源和固定功能 profile。discontinuity、xrun 或
hard-reference broken 时重建 3A 前端；普通 turn cancel 只重置 listener 与播放状态，
不销毁 vendor DSP。

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

## Rockchip 3A 当前适配与未决契约

匹配 BSP 已确认 `rkaudio_preprocess_init`、`rkaudio_preprocess_short` 和拼写如此的
`rkaudio_preprocess_destory` 三个入口，精确实现库为 `libaec_bf_process.so`，目标 ABI 是
ARMv7 hard-float/uClibc。匹配 header/PDF/wrapper 和 binary 已确定 16 kHz、16-bit、
256 samples（16 ms）；当前生产 profile 使用 `src_chan=2, ref_chan=1`，输入为 768 shorts，
ref-last 顺序为 `[mic0,mic1,refL]`，BF mono 成功返回 512 bytes。直接 API 由调用方构造
`RKAUDIOParam`，不读取 VQE JSON；Rockit/RockAA file mode 才解析 JSON。当前生产组合只启用
FastAEC、AES、ANR、Dereverberation 和 STDT，vendor AGC 已关闭；外层不得无依据叠加同类
处理。公开 ABI 不提供 DTD 事件；`wakeup_status` 是唤醒状态，不是 DTD 或产品 VAD。

当前 `RockchipVoiceDsp` 已进入 `boompi-client`：输入固定为 16 kHz/S16、
Mode1 仍提供 `[mic0,mic1,refL,refR]`；适配器在 vendor 边界固定丢弃 `refR`，以
`[mic0,mic1,refL]` 用定长 FIFO 把产品 320-sample/20 ms 帧桥接到 vendor
256-sample/16 ms block。当前生产 profile 固定 mask `1109`、STDT high/low `0.70/0.50`、
`model_aec_en=0`，不启用 software delay；`ALC31/ref1/delay0` 是本轮候选参数，并非最终
壳体声学结论。3A 初始化、固定帧调用和
Snowboy/VAD 后续链路已经在第三块板运行；这只能证明实际代码路径被调用，不能替代以下 HIL：

- 板端 mic0/mic1 的最终物理左右命名、极性及最终壳体声学表现。
- 最终壳体的 ERLE、残余回声、最大音量和真人 double-talk。
- discontinuity 后 destroy/re-init 的长期稳定性和恢复耗时。
- 算法延迟、CPU/RSS、单帧最坏耗时和持续实时率。

匹配工具链的真实符号 link-check 已通过；它只关闭编译、链接和三个动态入口的解析，详见
[2026-07-27 Rockchip 3A 交叉链接验证记录](../test/p0-rockchip-3a-link-validation-20260727.md)。
2026-07-29 的历史固定帧 HIL 把当时的 `init(16000,16,2,1)`、单帧 768-short 输入、
512-byte 成功返回和 handle→parameter tree 清理顺序固化为不安装、不自动执行的
`EXCLUDE_FROM_ALL` target。独立 HIL ELF 不直接 `NEEDED` vendor 库；按规定从外层清理 loader override
后，无参数 dry-run 不主动加载 vendor 库，
只有双 opt-in 与安全前置检查通过后才从固定 `/oem/usr/lib/libaec_bf_process.so` 路径 `dlopen`
并解析固定入口；loader override 环境必须为空，运行前仍须单独核对目标文件哈希。Linux fake 6/6
与匹配 RV1106 交叉构建已通过，详见
[2026-07-29 构建验证](../test/p0-rockchip-3a-hil-build-validation-20260729.md)。独立 HIL ELF
的执行边界仍按该记录控制；产品进程已经运行并不自动关闭物理 layout、声学效果或长期实时率。

2026-08-01 的历史 direct 3A 板端 HIL 已验证 `init(16000,16,2,2)`、1024-short/2048-byte
输入、512-byte 输出、guard 完整；init/process 分别为 `11262 us`/`1561 us`。详细证据和
有限无人工收敛边界见
[P0 Mode1 硬件播放参考验证记录](../test/p0-mode1-hard-reference-validation-20260801.md)。

同类无人声/嘈杂环境 A/B 中，AGC 开启 `n=5` 得到 `confirmed=4/5`、`follow=5/5`、
`attempts=119`；关闭 AGC 后累计 `n=10` 得到 `confirmed=2/10`、`follow=3/10`、
`attempts=43`。这说明 AGC OFF 对误触发有明显改善，但仍有残余确认和 follow-up，且环境噪声
没有受控，不能替代 ERLE、残余回声或真人 double-talk 验收。播放期参考探针先以原音量
连续确认近讲 6 帧（120 ms），随后一次性静音；不使用中间音量。之后最多等待 15 帧取得
连续 3 帧低 reference，清尾 3 帧（60 ms）并重置 listener，再连续 3 帧（60 ms）确认近讲，
成功后才清空播放并取消 response。新 turn 另需有效 VAD start 和 400 ms 连续低 reference。
失败恢复播放并冷却 15 帧（300 ms）。自然播放结束另由 backend 抑制 15 帧（300 ms）尾音，
follow-up 再连续 20 帧（400 ms）确认 VAD。它是应用层防自激
containment 候选，探针期间的采样不得用于宣称 AEC 通过或计算有效 AEC 分数。

## Snowboy 与 VAD 当前边界

产品没有通用 `WakeWordEngine` 层。一个私有 C ABI bridge 是唯一 Snowboy 边界：只有该翻译单元
包含 `snowboy-detect.h` 并设置 `_GLIBCXX_USE_CXX11_ABI=0`；边界外只传固定宽度状态、PCM 指针、
sample 数和不透明 handle。bridge 捕获全部 C++ 异常，禁止旧 ABI、RTTI、Snowboy 对象或
`std::string` 扩散到其余目标。

采集 actor 对同一帧 AEC 后 16 kHz/mono/320-sample PCM 依次调用 Snowboy 和 WebRTC VAD，
不增加 wake worker 或中间音频队列。Snowboy 只报告关键词命中，不提供可用置信度，也不充当
产品 VAD；WebRTC VAD 在 3A 后产生瞬时判定，普通非 follow-up 语音默认 120 ms 起始和
700 ms 尾静音迟滞；播放 containment 后的 follow-up 使用上述 400 ms 二次确认。
首次有效硬参考后保留 600 ms warm-up，生产 `near_voice` 不读取 raw mic RMS。application
独占 25 帧/500 ms pre-roll、turn generation 和打断状态，discontinuity 会清空旧时间轴。
生产目标中没有 unavailable/fake 唤醒实现。

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
和包含其他 configuration 的多配置生成器都会被拒绝。通过该闸门只证明依赖输入匹配，
不证明产品 adapter 已在该镜像正确加载、模型可用或板端实时率通过。

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

Snowboy 静态库使用旧 libstdc++ 字符串 ABI。当前实现只允许一个私有 C bridge TU
包含 `snowboy-detect.h` 并私有设置 `_GLIBCXX_USE_CXX11_ABI=0`；bridge 外只传固定宽度
整数、PCM 指针、长度和不透明 handle，不传 `std::string`、异常、RTTI 或 Snowboy
对象。所有异常在 bridge 内转换为错误码，旧 ABI 定义不得扩散到 `boompi-client`
或应用。v1 不使用动态插件，也不因兼容失败擅自改成多进程或替换唤醒引擎。

## 后续验证顺序

1. direct ALSA Mode1 四通道相关性和 48 kHz 全双工已通过；raw rk_mpi 仍是独立候选，其真实
   执行仍被 `rkipc` owner 阻断。
2. 直接 3A link-check、历史 2 mic + 2 ref 固定帧真板调用，以及现行 2 mic + refL fake、目标
   交叉构建和产品进程初始化已通过；继续关闭错误恢复、CPU/RSS、持续实时率和最终壳体 AEC 效果。
3. 私有 Snowboy legacy bridge、启动期格式校验、500 ms pre-roll、WebRTC VAD 和播放打断
   已进入产品路径；继续记录目标模型的误唤醒、漏唤醒、最坏帧耗时和至少 30 分钟稳定性。
4. 以 Mode1 硬件参考进行真人 double-talk、最大音量和 3 秒追问验收；功能通过前不进行压力测试。

README 和测试报告可以写“3A/Snowboy 已初始化并进入产品路径”，但在相关 HIL 完成前不得写
“AEC 声学效果、唤醒准确率或长期稳定性已通过”。
