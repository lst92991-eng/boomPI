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

## 报告要求

记录操作系统、编译器、CMake、Go、Python 版本，以及实际执行的命令和结果。没有执行的检查明确写“未验证”，不能用“应该可用”代替。
