# boomPI

boomPI 是面向自研 RV1106 板卡的语音 AI 项目。板端运行 C++17 客户端，本地电脑运行跨平台 Go 服务端；服务端默认连接 Qwen 中国内地（北京）区，板端不保存云端 API Key。

> **`v1.0.0` 是已完成真板人工验收的教学最终基线。** 双麦单参考 Rockchip 3A、Snowboy、
> WebRTC VAD、真实 Qwen 问答、连续流式播放、三秒追问、播放中同句打断、LVGL 触摸桌面、
> SC3336 本地预览、Wi-Fi 配网和实时音量控制已经形成闭环；本轮未观察到明显自激。
> 当前音频主线为 15 个生产文件、3078 ELOC，其中 vendor 集成 280 ELOC、产品胶水
> 2798 ELOC。LVGL/UI 2117 ELOC、NetworkBootstrap 183 ELOC 单独评审，板端生产 C/C++
> 总计 5378 ELOC。完整架构、依赖、构建、验收证据和后续边界见
> [boomPI v1.0.0 教学最终基线](docs/releases/v1.0.0.md)。最终壳体 ERLE、受控 double-talk、
> 长期稳定性和极限性能仍是后续量化优化项，不由本次主观验收代替。

## 系统形态

```text
双麦 / 扬声器 / 屏幕 / 触摸
              |
      RV1106 boompi-client
              |
      局域网 WSS（自动发现或显式地址）
              |
       boompi-server（Go）
              |
       Qwen China (Beijing) API
```

v1.0.0 包括双麦 AEC、Snowboy 英文唤醒、可打断流式 TTS、三秒连续对话、横屏动态语音球与字幕、以太网/Wi-Fi、首次二维码配网、本地服务端发现和 SC3336 本地预览。SC3336 多模态、在线音乐和长期记忆不属于第一版运行链路。

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
- 播放期的参考探针先以原音量连续确认六帧近讲，然后直接静音，等待 reference 降低和二次
  VAD 确认来抑制自激；失败后恢复播放并冷却，避免瞬态误判反复切断 TTS。这是
  产品侧 containment，不是最终 ERLE 证明。真人最终闭环已确认无明显自激且打断后新问题会
  正常提交；受控噪声、标准 double-talk 和最终壳体仍需量化。
- Snowboy 已在第三块板加载模型并多次检测到唤醒；误唤醒、漏唤醒、准确率和长期 CPU/RSS
  尚未形成可重复基线。
- 最终壳体下的 AEC、波束形成、双讲、远场和最大音量表现仍是后续量化项。
- 经 Windows SSH tunnel 的 WSS 真实 Qwen 语音闭环曾跑通，服务端已切到连续
  `server_commit` TTS；教学版 UDP 发现、SPKI 首次保存、以太网优先和 AP 配网页已实现。
  中国内地（北京）区真实 Qwen、真实 Wi-Fi、屏幕/GT911 和二维码配网已经人工跑通；断网恢复、
  量化首音延迟和长期运行继续作为上限测试。
- 最终客户端已部署并进入 `secure session ready`；屏幕、触摸、摄像头和音量滑块均已目视验收。
  正常语音/显示拓扑为 6 个长期执行上下文，摄像头开启后为 7 个。

### 软件阶段

v1.0.0 按仓库既定职责组织：
`application` 是会话状态机，`audio` 是有界播放编排，`network` 是持久 WSS/TLS，
`platform/rv1106` 是 ALSA/libswresample、Rockchip 3A、Snowboy 和 WebRTC VAD；同一
`network` 目录中的启动模块负责以太网/Wi-Fi/发现，`ui` 直接驱动 ST7789P3 和 GT911。
它们直接组合现有库，不再保留自写 WSS、重复 wire protocol、通用 playback/capture 框架或
supervisor/update 占位实现。当前音频主线精确计数为 15 个生产文件、3078 ELOC，其中 vendor
集成 280 ELOC、产品胶水 2798 ELOC。2026-08-04 严格交叉构建已经通过；stripped RV1106
v1.0.0 严格交叉构建客户端 SHA-256 为 `b8476d42a4520669ed02a8aabd52e5d829fafa5b9252d76bd0cd1d70cb245a37`，
完整自动化和人工验收证据见
[教学版软件收口记录](docs/test/software-closeout-20260804.md)。
2026-08-01 的
[语音客户端职责重排记录](docs/test/client-responsibility-layout-20260801.md)是纯语音阶段历史快照；
当前整体验证见 [教学版第一版集成验证](docs/test/teaching-v1-integration-20260803.md)。

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

`boompi-client` 当前是 RV1106/vendor-only 目标，host CMake 不生成一套与真板不同的假客户端。
Host 只把真实 `VoiceClient`/`AudioEngine` 与测试目录中的薄 fake 边界组合，确定性回归状态迁移、
背压、抖动重蓄水和有界退出；它不会打开 ALSA、屏幕或网络。最终客户端验收仍使用匹配
GCC 8.3/uClibc 的交叉构建与真板 HIL。

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

服务端以前台单文件程序运行，不要求安装数据库、容器或 Windows Service。教学部署只有三步：

1. 直接运行一次，程序在当前目录创建私有的 `config.yaml` 和 TLS 身份。
2. 打开 `config.yaml`，只填写 `qwen_api_key: "sk-..."`。
3. 再次运行；看到 `boomPI server starting` 且随后没有端口绑定错误后，即可让同一局域网内的板端自动发现。

所有非密钥字段都有可运行默认值。也可以用 `DASHSCOPE_API_KEY` 覆盖 YAML；有专属
Workspace 时再选填 `DASHSCOPE_WORKSPACE_ID`。教学版客户端和服务端内置相同的共享设备令牌，
只适合可信局域网；需要隔离时在两端设置同一个随机 `BOOMPI_DEVICE_TOKEN`。

当前实现提供 `wss://<host>:17806/ws`、稳定本地 TLS 身份、UDP `17807` 自动发现、16 kHz PCM
上行、默认 Qwen ASR → 对话模型 → 流式 TTS 组合链路、可选 Omni Realtime 直连、24 kHz
PCM/文本下行与响应取消。完整操作见
[服务端三步启动说明](server/README.md)。不要把真实 API Key、设备令牌、`config.yaml` 或
`state/` 提交到 Git；任何曾出现在聊天或公开日志中的 Key 都应在控制台吊销并重新生成。

需要显式验证真实 Qwen 凭据和 WebSocket 握手时，在已设置 `DASHSCOPE_API_KEY` 的终端进入
`server/`，执行：

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

准备完成后，把上述绝对路径设置为同名 `BOOMPI_*` 环境变量，然后只使用这一条板端构建入口：

```text
cmake --preset rv1106-candidate
cmake --build --preset rv1106-candidate --parallel
```

也可以在 Git 已忽略的 `CMakeUserPresets.json` 中继承 `rv1106-candidate` 并覆盖同名 cache
变量。仓库 preset 固定 Debug + `-O2`、ALSA、WSS、Rockchip 3A、Snowboy 和 WebRTC VAD；
工具链/sysroot 继续由 `BOOMPI_RV1106_TOOLCHAIN_ROOT` 和 `BOOMPI_RV1106_SYSROOT` 注入。
当前 vendor pin 闸门刻意拒绝 Release，所有 ABI 与 SHA-256 校验仍会执行；不得通过关闭
WSS/3A/Snowboy 生成一个冒充候选版的产物。已验证的完整参数与结果见
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

- 板端和本地服务端使用 WSS；教学版 UDP 发现包只给出端口和 SPKI，首次连接采用 TOFU 保存
  SPKI，只适用于可信局域网。
- 音频采用二进制帧，控制事件采用 JSON；[协议 fixture](protocol/fixtures/protocol-v1-golden.json)
  由 Python 校验器和 Go 协议测试读取，C++ 严格 JSON/PCM 契约由独立 target 验证。
- 断线或取消时丢弃当前 turn，不重传过期实时语音。
- 默认不保存原始录音、播放参考或完整对话文本，不自动上传遥测或崩溃信息。
- 生产代码不包含 Mock provider；deterministic fake 仅允许进入测试 target/package。

## 当前收尾与后置路线

当前教学版只按以下顺序收尾：

1. 修复确定性状态机、取消、背压、停止和协议问题，并用 Host harness 锁住回归。
2. 冻结源码，重算 ELOC，执行唯一 `rv1106-candidate` 严格交叉构建并检查 ELF/ABI/依赖。
3. 部署后由人工在同音量条件复验唤醒、VAD 句首、连续播放、尾播打断、double-talk、三秒追问、
   屏幕/触摸和真实 Wi-Fi 配网。
4. 记录分段延迟、XRUN/overrun/core、CPU/RSS 和有界稳定性结果；只有新记录能关闭本轮候选。

后置能力包括真实天气/资源数据、YOLO 与多模态、在线音乐、长期记忆，以及企业级配对、独立
supervisor、签名 A/B 更新、多设备管理和公网 OTA。它们不进入当前教学版阻断路径，也不提前建立占位模块。

仓库许可证尚未确定。不要擅自添加许可证声明，也不要提交 Snowboy、Rockchip 或其他第三方二进制，除非来源、版本、校验和与再分发许可均已确认。
