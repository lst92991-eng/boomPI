# boomPI

boomPI 是面向自研 RV1106 板卡的语音 AI 项目。板端运行 C++17 客户端，本地电脑运行跨平台 Go 服务端；服务端负责连接 Qwen 新加坡区，板端不保存云端 API Key。

> **语音链路的关键能力已经在第三块 RV1106 上完成组合验收，但当前候选仍不是发布版。**
> 2026-08-03 的单参考候选已完成严格交叉构建、固定语音 A/B、板端启动、真实 Qwen 问答以及
> 静默、完整播放、三秒追问和播放中打断四段真人验收；本次窗口未观察到自激或音频运行错误。
> 当前候选为 17 个生产 C++ 文件、2285 ELOC；扣除
> 439 ELOC 的 Rockchip/Snowboy vendor 集成后，
> 产品核心为 1846 ELOC。旧版的丢帧、高延迟、`EPIPE` 和 jitter queue 记录仍作为回归风险
> 保留；最终壳体 ERLE、受控 double-talk 和长期稳定性仍待量化验收。

## 系统形态

```text
双麦 / 扬声器 / 屏幕 / 触摸
              |
      RV1106 boompi-client
              |
      目标：局域网 WSS（当前开发链路经 Windows SSH tunnel）
              |
       boompi-server（Go）
              |
       Qwen Singapore API
```

第一版目标包括双麦 AEC、Snowboy 英文唤醒、可打断流式 TTS、三秒连续对话、横屏表情与字幕、以太网/Wi-Fi、首次配网与本地服务端配对。SC3336 多模态、在线音乐和长期记忆不属于第一版运行链路。

详细约束以 [AGENTS.md](AGENTS.md) 为准；架构摘要见 [docs/architecture/system-overview.md](docs/architecture/system-overview.md)，音频 vendor 边界见 [音频后端契约与依赖闸门](docs/architecture/audio-backends.md)，协议设计基线见 [protocol/protocol-v1.md](protocol/protocol-v1.md)。

## 当前真实状态

### 已有硬件单项结果

- 扬声器播放曾在真实板卡上跑通；历史记录曾把双麦基础采集标为通过，但当前可重复证据
  只确认临时 `DiffadcLR` 下两个动态 PCM slot，U9/U12 物理映射仍待验证。
- 以太网、Wi-Fi、ST7789P3、GT911 和 SC3336 已做过单项 bring-up。
- 双麦机械基线为正面横向 35 mm 中心距；扬声器位于双麦中点下方，推荐中心距 80 mm。

### 当前证据边界

- 当前板已验证 Mode1 下 direct ALSA `48 kHz/S16_LE` 真全双工：capture 为
  `[mic0,mic1,refL,refR]` 四通道，period/buffer 为 `960/1920`；playback 为双通道，
  period/buffer 为 `960/3840`，测试窗口无 xrun。997 Hz→`refL`、1499 Hz→`refR` 的相关系数
  均为 `0.9983`；物理扬声器主要使用 DAC-L，声学到达约 `14–17 ms`（阈值法近似）。
- 2026-08-01 历史 direct Rockchip 3A 已在板端以 `init(16000,16,2,2)`、
  `[mic0,mic1,refL,refR]`、1024-short 输入和 512-byte 输出通过固定帧调用。现行生产链路保留
  Mode1 四通道采集，但只把 `[mic0,mic1,refL]` 送入 3A：`init(16000,16,2,1)`、768-short
  输入，进入 vendor 前丢弃重复的 `refR`；不再维护软件 reference ring/60 ms lead。
- 当前生产 DSP profile 为 mask `1109`（FastAEC、AES、ANR、Dereverberation、STDT，vendor
  AGC 关闭），`ALC31/ref1/delay0` 仍是候选而非最终参数。同类无人声/嘈杂环境回归中，AGC
  开启 `n=5` 得到 `confirmed=4/5`、`follow=5/5`、`attempts=119`；关闭 AGC 累计 `n=10`
  得到 `confirmed=2/10`、`follow=3/10`、`attempts=43`。AGC OFF 明显改善但没有解决误触发。
- 播放期的主动硬参考探针通过硬静音、等待 reference 降低和二次 VAD 确认来抑制自激；这是
  产品侧 containment 候选，不是 AEC 效果证明。当前环境仍嘈杂，真人双讲和可控噪声条件下的
  打断验收待完成。
- Snowboy 已在第三块板加载模型并多次检测到唤醒；误唤醒、漏唤醒、准确率和长期 CPU/RSS
  尚未形成可重复基线。
- 最终壳体下的 AEC、波束形成、双讲、远场和最大音量表现。
- 经 Windows SSH tunnel 的 WSS 真实 Qwen 语音闭环曾跑通，服务端已切到连续
  `server_commit` TTS；正常局域网发现/配对、断网恢复、输入丢帧、首音延迟、播放稳定性、
  长期运行和 A/B 回滚仍未通过。

### 软件阶段

2026-08-01 板端生产目标按仓库既定职责整理为 17 个 C++ 文件、2285 ELOC；其中
Rockchip/Snowboy vendor 集成为 439 ELOC，产品核心为 1846 ELOC：
`application` 是会话状态机，`audio` 是有界播放编排，`network` 是持久 WSS/TLS，
`platform/rv1106` 是 ALSA/libswresample、Rockchip 3A、Snowboy 和 WebRTC VAD。它们直接组合
外部库，不再保留自写 WSS、重复 wire protocol、
通用 playback/capture 框架或未接入的 supervisor/UI/update 占位实现。精确范围、计数口径、
回滚位置和当前验证结果见
[语音客户端职责重排记录](docs/test/client-responsibility-layout-20260801.md)。

Mode1 四通道相关性和历史 2 mic + 2 ref direct 3A ABI、算法 profile、启动/退出和仍未关闭的
真人声学边界见
[P0 Mode1 硬件播放参考验证记录](docs/test/p0-mode1-hard-reference-validation-20260801.md)。

P0 已确认匹配 BSP 的 GCC 8.3/uClibc 工具链；direct ALSA 48 kHz 全双工、Rockchip 3A、
Snowboy 和现有 Go/Qwen 服务端均已进入真实板端链路。raw MPI AI+AO、最终壳体 AEC 效果、
远场指标和长期稳定性仍是独立 HIL 项，不能由本次源码收敛代替。
具体路径、哈希、两个 Mode 的区别和 HIL 顺序见
[2026-07-27 vendor 音频证据基线](docs/test/p0-vendor-audio-inventory-20260727.md)。
3A 交叉链接的命令、ELF 结果和严格边界另见
[2026-07-27 Rockchip 3A 交叉链接验证记录](docs/test/p0-rockchip-3a-link-validation-20260727.md)。
固定 profile、单帧调用顺序、离线 fake 和当前未执行边界另见
[2026-07-29 Rockchip 3A HIL 构建验证](docs/test/p0-rockchip-3a-hil-build-validation-20260729.md)。
MPI 音频的八个头文件 pin、当时 21 个 `UND`、MPP/RGA SONAME 与未运行边界见
[2026-07-27 Rockchip MPI 音频交叉链接验证记录](docs/test/p0-rockchip-mpi-link-validation-20260727.md)。

2026-07-31 已删除旧 `manual_single_turn`、自写 WSS/协议层、`AlsaSingleTurnIo`、独立
renderer/resampler/gain 组件和相应失效测试。当前只有 `VoiceClient`、`AudioEngine`、
`VoiceTransport`、`AudioBackend` 及薄配置/vendor 适配进入目标 ELF。旧 adapter 的验证结果仅作为历史证据保留在
[2026-07-27 ALSA playback adapter 验证记录](docs/test/p2f-c-a-validation-20260727.md)。
默认自动测试不会访问真实 Qwen，也不会消耗付费额度。

2026-07-28 已完成固定 OpenSSL 3.5.7 的 C++ WSS 单轮闭环和真实板端手动单轮：RV1106
从 `hw:0,0` 以 48 kHz/2ch 采集 slot 0，流式降采样到 16 kHz 后发送给离线 Go fake，
再将 24 kHz 提示音转换为 48 kHz 并完成 ALSA playback 写入与 drain。客户端和服务端
计数均通过，错误 pin 会在 provider 打开前失败；测试没有访问 Qwen。该入口尚未接入产品
状态机，也不证明 slot 0 信号质量、声学可听、真全双工、双麦或 AEC。证据与边界见
[P1 C++ WSS 单轮闭环验证记录](docs/test/p1-cpp-wss-client-validation-20260728.md)和
[RV1106 手动单轮 HIL 验证记录](docs/test/p1-rv1106-manual-single-turn-hil-validation-20260728.md)。

2026-07-29 的真实 Qwen 常驻语音闭环、连续 TTS 服务端、`voice9` 二进制哈希和未解决问题见
[Qwen Voice Loop HIL 快照](docs/test/qwen-voice-loop-hil-snapshot-20260729.md)。该记录明确标为
HIL 调试快照，不是 production release。

同日 direct ALSA 有界探针又完成两轮真实全双工：默认 `SingadcL` 时 transport 通过但
第二 slot 恒为 `-32768`；临时切到 `DiffadcLR` 后两个 slot 均出现非恒定样本，本次 PCM 聚合没有
`-32768/32767` 饱和值，并在测试后恢复原 mixer 值。板端实际仍是旧 `RV1106-Atguigu`
镜像，因此该结果不能替代正确自定义 BSP
复测，也不证明物理左右、极性或 reference。见
[P0 直接 ALSA 全双工验证记录](docs/test/p0-alsa-full-duplex-validation-20260728.md)。

## 仓库结构

```text
client/                 RV1106 C++17 板端客户端与 vendor HIL 探针
server/                 跨平台 Go 服务端
protocol/               板端/服务端共同遵守的 v1 wire contract 与 fixture
docs/architecture/      架构、边界和状态说明
docs/test/              host 与 RV1106 验证路径
scripts/                不依赖私有路径的辅助检查
third_party/            第三方接入说明；不提交未获许可的 vendor/model 二进制
```

## Host 构建与测试

`boompi-client` 当前是 RV1106/vendor-only 目标，host CMake 不再编译一套与真板不同的假音频
框架。Host 侧继续运行 Go 服务端测试、共享协议 fixture 校验和不打开硬件的 BSP 探针测试；
客户端验收使用匹配 GCC 8.3/uClibc 的交叉构建与真板 HIL。

```text
python scripts/verify_protocol_fixtures.py
python3 -m unittest discover -s scripts/tests -p 'test_*.py' -v
cd server && go test ./...
```

Linux/macOS 还会运行 P0 探针的离线脱敏回归：

```text
sh -n scripts/probes/rv1106_p0_probe.sh
python3 -m unittest discover -s scripts/tests -p 'test_*.py' -v
```

同一 Python 入口也使用全 fake ALSA/mixer 环境回归显式 opt-in 的
[直接 ALSA 全双工 HIL 工具](docs/test/p0-alsa-full-duplex-hil-guide.md)。自动测试不会打开
真实 PCM；真实执行与边界由独立的
[板端验证记录](docs/test/p0-alsa-full-duplex-validation-20260728.md)承接。

客户端 CI 应将纯协议/服务逻辑放在 Go/Python host 测试，把 vendor C++ 编译固定到匹配的
RV1106 交叉环境；不再维护一份 Windows/Linux 假设备客户端 target。

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

当前实现提供单设备 `wss://<host>:17806/ws`、稳定本地 TLS 身份、16 kHz PCM 上行、Qwen Realtime 流式转发、24 kHz PCM/文本下行与响应取消。开发阶段要求客户端在 `hello.payload.device_token` 中提交一个环境变量注入的共享令牌；服务端会在打开付费 provider 会话之前验证它。受控 reverse-tunnel 下的持久 WSS、真全双工、自动重连和板端语音状态机已接通；UDP 发现、六位码配对、每设备独立 Token、最终声学验收和完整生产信任链仍是后续工作。

Qwen 凭据只能通过当前进程环境提供：

- `DASHSCOPE_API_KEY`
- `DASHSCOPE_WORKSPACE_ID`

开发设备令牌同样只能通过当前进程环境提供：

- `BOOMPI_DEVICE_TOKEN`：32–256 字节、不得包含空白；第一版客户端与服务端配置相同的随机值

可在 PowerShell 中生成并配置当前终端的临时令牌：

```powershell
[byte[]]$tokenBytes = New-Object byte[] 32
$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
$rng.GetBytes($tokenBytes)
$rng.Dispose()
$env:BOOMPI_DEVICE_TOKEN = [Convert]::ToBase64String($tokenBytes)
```

Linux/macOS 可使用 `export BOOMPI_DEVICE_TOKEN="$(openssl rand -base64 32)"`。这只是首次配对功能落地前的开发保护，不应让多个部署共用同一令牌。

不要把 API Key 或设备令牌的真实值写进 YAML、`.env`、命令示例、日志、截图或 Git。任何曾出现在聊天或日志中的 Key 都应在控制台吊销并重新生成。默认 provider 使用新加坡区 `qwen3.5-omni-plus-realtime`，接入时仍必须重新核对官方 endpoint、事件和音频格式。

需要显式验证真实 Qwen 凭据和 WebSocket 握手时，在已导出上述变量的同一终端进入 `server/`，执行：

```text
BOOMPI_QWEN_LIVE_TEST=1 go test -count=1 -run '^TestLiveOpenSession$' ./internal/backend/qwen
```

该测试只建立并关闭会话，不上传音频，也不请求生成回答；默认 `go test ./...` 仍保持离线。

## RV1106 交叉编译

执行 `rv1106-*` preset 前必须显式准备：

1. 与目标镜像匹配的 RV1106 交叉编译器和 sysroot。
2. 已确认的 CPU ISA、hard/soft-float ABI、动态加载器、libc 和 libstdc++ 版本。
3. ALSA 开发头文件/库，以及经板端确认的声卡、PCM 和 mixer 参数。
4. Rockchip MPI/3A 的匹配头文件与二进制库；不得只从板端 `.so` 名称猜 API。
5. Snowboy runtime/model 的兼容性和再分发许可结论。
6. 固定 OpenSSL 3.5.7 的 RV1106 静态 package；源码、完整头文件树、CMake config 和
   archive 必须与仓库闸门一致。
7. Boost 1.74 兼容头文件；WebSocketpp 0.8.2 已以源码形式固定在 `third_party/websocketpp/`。
8. 工具链文件要求的 SDK/sysroot 环境变量或 CMake cache 参数；不得把个人绝对路径写入 preset。

当前工具链文件识别以下显式配置：

- `BOOMPI_RV1106_TOOLCHAIN_ROOT`：必填，目录中包含交叉编译器的 `bin/`。
- `BOOMPI_RV1106_TOOLCHAIN_PREFIX`：可选；当前默认值为 `arm-rockchip830-linux-uclibcgnueabihf`，必须与实际 SDK 一致。
- `BOOMPI_RV1106_SYSROOT`：接入目标系统库时必须指向与镜像匹配的 sysroot。
- `BOOMPI_OPENSSL_ROOT`：当前 RV1106 语音候选必填，指向通过固定哈希和完整头文件 manifest
  校验的 flat OpenSSL 3.5.7 静态 package root。
- `BOOMPI_BOOST_INCLUDE_DIR`：必填，指向 Boost 头文件根目录，不链接目标机 `libboost_system`。

`BOOMPI_ENABLE_ROCKCHIP_MPI_AUDIO`、`BOOMPI_ENABLE_ROCKCHIP_3A` 和
`BOOMPI_ENABLE_SNOWBOY` 默认均为 `OFF`。当前 pins
只用于可行性探针：必须同时核对 Linux/ARM 交叉编译、固定 RV1106 GNU compiler 和
uClibc sysroot，并显式设置 `BOOMPI_ALLOW_FEASIBILITY_AUDIO_VENDOR_INPUTS=ON`；仅
Debug-only 配置可继续逐项校验绝对路径和 SHA-256，Release 配置一律拒绝。启用任一
Rockchip 候选后，tests-off 默认 ALL 会链接对应的不安装、不自动执行的符号检查 target；
它不会自动搜索相邻 SDK、下载依赖或生成生产 adapter。MPI 的 MPP/RGA pins 对应
`media/out/lib` 未 strip 链接候选，不能用 OEM stripped 副本替代。详细 cache 输入及安全边界见
[音频后端契约与依赖闸门](docs/architecture/audio-backends.md)。

`BOOMPI_BUILD_ROCKCHIP_MPI_AUDIO_HIL` 也默认 `OFF`。它只在上述 pinned MPI Debug
feasibility 环境中创建 `EXCLUDE_FROM_ALL` 的原始 AI/AO 探针，默认 build、CTest、install 和
启动流程均不会构建或运行它。板端使用前必须阅读
[Rockchip MPI 原始音频 HIL 指南](docs/test/p0-rockchip-mpi-audio-hil-guide.md)。

准备完成后从全新 build 目录配置 Debug + `-O2` feasibility 候选，并显式传入上面列出的
toolchain/sysroot、Boost、OpenSSL、Rockchip 3A、Snowboy 和 WebRTC VAD 路径。当前 vendor pin
闸门刻意拒绝 Release；不得通过关闭 WSS/3A/Snowboy 生成一个冒充候选版的产物。已验证的
完整参数与结果见
[语音客户端职责重排记录](docs/test/client-responsibility-layout-20260801.md)。

2026-07-25 已使用与 BSP 匹配的 GCC 8.3.0 Buildroot wrapper 和 uClibc sysroot 成功构建 RV1106 Release 产物，并验证 ELF32 ARM EABI5 hard-float 与 loader；因当时板端管理通道不可用，产物尚未在板端执行。具体证据见 [P0 可行性报告](docs/test/p0-feasibility-report-20260725.md)，完整闸门见 [docs/test/rv1106-validation-gates.md](docs/test/rv1106-validation-gates.md)。刷镜像、改分区、设备树或启动项前必须单独取得用户授权。

2026-07-27 又在匹配 GCC 8.3/uClibc 环境完成 Rockchip 3A Debug/tests-off 默认 ALL
交叉链接：最终 ELF 保留 AEC/common `NEEDED` 与三个入口 `UND`。该目标没有运行或安装，
不代表板端 PCM、通道布局或 3A 效果通过；详见
[3A 交叉链接验证记录](docs/test/p0-rockchip-3a-link-validation-20260727.md)。

2026-07-29 新增显式 `EXCLUDE_FROM_ALL` 的 Rockchip 3A 固定帧 HIL：固定
`16 kHz / 256 samples / 2 mic + 1 ref / input_size=768 shorts`，只处理一帧内存合成输入。
Linux fake 6/6 和匹配 RV1106 严格交叉构建通过；清理 loader override 后，dry-run 不主动加载
vendor `.so`，真实调用只在双 opt-in 与安全前置检查后从固定 `/oem/usr/lib` 路径解析。产物未
复制或运行到当前旧镜像，不关闭物理
slot/reference、AEC 效果或实时率。详见
[3A HIL 构建验证](docs/test/p0-rockchip-3a-hil-build-validation-20260729.md)。

2026-08-01 当时的生产 profile 和 HIL 曾升级为 `2 mic + 2 ref`，并在第三块板以
`init(16000,16,2,2)`、1024-short 输入和 512-byte 输出完成 direct vendor 调用。上段
`2 mic + 1 ref` 同时保留为 2026-07-29 历史构建证据。2026-08-03 现行生产布局已固定为
`2 mic + refL`；Mode1 仍采集 `refR`，但在 vendor 输入边界丢弃它。

同日还完成 Rockchip MPI 音频 Debug/tests-off 默认 ALL 交叉链接：ELF32 ARM
hard-float/uClibc 产物保留 Rockit/MPP/RGA `NEEDED`、21 个 raw 生命周期 `UND`，且没有
`RPATH`/`RUNPATH`。该目标同样没有安装或执行；板端只读存在性也不等于全双工功能通过。
详见 [MPI 音频交叉链接验证记录](docs/test/p0-rockchip-mpi-link-validation-20260727.md)。

2026-07-28 当前 link-check 与显式 raw MPI HIL 已按 22 个精确符号完成真实 RV1106 交叉构建，
并通过离线闸门；两者均未在板端执行，详见
[MPI HIL 构建验证记录](docs/test/p0-rockchip-mpi-hil-build-validation-20260728.md)。

同日恢复 SSH 后，专用只读 preflight 完整扫描到 PCM owner 为 0，但运行中的 `rkipc` 持有
22 个 `/dev/mpi/*` FD；当前 `safe_to_execute=false`。C++ HIL 已同步在首次 MPI 调用前拦截
配置 PCM 和全部 `/dev/mpi/*` owner，但快照仍不等于排他。当前 OEM stop 链会结束
`udhcpc` 并停止整组 OEM service，禁止拿来自动跑 HIL；详见
[MPI HIL 只读前置验证记录](docs/test/p0-rockchip-mpi-audio-preflight-20260728.md)。

板端 SSH 可用时，可运行不会打开 PCM 或修改系统的脱敏探针：

```powershell
cmd /d /s /c "ssh <board-host> sh -s < scripts\probes\rv1106_p0_probe.sh"
```

Windows PowerShell 的 `Get-Content | ssh` 可能重编码 stdin 并注入 BOM/CRLF，因此这里使用
`cmd` 的二进制重定向。raw MPI HIL 前还必须运行更严格、同样零写入的专用 preflight：

```powershell
cmd /d /s /c "ssh <board-host> sh -s < scripts\probes\rv1106_rockchip_mpi_audio_preflight.sh"
```

只读盘点和运行库闭包通过后，再按
[直接 ALSA 全双工 HIL 指南](docs/test/p0-alsa-full-duplex-hil-guide.md)先 dry-run；真正测试必须
显式确认 PCM I/O、单个 DAC mixer 写入和短录音 artifact。脚本默认不会执行这些操作。
raw MPI 对照使用独立的
[Rockchip MPI 原始音频 HIL](docs/test/p0-rockchip-mpi-audio-hil-guide.md)：首轮只发送数字静音、
记录 AI frame metadata/MB capacity，不保存或分析语音，也不接入现有 production 音频层。

发布前使用 `scripts/probes/verify_rv1106_elf.py` 检查 strip 后的目标 ELF，拒绝
错误 ARM ABI、glibc、过高 GLIBCXX、RPATH/RUNPATH 和开发机绝对路径。

## 协议与隐私

- 板端和本地服务端最终使用 WSS；UDP 发现包本身不可信，必须经过六位码配对和 SPKI 固定。
- 音频采用二进制帧，控制事件采用 JSON；C++ 和 Go 必须读取同一份 [golden fixture](protocol/fixtures/protocol-v1-golden.json)。
- 断线或取消时丢弃当前 turn，不重传过期实时语音。
- 默认不保存原始录音、播放参考或完整对话文本，不自动上传遥测或崩溃信息。
- 生产代码不包含 Mock provider；deterministic fake 仅允许进入测试 target/package。

## 路线图

1. **P0 可行性闸门（进行中）**：direct ALSA Mode1 四通道全双工、capture/reference layout
   和历史 2 mic + 2 ref direct Rockchip 3A 板端调用已通过；现行生产改用 2 mic + refL，继续关闭
   raw rk_mpi、真人 double-talk、
   最终壳体声学、长期实时率，以及 Wi-Fi AP/UI backend 探测。
2. **P1 工程骨架（已完成）**：CMake/Go 目录、配置、日志、事件、共享协议 fixture 和基础 CI。
3. **P2 本地音频（进行中）**：只保留已进入真实客户端或直接支撑 HIL 的音频代码；以
   vendor raw PCM 最小闭环和板端实测为边界，不预建通用 worker/control 层。
4. **P3 服务端**：discovery、pairing、Qwen adapter、Session Actor 和 ToolRegistry。
5. **P4 端到端对话**：流式文字/音频、取消、上下文、断网和延迟测量。
6. **P5 UI 与配网**：表情、字幕、触摸、二维码和网络优先级。
7. **P6 产品化**：supervisor、局域网签名 A/B 应用更新、回滚与完整 HIL。
8. **P7 后续能力**：SC3336、多模态、音乐和其他 provider。

仓库许可证尚未确定。不要擅自添加许可证声明，也不要提交 Snowboy、Rockchip 或其他第三方二进制，除非来源、版本、校验和与再分发许可均已确认。
