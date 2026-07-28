# Host 验证

## 目的

Host 检查用于验证工程结构、跨语言协议、状态机和不依赖 RV1106 硬件的逻辑。Host 通过不能替代真实板端 ALSA、Codec、DSP、Snowboy、显示或 Wi-Fi 验证。

## 前置条件

- 支持 C++17 的 host 编译器。
- CMake 3.21 或更高版本和 preset 所需生成器。
- Go 1.26.x，与 `server/go.mod` 一致。
- Python 3；fixture 校验脚本仅使用标准库。

## 标准命令

在仓库根目录执行：

```text
cmake --preset host-debug
cmake --build --preset host-debug --parallel
ctest --preset host-debug --output-on-failure
python scripts/verify_protocol_fixtures.py
python scripts/dsp/generate_fir_decimator_48_to_16.py --check client/src/audio/fir_decimator_48_to_16.cpp --quiet
python scripts/dsp/generate_playback_resampler_24_to_48.py --check client/src/audio/playback_resampler_24_to_48.cpp --quiet
```

Linux 安装 ALSA 开发头后（Debian/Ubuntu 为 `libasound2-dev`），CI 还会把同一 preset 显式配置为
`-DBOOMPI_ENABLE_ALSA_PLAYBACK=ON`，并运行带
`alsa-null-accepted-only` label 的 smoke。启用 ALSA 时，默认 ALL 构建还会生成不执行的
link-check executable；Windows/macOS 默认不编译 libasound target。

Linux/macOS 还需校验只读 P0 探针和显式 opt-in HIL 脚本：

```text
sh -n scripts/probes/rv1106_p0_probe.sh
sh -n scripts/probes/rv1106_rockchip_mpi_audio_preflight.sh
sh -n scripts/hil/rv1106_alsa_full_duplex.sh
python3 -m unittest discover -s scripts/tests -p 'test_*.py' -v
```

ALSA HIL 的自动测试只使用 fake commands 与临时目录，必须覆盖 dry-run 在所有命令访问和
文件写入前返回、缺失 opt-in、PCM 占用、mixer 恢复、有限字节/重叠和 dmesg delta。它不
连接 Codec 或板卡，不能算作全双工通过。操作边界见
[直接 ALSA 全双工 HIL 指南](p0-alsa-full-duplex-hil-guide.md)。

raw MPI HIL 的自动测试同样只关闭 CMake/CLI 离线闸门：默认 `OFF`、显式 target 为
`EXCLUDE_FROM_ALL`、无自动 build/install/CTest/run，并用 fake ABI 验证 dry-run 零 MPI 调用。
真实 RV1106 交叉构建证明当前 22 个 Rockchip MPI 符号和链接闭包可解析，但没有执行该
ARM ELF，不能算作板端 raw 全双工通过。证据见
[2026-07-28 Rockchip MPI HIL 构建验证记录](p0-rockchip-mpi-hil-build-validation-20260728.md)。

专用 MPI preflight 的 Linux fixture 还必须证明：help/非法参数零探测，无 owner 时仍不会
给出执行许可，`rkipc` 仅持 `/dev/mpi/*` 也会阻断，PCM owner/扫描不完整/dmesg 无 follow/
危险 OEM stop 均 fail closed，并且测试前后 fixture 字节不变、没有调用 mixer/PCM/signal/
service-control 命令。板端只读结果见
[2026-07-28 MPI HIL 只读前置验证记录](p0-rockchip-mpi-audio-preflight-20260728.md)。

Linux 上修改实时队列或 PCM 边界后，还需运行 sanitizer 构建。以下目录都是忽略的
本地构建产物：

```text
cmake -S . -B build/host-asan-ubsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBOOMPI_BUILD_TESTS=ON \
  -DBOOMPI_STRICT_WARNINGS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build/host-asan-ubsan --parallel
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build/host-asan-ubsan --output-on-failure

cmake -S . -B build/host-tsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBOOMPI_BUILD_TESTS=ON \
  -DBOOMPI_STRICT_WARNINGS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build/host-tsan --parallel
TSAN_OPTIONS=halt_on_error=1 build/host-tsan/client/boompi_audio_queue_tests
```

若 GCC ThreadSanitizer 在启用 PIE/ASLR 的虚拟机中仅报
`unexpected memory mapping`，可只对该测试进程使用
`setarch "$(uname -m)" -R` 后重试。不得据此忽略真实 race 报告，也不得把关闭
ASLR 的设置带入产品运行环境。

本阶段的 Linux TSan 二进制已成功构建，但运行时在测试逻辑启动前即以
`FATAL: ThreadSanitizer: unexpected memory mapping` 退出。因此该项当前状态是
“运行时环境不可用”，不是代码 race 报告，也不能记作 TSan 通过；GCC Debug/Release
以及 ASan/UBSan 是分开执行的验证结果。

Windows 如果使用 Visual Studio 多配置生成器，将 build/test 两条命令改为：

```text
cmake --build --preset host-debug --parallel --config Debug
ctest --preset host-debug --output-on-failure -C Debug
```

在 `server/` 目录执行：

```text
go test ./...
go vet ./...
go build -trimpath ./cmd/boompi-server
```

2026-07-28 14:38:40 +08:00 的最终树已在 clean 临时目录通过 16 个 CTest、53 个
Python/script 测试，以及 Go 1.26.5 的 `go test ./...`、`go vet ./...` 和
`go build -trimpath ./cmd/boompi-server`。其中 Rockchip MPI 专项为 10/10，通用 vendor
CMake 专项为 12/12。这些是离线 host/构建回归，不包含付费 provider 请求，也不替代
RV1106 HIL。

自动测试必须离线运行，不读取真实 `DASHSCOPE_API_KEY`，也不得发起付费 provider 请求。需要真实 Qwen 的测试必须使用单独的显式开关，并在测试报告中记录区域、模型和费用风险。

## P1 最低检查项

- CMake configure/build/CTest 在 Windows、Linux 和 macOS 上通过。
- Go test/vet/build 在 Windows、Linux 和 macOS 上通过。
- C++ 和 Go 的协议常量与根 `protocol/protocol-v1.md` 一致。
- `protocol/fixtures/protocol-v1-golden.json` 能由标准库脚本解析，64-byte PCM header 的每个 offset 和 wire hex 一致。
- malformed length、未知版本、超大 payload 和无效 ID 的测试不会越界或隐式改变状态。
- 运行产物、配置秘密、证书私钥和本机绝对路径不进入 Git diff。

## P2 DSP 检查项

- mapper 对四个物理 slot 做一一置换，极性只能为 `+1/-1`；非法配置和非法帧不
  修改调用方输出。
- FIR 生成器与提交的 211 个 Q15 系数逐项一致，系数对称且总和为 32768。
- 四路流式 48→16 kHz 结果与独立参考卷积一致；跨 20 ms 帧的历史、Reset、
  正负 half-LSB 舍入和 S16 饱和均有确定性测试。
- 频响测试输入使用固定非零相位，避免抽取后的正弦恰好落在零点而造成阻带假阳性。
- 采集前端与独立 primitive 的输出 bit-exact 一致；空输出重试、跨帧历史、新旧
  generation、旧帧隔离、continuity/transform fault 锁存和新 generation 恢复均有
  确定性测试。
- TTS 短帧只有在 EOS 且未使用尾部全零时有效；完整 reference 的格式、时间戳来源、
  1.5 秒排队上限以及 `ResetHeader` 不清 PCM 均有边界测试。
- `boompi_audio_playback_buffer_tests` 还校验 `AcceptedRenderChunk48k` 的固定
  48 kHz/mono/S16 格式、metadata/identity、绝对 source offset、接受长度、chunk/EOS
  完成关系、write/presentation 时序、1.5 秒排队上限和未使用尾部清零，并覆盖 64 槽
  accepted ledger 的无效 publish 修复、cancel、满队列、FIFO、回绕和槽复用。
- `boompi_audio_playback_resampler_tests` 校验 65-tap Q31 half-band 表、int64 累加与
  对称舍入、跨帧 bit-exact 结果、wide-int32 overshoot 和状态换代；生成器离线扫描
  还必须保持 0–10 kHz 通带纹波不超过 0.005 dB、14–24 kHz 阻带衰减至少 76 dB。
  当前提交表的证据为约 0.001664 dB 通带纹波和 -78.020480 dB 阻带峰值，host tone
  测试另验证 1/6/10 kHz 增益误差不超过 0.01 dB，以及 10 kHz 输入的 14 kHz image
  rejection 优于 70 dB。
- EOS 测试必须覆盖最终 `Process` 的 exact-`2N` prefix（EOS false、要求 drain）和后续
  63-sample drain（EOS true、末位显式为零），并验证 pending 状态、metadata 继承、
  `source_offset_sample_frames`、未使用尾部清零和成功后自动退役。prefix/drain 使用相同
  sequence/timestamp，后续 worker 不得用普通 `FrameContinuityGate` 直接判二者顺序。
- `boompi_audio_playback_gain_limiter_tests` 校验 int32 wide 输入直到 final S16 才转换，
  volume、speaker gain、duck/recovery Q16 ramp、块峰值 limiter 的即时 attack 与跨 chunk
  release，以及过载/限幅/防御性 clamp 统计。默认 volume=60、speaker_gain=100；
  volume=100 是合法配置，但最大音量和更高增益尚未完成最终壳体与板端 HIL 安全验收。
- `boompi_audio_playback_renderer_tests` 校验 facade 的固定顺序、generation/连续性故障、
  失败调用不推进状态、duck 跨帧变化和 EOS 两调用事务。facade 不观察 epoch fence，
  不是 cancel/Arm ACK，也不证明 ALSA accepted-prefix、DAC/扬声器播放或 AEC reference。
- `boompi_audio_pcm_playback_sink_tests` 校验 portable write/control POD 的合法组合、
  control aggregate `{}` 通过 `kUnset` 失败关闭、`UnavailablePcmPlaybackSink` 失败关闭，
  以及固定容量 scripted sink 的参数校验、unchecked malformed 注入、脚本消耗、hook 和
  调用顺序。scripted accepted/timing 是测试输入，不是真实 ALSA 证据。
- `boompi_audio_pcm_playback_adapter_tests` 用 fake device 校验显式 48 kHz 设备配置、
  mono/双声道固定映射、full/partial、全部 typed code 与 native errno、写前/写后 status、
  positive 后仍为 PREPARED、时钟/timestamp/overflow 故障，以及 `Drop`/`Prepare` 的精确
  调用次数。任何 positive 后故障必须保留 accepted count 且形成 malformed result，不能
  重放或发布伪 timing。Linux 显式启用 ALSA 后，`alsa-null-accepted-only` 另验证真实
  libasound named PCM API 边界的 exact configure/status/write/drop/prepare；显式 named
  plugin 仍可能在内部转换，且 `null` 会丢弃 PCM，不能证明直接硬件路径 exact、hardware
  presentation、played 或 audible。
- `boompi_audio_playback_committer_tests` 校验配置/初始化、partial write exactly-once、
  四类 committer 公共结果的空 aggregate 也必须保持 `kUnset`，不得默认 accepted/ACK；
  accepted ledger backpressure 下零 sink write、write 前后 fence 竞态、would-block/
  interrupted 重试、typed reset fault、over-accept/protocol fault、EOS prefix/drain 顺序、
  software/hardware reference policy、accepted sequence/PCM incarnation 耗尽，以及
  stale/duplicate cancel。成功取消的 host 边界是精确 generation 下的
  `Drop -> incarnation++ -> Prepare -> local playback ACK`；随后只通过直接调用验证精确
  request/generation 的 reference-reset confirmation，未验证 mailbox 或 DSP reset producer。
- malformed positive write 测试必须证明：请求范围内的可能 accepted prefix 只保守推进
  一次并计入 cancel 诊断，不会重放；由于 timing/result 不可信，不向 accepted ledger
  发布伪 reference，并进入 cancellation-required。accepted sequence 分配到 `UINT64_MAX`
  后必须在下一次设备 write 前拒绝，完成 `Drop`/`Prepare` teardown 后仍返回 terminal
  restart-required，不能 Confirm/Arm 假恢复。
- cancel 结果测试分别核对 `retired_pcm_incarnation` 与 `prepared_pcm_incarnation`：只有
  `drop_succeeded` 证明旧 timeline 已退役，只有 `prepare_succeeded` 才允许 prepared 值
  非零；incarnation 耗尽或 prepare 失败时 prepared 值必须为 0 且不得 ACK。
- `boompi_audio_playback_control_tests` 校验固定容量值拷贝 SPSC mailbox 的空 aggregate
  失败关闭、满队列/FIFO/回绕和 100,000 条双线程传递，并证明 normal lifecycle 满槽时
  独立 urgent cancel 仍可发布。它还覆盖 producer start/stop、DSP reset、EOS/critical
  分槽契约，以及 actor-owned exact cancellation join 的 active、never-armed 和
  renderer-only 路径。
- active cancel 测试只有在 producer stop、本地 `Drop -> Prepare`、producer 已停后的稳定
  ingress 空观察、按 retired PCM incarnation 完成的 DSP reset 和 worker
  `ConfirmReferenceReset` 全部到齐后才允许 Arm。no-sink 快捷路径仍要求 producer stop 与
  稳定 ingress 空，并必须严格证明 committer 从未 Arm、无 accepted PCM；renderer-only
  路径还必须证明 cancel fence 后 renderer 已 disarm。乱序、身份/epoch/incarnation 不匹配、
  冲突重复和 barrier failure 均不得误完成。
- mailbox 契约要求未来 network producer 先处理 Stop，并在 Start 生效、获取 write lease
  和每次 publish 前重验 Stop/fence/授权。`boompi_audio_playback_worker_tests` 验证
  deterministic host worker core 的有界单步推进、分阶段 Arm、ingress→render→commit、
  EOS prefix/drain，以及 EOS/critical 事件的精确保留重试；critical pending 会暂停普通
  数据工作，但 urgent cancel 仍可执行 teardown。该 core 不创建线程，network producer
  与 DSP endpoint 仍未实现。
- worker 测试还覆盖：冷 fence=0 的结构有效 Arm error、不可关联 envelope 只通过
  `kInvalidControlInput` 报告且不污染 result mailbox、可关联非法 payload 产生结构有效的
  typed error、Arm ACK 发布前保持 `kArming`、精确 Quiesce ACK 原样重放，以及 PCM
  incarnation 耗尽时 Drop-only completion 被 actor join 识别为 terminal restart，而不是
  invalid input 或普通取消 ACK。
- playback gate 覆盖 fence epoch 授权、epoch→stream→turn 的 stale 隔离、精确 retire、
  迟到 cancel 和最近身份防复用；fence 覆盖非零严格递增、耗尽及 release/acquire。
- 软件 drain 覆盖空、部分、满 64 帧和 held producer lease 晚发布；触及迭代上限不被
  当作“已清空”，也不把软件队列结果描述为 ALSA 已 drop 或扬声器已经静音。
- `AudioDspEngineConfig` 拒绝未指定/未知 reference source、未知 layout、未知 feature
  bit 和不完整的 v1 AEC/NS/BF/AGC mask；hardware/software reference 不能同时隐含启用。
- unavailable DSP 明确返回 backend unavailable，并使非成功输出 header 无效；测试
  fake 只验证 Configure/Arm/Disarm、generation、stale、discontinuity 和 backend fault，
  不得把复制测试波形描述成 AEC 后音频。
- WakeWord POD 结果覆盖 no-event、silence、detected、invalid、unavailable、backend
  error 和 fault 的字段一致性。score 只有在 available 时允许 0–1000；Snowboy 风格的
  detection 必须允许 `score_available=false` 且 score 为 0；reset 的 bool/error 组合
  也必须一致。
- unavailable wake engine 对合法 16 kHz mono frame 返回 backend unavailable，对非法
  格式返回 invalid frame，永不报告 detection。确定性 wake fake 只进入测试 target，
  不能成为生产 fallback；Snowboy `-2` 不作为 VAD 测试结论。
- vendor CMake 夹具验证 MPI、3A 和 Snowboy 三个 enable 选项默认关闭，并以不存在的显式
  输入确认 OFF 时零访问、零路径泄漏；host 即使伪造 `BOOMPI_TARGET_RV1106=ON` 也不能
  越过 cross Linux/ARM、固定 compiler 和 uClibc sysroot 检查。当前 pins 还要求显式
  feasibility opt-in 和 Debug-only 配置；Release 被拒绝。绝对路径、文件类型、缺失和
  SHA-256 不符均在 configure 阶段失败。默认测试不加载 Rockchip 库、Snowboy 模型或
  OpenBLAS。
- Linux 专用的 3A 合成 shared-object 夹具会在 `BOOMPI_BUILD_TESTS=OFF` 下执行默认 ALL，
  确认 link-check target 确实生成且动态段无 `RPATH`/`RUNPATH`；分别删除 init、process、
  destroy 任一入口时，链接必须失败。独立 MPI 夹具同样验证 tests-off 默认 ALL、无
  `RPATH`/`RUNPATH`，并分别删除代表性的 SYS、MB、AI、AO 入口确认链接失败。合成库只验证
  CMake 回归，不能替代真实 RV1106 交叉链接证据；非 Linux host 明确跳过 `.so` 链接用例。
- 独立的 RV1106 Debug/tests-off feasibility 构建已真实链接
  `boompi_rockchip_3a_link_check`。最终 ELF 保留 `libaec_bf_process.so`、
  `librkaudio_common.so` 的 `NEEDED` 和三个 `rkaudio_preprocess_*` `UND`；这只证明匹配
  header、库和目标 linker 的符号兼容，不是 host 测试，也没有运行或安装该 ELF。详见
  [2026-07-27 Rockchip 3A 交叉链接验证记录](p0-rockchip-3a-link-validation-20260727.md)。
- 2026-07-27 独立的 RV1106 Debug/tests-off feasibility 构建已真实链接
  `boompi_rockchip_mpi_audio_link_check`。当时最终 ELF 为 ARM EABI5 hard-float/uClibc，保留
  Rockit/MPP/RGA `NEEDED`、21 个 raw SYS/MB/AI/AO `UND`，且无 `RPATH`/`RUNPATH`；临时
  strip-debug 副本通过完整 ELF verifier。详见
  [2026-07-27 Rockchip MPI 音频交叉链接验证记录](p0-rockchip-mpi-link-validation-20260727.md)。
- 当前 link-check 与显式 `EXCLUDE_FROM_ALL` raw MPI HIL 精确加入 `RK_MPI_MB_GetSize` 后，
  均以 22 个 Rockchip MPI `UND` 完成匹配 GCC 8.3/uClibc 的真实交叉构建，动态段无
  `RPATH`/`RUNPATH`；
  默认 ALL 仍不构建 HIL，且没有安装或执行任一 ARM ELF。详见
  [2026-07-28 Rockchip MPI HIL 构建验证记录](p0-rockchip-mpi-hil-build-validation-20260728.md)。
- 同日只读 preflight fixture 9/9、完整 Python/script discovery 62/62 通过；真实板端扫描
  识别到 `rkipc` 的 22 个 `/dev/mpi/*` FD 并保持 `safe_to_execute=false`。HIL 自身的两次
  快照也已覆盖配置 PCM 与全部 `/dev/mpi/*`，但快照仍不等于排他。
- Host DSP 测试只能证明算法和内存边界；实际通道顺序、CPU 实时率和声音质量仍按
  HIL 闸门验证。

P2e-a 的完成条件仅是核心接口、失败关闭、测试 fake 和依赖闸门可重复验证。P2f-a 的
playback target 与 FIR 生成器只验证 pre-ALSA software PCM；P2f-b-a 又验证了 portable
sink、accepted-prefix ledger、committer 状态机和本地取消事务；P2f-b-b1 验证了固定
mailbox、producer/DSP 合约和 actor cancellation join；P2f-b-b2 验证了 deterministic
host worker core。P2f-c-a 又验证了 fake-device adapter；Linux opt-in 会对 ALSA `null`
执行真实 `snd_pcm_writei`、`snd_pcm_drop` 和 `snd_pcm_prepare`，但该插件只丢弃数字 PCM，
adapter 也尚未由 runtime 创建。当前没有实际播放线程/调度、物理 ALSA/Codec、network
producer endpoint、DSP endpoint、AEC reference
消费、normal-EOS presentation completion 或壳体声学 HIL。accepted（包括 EOS accepted）
不等于 presented、played 或 audible，本地 playback cancel ACK 也不等于扬声器静音或
DSP 历史已经复位。匹配 BSP 已关闭直接 Rockchip 3A 的固定 frame、`input_size` 和成功
输出长度，目标交叉链接与三个入口的符号解析也已通过；后续仍必须验证真实 packing/slot、
板端加载、错误恢复与实时率，再实现生产 adapter。
Snowboy 还需私有 legacy C ABI bridge、模型加载、
单线程 wake worker、500 ms pre-roll/VAD 和板端准确率/实时率测试。上述工作完成前不得
在 host 报告中写“ALSA 播放已接通”“AEC 已接通”“EOS 已播放完成”或“唤醒已通过”。

## 报告要求

记录操作系统、编译器、CMake、Go、Python 版本，以及实际执行的命令和结果。没有执行的检查明确写“未验证”，不能用“应该可用”代替。
