# P2f-c-a ALSA playback adapter 验证记录（2026-07-27）

> 历史状态：本记录对应的 `PcmPlaybackSink48k`、`AlsaPcmPlaybackDevice`、link-check 和
> `alsa-null-accepted-only` smoke 已于 2026-07-31 随未进入真实客户端 ELF 的通用播放层
> 一并删除。下列命令和 target 仅用于追溯当时证据，不属于当前构建或测试矩阵；现役路径是
> `AlsaSingleTurnIo` 和 `alsa-null-api-flow-only`。

- 时间：2026-07-27 19:58（Asia/Shanghai，UTC+08:00）
- 范围：`PcmPlaybackSink48k`、`AlsaPcmPlaybackDevice`、Linux `null` accepted-only
  smoke，以及 RV1106 tests-off 交叉链接检查。
- 证据边界：本记录没有连接或运行真实板卡，不证明 Codec、DAC、扬声器、声学输出、
  直接硬件路径 exact 或 runtime composition 已完成。

## Windows host

环境为 Visual Studio MSVC `/W4 /WX` 的 Debug 多配置构建：

```text
cmake --preset host-debug
cmake --build --preset host-debug --parallel --config Debug
ctest --preset host-debug --output-on-failure -C Debug
```

结果：16/16 CTest 通过。adapter fake-device 测试包括 full/partial、mono/stereo、
960-frame 上限、全部 typed code/native errno、写前/写后 status、两次 device clock、
malformed positive、timestamp/queue/timeline 边界和 `Drop`/`Prepare`。

## Ubuntu native ALSA

环境：Ubuntu GCC 11.4.0、CMake 3.22.1、`libasound2-dev` 1.2.6.1。

Debug 和 Release 均以 `BOOMPI_ENABLE_ALSA_PLAYBACK=ON`、
`BOOMPI_BUILD_TESTS=ON` 配置并执行：

```text
cmake -S . -B <linux-debug-or-release-build-dir> \
  -DCMAKE_BUILD_TYPE=<Debug-or-Release> \
  -DBOOMPI_BUILD_TESTS=ON -DBOOMPI_STRICT_WARNINGS=ON \
  -DBOOMPI_ENABLE_ALSA_PLAYBACK=ON
cmake --build <linux-debug-or-release-build-dir> --parallel
ctest --test-dir <linux-debug-or-release-build-dir> --output-on-failure
ctest --test-dir <linux-debug-build-dir> --output-on-failure \
  --no-tests=error -L alsa-null-accepted-only
```

结果：

- Debug：17/17 通过；label 定向检查 1/1 通过。
- Release：17/17 通过。
- `boompi_alsa_playback_link_check` 在两种配置下均进入默认 ALL 并完成链接。
- `null` smoke 完成 48 kHz、S16_LE、双声道、960-frame 一次正接受，再执行
  `Drop -> Prepare`；`null` 丢弃 PCM，因此只能作为数字 API/accepted 证据。

ASan/UBSan Debug 使用以下编译/链接标志：

```text
-fsanitize=address,undefined -fno-omit-frame-pointer
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
  ctest --test-dir <linux-asan-build-dir> --output-on-failure
```

结果：17/17 通过，未报告 sanitizer 错误。

## RV1106 交叉链接

环境为匹配当前 BSP 的 crosstool-NG GCC 8.3.0、uClibc EABI5 sysroot 和 ALSA 1.2.8。
私有工具链绝对路径不写入仓库；复现时通过已有变量提供：

```text
BOOMPI_RV1106_TOOLCHAIN_ROOT=<matching-bsp-toolchain> \
BOOMPI_RV1106_SYSROOT=<matching-bsp-sysroot> \
  cmake --preset rv1106-debug
cmake --build --preset rv1106-debug --parallel
```

结果：tests-off 默认 ALL 构建完成 `boompi-client` 和
`boompi_alsa_playback_link_check`。后者为 32-bit LSB ARM EABI5 动态 ELF，解释器为
`/lib/ld-uClibc.so.0`，动态依赖包含 `libasound.so.2`、`libstdc++.so.6`、
`libgcc_s.so.1` 和 `libc.so.0`。目标 `nm -C` 可见 `Open`、析构/Close、
`MonotonicNowUs`、`QueryStatus`、`TryWriteInterleavedS16`、`Drop` 和 `Prepare` 已进入
最终 ELF；这证明目标链接器解析了 adapter/ALSA/clock 依赖，但没有在板端执行该 ELF。

## 其他工程门禁

- 协议 fixture：通过。
- 两个 FIR 生成器 `--check`：通过。
- POSIX P0 探针测试：21/21 通过。
- Go 1.26.5：`go test ./...`、`go vet ./...`、`go build -trimpath` 均通过。
- clang-format dry-run 与 `git diff --check`：通过。
