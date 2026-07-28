# boomPI

boomPI 是面向自研 RV1106 板卡的语音 AI 项目。板端运行 C++17 客户端，本地电脑运行跨平台 Go 服务端；产品面向中国大陆市场，服务端默认连接 Qwen 中国北京区，板端不保存云端 API Key。服务端可以部署在新加坡，但 provider region 必须与 Key/Workspace 一致。

> **P1 工程骨架已完成；当前第一闸门是 vendor 音频最小闭环。** 下一步先用匹配 BSP 的
> `rk_mpi_ai`/`rk_mpi_ao` 与直接 ALSA 实测 48 kHz 全双工、真实通道布局和 Rockchip
> VQE/3A，再决定生产模块如何拆分。此前的 renderer、queue、playback control、committer
> 和 ALSA adapter 代码及测试保留为 host/交叉链接证据，但暂停扩展，不能冒充板端能力。

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
       Qwen China (Beijing) API
```

第一版目标包括双麦 AEC、Snowboy 英文唤醒、可打断流式 TTS、三秒连续对话、横屏表情与字幕、以太网/Wi-Fi、首次配网与本地服务端配对。SC3336 多模态、在线音乐和长期记忆不属于第一版运行链路。

详细约束以 [AGENTS.md](AGENTS.md) 为准；架构摘要见 [docs/architecture/system-overview.md](docs/architecture/system-overview.md)，音频 vendor 边界见 [音频后端契约与依赖闸门](docs/architecture/audio-backends.md)，协议设计基线见 [protocol/protocol-v1.md](protocol/protocol-v1.md)。

## 当前真实状态

### 已有硬件单项结果

- 扬声器播放和双麦基本采集曾在真实板卡上分别跑通。
- 以太网、Wi-Fi、ST7789P3、GT911 和 SC3336 已做过单项 bring-up。
- 双麦机械基线为正面横向 35 mm 中心距；扬声器位于双麦中点下方，推荐中心距 80 mm。

以上结果不代表以下事项已经通过：

- 48 kHz 真全双工、实际 capture 通道数和数字播放 reference slot。当前 DTB 的
  `TRCM clk-trcm=1` 只说明 TX/RX 共享 TX 时钟，不证明四通道；vendor VQE 样例选择的 loopback
  Mode2 也是另一项尚未在本板验证的 mixer 设置。
- Rockchip 3A 的板端加载、实际通道契约和 16 kHz 实时率；匹配 SDK 的交叉链接与三个
  入口符号解析已通过，但对应 ELF 尚未在板端执行。
- Snowboy 的板端模型加载、准确率和实时率；旧 ARM 库的交叉链接候选已核对。
- 最终壳体下的 AEC、波束形成、双讲、远场和最大音量表现。
- WSS 配对、断网恢复、端到端 Qwen 会话、长期稳定性和 A/B 回滚。

### 软件阶段

P1 已建立 CMake/Go 构建、配置校验、协议 fixture 和跨平台 CI。P0 已确认匹配 BSP 的
GCC 8.3/uClibc 工具链；Rockchip 3A 的 tests-off 默认 ALL 交叉链接和三个入口符号解析
已通过；Rockchip MPI 音频的 tests-off 默认 ALL 交叉链接也已解析 21 个 raw
SYS/MB/AI/AO 生命周期符号及 Rockit→MPP/RGA 依赖。Snowboy/OpenBLAS 仍是 ABI/链接候选；
板端能力仍是部分通过。当前镜像的只读探针只确认 `librockit.so`、AI/AO test、一个
capture PCM、一个 playback PCM 和直接 3A 库存在，同时确认 VQE JSON 缺失；探针没有
打开 PCM 或执行 vendor API。raw PCM 的实际参数、AI+AO 同时运行、VQE 资源安装和 3A
实时率尚未在当前镜像闭环验证。
具体路径、哈希、两个 Mode 的区别和 HIL 顺序见
[2026-07-27 vendor 音频证据基线](docs/test/p0-vendor-audio-inventory-20260727.md)。
3A 交叉链接的命令、ELF 结果和严格边界另见
[2026-07-27 Rockchip 3A 交叉链接验证记录](docs/test/p0-rockchip-3a-link-validation-20260727.md)。
MPI 音频的八个头文件 pin、21 个 `UND`、MPP/RGA SONAME 与未运行边界见
[2026-07-27 Rockchip MPI 音频交叉链接验证记录](docs/test/p0-rockchip-mpi-link-validation-20260727.md)。

已有 playback renderer/committer/worker/ALSA adapter 及其 host、Linux `null`、RV1106
交叉链接结果不会删除，详细证据保留在
[2026-07-27 ALSA playback adapter 验证记录](docs/test/p2f-c-a-validation-20260727.md)。
这些模块现在冻结，不继续增加 runner、mailbox 或控制层；后续首先完成 vendor raw PCM
最小闭环，再按真实阻塞、通道和时序需求复用或简化现有代码。默认自动测试不会访问真实
Qwen，也不会消耗付费额度。

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
python scripts/dsp/generate_fir_decimator_48_to_16.py --check client/src/audio/fir_decimator_48_to_16.cpp --quiet
python scripts/dsp/generate_playback_resampler_24_to_48.py --check client/src/audio/playback_resampler_24_to_48.cpp --quiet
```

Linux 安装 ALSA 开发包（Debian/Ubuntu 为 `libasound2-dev`）后，可显式启用 ALSA，
并用丢弃数字 PCM 的 `null` 插件验证 API/accepted 边界：

```text
cmake --preset host-debug -DBOOMPI_ENABLE_ALSA_PLAYBACK=ON
cmake --build --preset host-debug --parallel
ctest --preset host-debug --output-on-failure --no-tests=error -L alsa-null-accepted-only
```

该 smoke 不连接 Codec、DAC 或扬声器，不能写成 played/audible。

Linux/macOS 还会运行 P0 探针的离线脱敏回归：

```text
sh -n scripts/probes/rv1106_p0_probe.sh
python3 -m unittest discover -s scripts/tests -p 'test_*.py' -v
```

同一 Python 入口也使用全 fake ALSA/mixer 环境回归显式 opt-in 的
[直接 ALSA 全双工 HIL 工具](docs/test/p0-alsa-full-duplex-hil-guide.md)。自动测试不会打开
真实 PCM；板端物理链路恢复前，该工具只有 dry-run/离线证据，不能写成 48 kHz 已通过。

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

配置检查通过后，直接以前台进程启动最小服务端：

```powershell
# Windows
.\boompi-server.exe --config configs/config.yaml
```

```bash
# Linux/macOS
./boompi-server --config configs/config.yaml
```

当前实现提供单设备 `wss://<host>:17806/ws`、稳定本地 TLS 身份、16 kHz PCM 上行、Qwen Realtime 流式转发、24 kHz PCM/文本下行与响应取消。UDP 发现、六位码配对、设备 Token、自动重连和完整板端联调仍是后续工作；因此当前 WSS 可用于开发联调，但还不是完整的生产信任链。

Qwen 凭据只能通过当前进程环境提供：

- `DASHSCOPE_API_KEY`
- `DASHSCOPE_WORKSPACE_ID`

不要把真实值写进 YAML、`.env`、命令示例、日志、截图或 Git。任何曾出现在聊天或日志中的 Key 都应在控制台吊销并重新生成。默认 provider 使用中国北京区 `qwen3.5-omni-plus-realtime`；配置值 `china-beijing`，也可显式切换为 `singapore`。Key、Workspace 与 region 必须一致，接入时仍必须重新核对官方 endpoint、事件和音频格式。

需要显式验证真实 Qwen 凭据和端到端语音响应时，在已导出上述变量的同一终端进入 `server/`，准备一段不超过 10 秒的 16 kHz、单声道、S16_LE PCM，然后执行：

```text
BOOMPI_LIVE_QWEN=1 BOOMPI_LIVE_PCM=/absolute/path/to/16k-mono-s16le.pcm \
  go test -count=1 -run '^TestLiveRealtime$' -v ./internal/backend/qwen
```

该测试会上传音频并请求生成回答，可能产生费用；默认 `go test ./...` 仍保持离线。
如需保存返回的 24 kHz、单声道、S16_LE PCM，可额外设置
`BOOMPI_LIVE_OUTPUT_PCM=/absolute/path/to/output.pcm`；测试将输出限制在 60 秒以内。

## RV1106 交叉编译

执行 `rv1106-*` preset 前必须显式准备：

1. 与目标镜像匹配的 RV1106 交叉编译器和 sysroot。
2. 已确认的 CPU ISA、hard/soft-float ABI、动态加载器、libc 和 libstdc++ 版本。
3. ALSA 开发头文件/库，以及经板端确认的声卡、PCM 和 mixer 参数。
4. Rockchip MPI/3A 的匹配头文件与二进制库；不得只从板端 `.so` 名称猜 API。
5. Snowboy runtime/model 的兼容性和再分发许可结论。
6. 工具链文件要求的 SDK/sysroot 环境变量或 CMake cache 参数；不得把个人绝对路径写入 preset。

当前工具链文件识别以下显式配置：

- `BOOMPI_RV1106_TOOLCHAIN_ROOT`：必填，目录中包含交叉编译器的 `bin/`。
- `BOOMPI_RV1106_TOOLCHAIN_PREFIX`：可选；当前默认值为 `arm-rockchip830-linux-uclibcgnueabihf`，必须与实际 SDK 一致。
- `BOOMPI_RV1106_SYSROOT`：接入目标系统库时必须指向与镜像匹配的 sysroot。

`BOOMPI_ENABLE_ROCKCHIP_MPI_AUDIO`、`BOOMPI_ENABLE_ROCKCHIP_3A` 和
`BOOMPI_ENABLE_SNOWBOY` 默认均为 `OFF`。当前 pins
只用于可行性探针：必须同时核对 Linux/ARM 交叉编译、固定 RV1106 GNU compiler 和
uClibc sysroot，并显式设置 `BOOMPI_ALLOW_FEASIBILITY_AUDIO_VENDOR_INPUTS=ON`；仅
Debug-only 配置可继续逐项校验绝对路径和 SHA-256，Release 配置一律拒绝。启用任一
Rockchip 候选后，tests-off 默认 ALL 会链接对应的不安装、不自动执行的符号检查 target；
它不会自动搜索相邻 SDK、下载依赖或生成生产 adapter。MPI 的 MPP/RGA pins 对应
`media/out/lib` 未 strip 链接候选，不能用 OEM stripped 副本替代。详细 cache 输入及安全边界见
[音频后端契约与依赖闸门](docs/architecture/audio-backends.md)。

准备完成后使用：

```text
cmake --preset rv1106-release
cmake --build --preset rv1106-release --parallel
```

2026-07-25 已使用与 BSP 匹配的 GCC 8.3.0 Buildroot wrapper 和 uClibc sysroot 成功构建 RV1106 Release 产物，并验证 ELF32 ARM EABI5 hard-float 与 loader；因当时板端管理通道不可用，产物尚未在板端执行。具体证据见 [P0 可行性报告](docs/test/p0-feasibility-report-20260725.md)，完整闸门见 [docs/test/rv1106-validation-gates.md](docs/test/rv1106-validation-gates.md)。刷镜像、改分区、设备树或启动项前必须单独取得用户授权。

2026-07-27 又在匹配 GCC 8.3/uClibc 环境完成 Rockchip 3A Debug/tests-off 默认 ALL
交叉链接：最终 ELF 保留 AEC/common `NEEDED` 与三个入口 `UND`。该目标没有运行或安装，
不代表板端 PCM、通道布局或 3A 效果通过；详见
[3A 交叉链接验证记录](docs/test/p0-rockchip-3a-link-validation-20260727.md)。

同日还完成 Rockchip MPI 音频 Debug/tests-off 默认 ALL 交叉链接：ELF32 ARM
hard-float/uClibc 产物保留 Rockit/MPP/RGA `NEEDED`、21 个 raw 生命周期 `UND`，且没有
`RPATH`/`RUNPATH`。该目标同样没有安装或执行；板端只读存在性也不等于全双工功能通过。
详见 [MPI 音频交叉链接验证记录](docs/test/p0-rockchip-mpi-link-validation-20260727.md)。

恢复板端 SSH 后，可运行不会打开 PCM 或修改系统的脱敏探针：

```powershell
Get-Content -Raw scripts/probes/rv1106_p0_probe.sh | ssh <board-host> "sh -s"
```

只读盘点和运行库闭包通过后，再按
[直接 ALSA 全双工 HIL 指南](docs/test/p0-alsa-full-duplex-hil-guide.md)先 dry-run；真正测试必须
显式确认 PCM I/O、单个 DAC mixer 写入和短录音 artifact。脚本默认不会执行这些操作。

发布前使用 `scripts/probes/verify_rv1106_elf.py` 检查 strip 后的目标 ELF，拒绝
错误 ARM ABI、glibc、过高 GLIBCXX、RPATH/RUNPATH 和开发机绝对路径。

## 协议与隐私

- 板端和本地服务端最终使用 WSS；UDP 发现包本身不可信，必须经过六位码配对和 SPKI 固定。
- 音频采用二进制帧，控制事件采用 JSON；C++ 和 Go 必须读取同一份 [golden fixture](protocol/fixtures/protocol-v1-golden.json)。
- 断线或取消时丢弃当前 turn，不重传过期实时语音。
- 默认不保存原始录音、播放参考或完整对话文本，不自动上传遥测或崩溃信息。
- 生产代码不包含 Mock provider；deterministic fake 仅允许进入测试 target/package。

## 路线图

1. **P0 可行性闸门（进行中）**：先完成 rk_mpi/ALSA 48 kHz 全双工、真实 capture
   layout、Rockchip VQE/3A，再继续 Snowboy、WSS、Wi-Fi AP 和 UI backend 探测。
2. **P1 工程骨架（已完成）**：CMake/Go 目录、配置、日志、事件、共享协议 fixture 和基础 CI。
3. **P2 本地音频（进行中）**：现有 host 音频核心保留但冻结扩展；以 vendor raw PCM 最小
   闭环和板端 HIL 为当前入口，实测后再决定哪些已有模块进入 runtime。
4. **P3 服务端**：discovery、pairing、Qwen adapter、Session Actor 和 ToolRegistry。
5. **P4 端到端对话**：流式文字/音频、取消、上下文、断网和延迟测量。
6. **P5 UI 与配网**：表情、字幕、触摸、二维码和网络优先级。
7. **P6 产品化**：supervisor、局域网签名 A/B 应用更新、回滚与完整 HIL。
8. **P7 后续能力**：SC3336、多模态、音乐和其他 provider。

仓库许可证尚未确定。不要擅自添加许可证声明，也不要提交 Snowboy、Rockchip 或其他第三方二进制，除非来源、版本、校验和与再分发许可均已确认。
