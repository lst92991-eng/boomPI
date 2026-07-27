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

Linux/macOS 还需校验只读 P0 探针：

```text
sh -n scripts/probes/rv1106_p0_probe.sh
python3 -m unittest discover -s scripts/tests -p 'test_*.py' -v
```

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
- vendor CMake 夹具验证两个 enable 选项确实默认关闭，并用全部十个不存在路径确认
  OFF 时零访问、零路径泄漏；host 即使伪造 `BOOMPI_TARGET_RV1106=ON` 也不能越过
  cross Linux/ARM、固定 compiler 和 uClibc sysroot 检查。当前 pins 还要求显式
  feasibility opt-in 和 Debug-only 配置；Release 被拒绝。绝对路径、文件类型、缺失和
  SHA-256 不符均在 configure 阶段失败。该夹具在 Windows/Linux/macOS CI 运行，默认
  测试不加载 Rockchip 库、Snowboy 模型或 OpenBLAS。
- Host DSP 测试只能证明算法和内存边界；实际通道顺序、CPU 实时率和声音质量仍按
  HIL 闸门验证。

P2e-a 的完成条件仅是核心接口、失败关闭、测试 fake 和依赖闸门可重复验证。P2f-a 的
playback target 与 FIR 生成器只验证 pre-ALSA software PCM；P2f-b-a 又验证了 portable
sink、accepted-prefix ledger、committer 状态机和本地取消事务。scripted sink 返回的
accepted/timing 仍是 host 测试数据；没有执行真实 `snd_pcm_write*`、`snd_pcm_drop`、
`snd_pcm_prepare`、renderer/committer worker、控制 mailbox、DSP reset producer/join、
AEC reference 消费、normal-EOS presentation completion 或壳体声学 HIL。accepted 不等于
presented、played 或 audible，本地 playback cancel ACK 也不等于扬声器静音或 DSP 历史
已经复位。后续必须先从匹配 BSP 的真实头文件确认 Rockchip `input_size`、packing、
返回码和 reset，再实现 adapter；Snowboy 还需私有 legacy C ABI bridge、模型加载、
单线程 wake worker、500 ms pre-roll/VAD 和板端准确率/实时率测试。上述工作完成前不得
在 host 报告中写“ALSA 播放已接通”“AEC 已接通”“EOS 已播放完成”或“唤醒已通过”。

## 报告要求

记录操作系统、编译器、CMake、Go、Python 版本，以及实际执行的命令和结果。没有执行的检查明确写“未验证”，不能用“应该可用”代替。
