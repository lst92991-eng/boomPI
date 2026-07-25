# boomPI

boomPI 是面向自研 RV1106 板卡的语音 AI 项目。板端运行 C++17 客户端，本地电脑运行跨平台 Go 服务端；服务端负责连接 Qwen 新加坡区，板端不保存云端 API Key。

> **P1 工程骨架已完成，P0 可行性闸门正在补证。** Host 构建、测试、配置和协议边界已经建立；匹配 BSP 的 RV1106 交叉构建、Rockchip 3A ABI 和 Snowboy 加 OpenBLAS 的链接候选已验证。真实板端执行、四通道 AEC、WSS 配对、Wi-Fi 二维码配网和 A/B 更新尚未完成。本文不会把硬件单项测试或接口骨架描述成完整产品能力。

## 系统形态

```text
双麦 / 扬声器 / 屏幕 / 触摸
              |
      RV1106 boompi-client
              |
      局域网 WSS（规划中）
              |
       boompi-server（Go）
              |
       Qwen Singapore API
```

第一版目标包括双麦 AEC、Snowboy 英文唤醒、可打断流式 TTS、三秒连续对话、横屏表情与字幕、以太网/Wi-Fi、首次配网与本地服务端配对。SC3336 多模态、在线音乐和长期记忆不属于第一版运行链路。

详细约束以 [AGENTS.md](AGENTS.md) 为准；架构摘要见 [docs/architecture/system-overview.md](docs/architecture/system-overview.md)，协议设计基线见 [protocol/protocol-v1.md](protocol/protocol-v1.md)。

## 当前真实状态

### 已有硬件单项结果

- 扬声器播放和双麦基本采集曾在真实板卡上分别跑通。
- 以太网、Wi-Fi、ST7789P3、GT911 和 SC3336 已做过单项 bring-up。
- 双麦机械基线为正面横向 35 mm 中心距；扬声器位于双麦中点下方，推荐中心距 80 mm。

以上结果不代表以下事项已经通过：

- 48 kHz 全双工与 Codec Mode1 四通道回采。
- Rockchip 3A 的板端加载、实际通道契约和 16 kHz 实时率；SDK ABI 候选已核对。
- Snowboy 的板端模型加载、准确率和实时率；旧 ARM 库的交叉链接候选已核对。
- 最终壳体下的 AEC、波束形成、双讲、远场和最大音量表现。
- WSS 配对、断网恢复、端到端 Qwen 会话、长期稳定性和 A/B 回滚。

### 软件阶段

P1 已建立模块边界、构建入口、配置校验、测试支撑和协议 fixture，Windows/Linux/macOS CI 已通过。P0 的当前证据与阻断见 [2026-07-25 可行性报告](docs/test/p0-feasibility-report-20260725.md)。P2 已开始实现固定音频帧、预分配 SPSC lease、sequence/discontinuity 和连续性门禁；具体边界见 [音频运行时文档](docs/architecture/audio-runtime.md)，尚未连接 ALSA、3A 或 Snowboy。默认自动测试不得访问真实 Qwen，也不会消耗付费额度。功能完成情况必须以测试和板端记录为准，不能根据目录或接口名称推断。

## 仓库结构

```text
client/                 RV1106 C++17 客户端与 host 可测核心
server/                 跨平台 Go 服务端
protocol/               板端/服务端共同遵守的 v1 wire contract 与 fixture
docs/architecture/      架构、边界和状态说明
docs/test/              host 与 RV1106 验证路径
scripts/                不依赖私有路径的辅助检查
third_party/            第三方接入说明；不提交未获许可的 vendor/model 二进制
```

## Host 构建与测试

前置条件：

- Git。
- 支持 C++17 的编译器。
- CMake 3.21 或更高版本；如果主动选择 Ninja 生成器，还需安装 Ninja。
- Go 1.26.x，与 `server/go.mod` 一致。
- Python 3，用于校验共享协议 fixture；脚本只使用标准库。

标准入口：

```text
cmake --preset host-debug
cmake --build --preset host-debug --parallel
ctest --preset host-debug --output-on-failure
python scripts/verify_protocol_fixtures.py
```

Linux/macOS 还会运行 P0 探针的离线脱敏回归：

```text
sh -n scripts/probes/rv1106_p0_probe.sh
python3 -m unittest discover -s scripts/tests -p 'test_*.py' -v
```

Windows 使用支持所选编译器的 PowerShell/Developer PowerShell；Linux 和 macOS 使用系统 C++ 工具链。CI 会在 Windows、Linux 和 macOS 上执行同一组 host 检查。

Windows 如果 CMake 自动选择 Visual Studio 这类多配置生成器，构建和测试时显式选择 Debug：

```text
cmake --build --preset host-debug --parallel --config Debug
ctest --preset host-debug --output-on-failure -C Debug
```

## Go 服务端

在 `server/` 目录执行：

```text
go test ./...
go vet ./...
go build -trimpath ./cmd/boompi-server
```

服务端第一版以前台终端程序运行，不要求安装 Windows Service。运行前应从 `server/configs/config.example.yaml` 创建本机配置，并先执行：

Windows：

```text
.\boompi-server.exe --check-config --config configs/config.yaml
```

Linux/macOS：

```text
./boompi-server --check-config --config configs/config.yaml
```

Qwen 凭据只能通过当前进程环境提供：

- `DASHSCOPE_API_KEY`
- `DASHSCOPE_WORKSPACE_ID`

不要把真实值写进 YAML、`.env`、命令示例、日志、截图或 Git。任何曾出现在聊天或日志中的 Key 都应在控制台吊销并重新生成。默认 provider 计划使用新加坡区 `qwen3.5-omni-plus-realtime`，接入时仍必须重新核对官方 endpoint、事件和音频格式。

## RV1106 交叉编译

执行 `rv1106-*` preset 前必须显式准备：

1. 与目标镜像匹配的 RV1106 交叉编译器和 sysroot。
2. 已确认的 CPU ISA、hard/soft-float ABI、动态加载器、libc 和 libstdc++ 版本。
3. ALSA 开发头文件/库，以及经板端确认的声卡、PCM 和 mixer 参数。
4. Rockchip 3A 的匹配头文件与二进制库；不得只从板端 `.so` 名称猜 API。
5. Snowboy runtime/model 的兼容性和再分发许可结论。
6. 工具链文件要求的 SDK/sysroot 环境变量或 CMake cache 参数；不得把个人绝对路径写入 preset。

当前工具链文件识别以下显式配置：

- `BOOMPI_RV1106_TOOLCHAIN_ROOT`：必填，目录中包含交叉编译器的 `bin/`。
- `BOOMPI_RV1106_TOOLCHAIN_PREFIX`：可选；当前默认值为 `arm-rockchip830-linux-uclibcgnueabihf`，必须与实际 SDK 一致。
- `BOOMPI_RV1106_SYSROOT`：接入目标系统库时必须指向与镜像匹配的 sysroot。

准备完成后使用：

```text
cmake --preset rv1106-release
cmake --build --preset rv1106-release --parallel
```

2026-07-25 已使用与 BSP 匹配的 GCC 8.3.0 Buildroot wrapper 和 uClibc sysroot 成功构建 RV1106 Release 产物，并验证 ELF32 ARM EABI5 hard-float 与 loader；因当时板端管理通道不可用，产物尚未在板端执行。具体证据见 [P0 可行性报告](docs/test/p0-feasibility-report-20260725.md)，完整闸门见 [docs/test/rv1106-validation-gates.md](docs/test/rv1106-validation-gates.md)。刷镜像、改分区、设备树或启动项前必须单独取得用户授权。

恢复板端 SSH 后，可运行不会打开 PCM 或修改系统的脱敏探针：

```powershell
Get-Content -Raw scripts/probes/rv1106_p0_probe.sh | ssh <board-host> "sh -s"
```

发布前使用 `scripts/probes/verify_rv1106_elf.py` 检查 strip 后的目标 ELF，拒绝
错误 ARM ABI、glibc、过高 GLIBCXX、RPATH/RUNPATH 和开发机绝对路径。

## 协议与隐私

- 板端和本地服务端最终使用 WSS；UDP 发现包本身不可信，必须经过六位码配对和 SPKI 固定。
- 音频采用二进制帧，控制事件采用 JSON；C++ 和 Go 必须读取同一份 [golden fixture](protocol/fixtures/protocol-v1-golden.json)。
- 断线或取消时丢弃当前 turn，不重传过期实时语音。
- 默认不保存原始录音、播放参考或完整对话文本，不自动上传遥测或崩溃信息。
- 生产代码不包含 Mock provider；deterministic fake 仅允许进入测试 target/package。

## 路线图

1. **P0 可行性闸门（进行中）**：工具链、Snowboy、Rockchip 3A、四通道参考、WSS、Wi-Fi AP 和 UI backend 探测。
2. **P1 工程骨架（已完成）**：CMake/Go 目录、配置、日志、事件、共享协议 fixture 和基础 CI。
3. **P2 本地音频（进行中）**：48/16 kHz 链路、AEC/BF/VAD、Snowboy、播放和打断。
4. **P3 服务端**：discovery、pairing、Qwen adapter、Session Actor 和 ToolRegistry。
5. **P4 端到端对话**：流式文字/音频、取消、上下文、断网和延迟测量。
6. **P5 UI 与配网**：表情、字幕、触摸、二维码和网络优先级。
7. **P6 产品化**：supervisor、局域网签名 A/B 应用更新、回滚与完整 HIL。
8. **P7 后续能力**：SC3336、多模态、音乐和其他 provider。

仓库许可证尚未确定。不要擅自添加许可证声明，也不要提交 Snowboy、Rockchip 或其他第三方二进制，除非来源、版本、校验和与再分发许可均已确认。
