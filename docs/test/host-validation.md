# Host 验证

## 目的

Host 检查只验证工程结构、协议和不依赖 RV1106 硬件的确定性逻辑。通过 Host 测试不能替代
真实板端 ALSA、Codec、Rockchip 3A、Snowboy、显示或 Wi-Fi 验证，也不会访问付费 Qwen。

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

Windows 使用 Visual Studio 多配置生成器时，build/test 加 `--config Debug` 或 `-C Debug`。

Linux 安装 ALSA 开发头后（Debian/Ubuntu 为 `libasound2-dev`），再执行现役单轮 I/O smoke：

```text
cmake --preset host-debug -DBOOMPI_ENABLE_ALSA_PLAYBACK=ON
cmake --build --preset host-debug --parallel
ctest --preset host-debug --output-on-failure --no-tests=error -L alsa-null-api-flow-only
```

该 smoke 使用 ALSA `null`，只验证 `AlsaSingleTurnIo` 的 open/read/write/drop/drain API 流程；
`null` 会丢弃 PCM，不证明直接硬件路径、扬声器可听、全双工或 AEC。

服务端在 `server/` 目录执行：

```text
go test ./...
go vet ./...
go build -trimpath ./cmd/boompi-server
```

真实 Qwen smoke 必须由单独开关启用，并在报告中记录区域、模型和费用风险；默认测试不得
读取或打印 `DASHSCOPE_API_KEY`。

## 当前 CTest 矩阵

2026-07-31 清理未链接代码后，启用 ALSA 的 Host 矩阵为 9 项：

1. `boompi_host_tests`：配置、状态、UI/update 小型契约和共享协议 fixture。
2. `boompi_server_control_tests`：服务端控制消息解析与边界。
3. `boompi_audio_continuity_tests`：20 ms 帧连续性和 discontinuity。
4. `boompi_audio_fir_decimator_tests`：48→16 kHz FIR 系数、流式状态与幅度边界。
5. `boompi_audio_playback_resampler_tests`：24→48 kHz half-band 重采样。
6. `boompi_audio_playback_gain_limiter_tests`：volume、speaker gain、duck 和 limiter。
7. `boompi_audio_playback_renderer_tests`：现役 renderer 的帧顺序、EOS 和错误路径。
8. `boompi_snowboy_wake_word_engine_tests`：Snowboy adapter 与 legacy bridge fake。
9. `boompi_alsa_single_turn_io_null_smoke`：现役 ALSA API 流程，仅 Linux opt-in。

已删除且不得继续列入当前矩阵：通用 DSP frontend/channel mapper、SPSC playback buffer、
playback sink/adapter、committer/control/worker 和旧 ALSA adapter 的测试。历史记录可用于解释
为何删除，但不能作为当前代码覆盖率或产品能力。

## Vendor 与 HIL 离线闸门

Linux/macOS 继续校验只读探针和显式 opt-in HIL 脚本：

```text
sh -n scripts/probes/rv1106_p0_probe.sh
sh -n scripts/probes/rv1106_rockchip_mpi_audio_preflight.sh
sh -n scripts/hil/rv1106_alsa_full_duplex.sh
python3 -m unittest discover -s scripts/tests -p 'test_*.py' -v
```

- ALSA HIL 自动测试只使用 fake commands/临时目录，覆盖 dry-run、opt-in、PCM 占用、mixer
  恢复和有限 artifact；不能算真实全双工通过。
- raw MPI HIL 默认关闭且 `EXCLUDE_FROM_ALL`。离线/交叉链接只能证明 ABI 和符号闭包；真实
  板端还受 `/dev/mpi/*` owner、PCM owner 和 OEM 服务约束。
- Rockchip 3A HIL 的 fake shared object 验证 load/init/process/destroy 顺序和固定 profile；
  不证明算法效果、真实 slot、reference 或实时率。
- Snowboy host fake 不证明模型在目标 libc/libstdc++/ARM ABI 上的准确率和稳定性；板端已有
  唤醒功能证据，误唤醒、漏唤醒和长期 CPU/RSS 仍需独立验收。

详细边界见：

- [直接 ALSA 全双工 HIL 指南](p0-alsa-full-duplex-hil-guide.md)
- [Rockchip MPI HIL 构建验证](p0-rockchip-mpi-hil-build-validation-20260728.md)
- [Rockchip 3A HIL 构建验证](p0-rockchip-3a-hil-build-validation-20260729.md)
- [MPI HIL 只读前置验证](p0-rockchip-mpi-audio-preflight-20260728.md)

## 2026-07-31 清理验证

在 Ubuntu 临时副本使用 GCC 11.4、严格警告和 ALSA 开关执行：

```text
cmake -S . -B build/prune-host -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBOOMPI_BUILD_TESTS=ON \
  -DBOOMPI_STRICT_WARNINGS=ON \
  -DBOOMPI_ENABLE_ALSA_PLAYBACK=ON
cmake --build build/prune-host --parallel 4
ctest --test-dir build/prune-host --output-on-failure
```

结果：configure 通过、全部目标编译通过、CTest `9/9` 通过。此结果不包含 RV1106 交叉构建
或人声/声学测试。

## 报告要求

记录操作系统、编译器、CMake、Go、Python 版本，以及实际执行的命令和结果。未执行项明确
写“未验证”，不能用“应该可用”代替；日志不得包含 API key、device token、录音或完整对话。
