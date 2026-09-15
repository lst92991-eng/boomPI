# 2026-09-15 客户端与服务端重构交接

## 起点与工作区保护

- 分支：`codex/p1-fast-vertical-integration`。
- 开始时 HEAD：`d33795641e9b5ae7167576bc0c5fe232744aad2d`。
- 工作区已包含未提交的客户端、UI、音频模块、测试与教材修改；本轮沿用这些内容，没有回退至纪要中的 `ebc40d4`，也没有使用候选 ZIP 覆盖。
- 修改前在工作区外保存 `../refactor_work/handoff-20260915-start/`：HEAD、porcelain 状态、工作区和暂存区 binary patch，以及全部已跟踪/未忽略文件快照。统计以该快照为本轮起点，不能把此前的改动算作本轮删除量。
- 本轮未连接或部署开发板。交叉编译失败、host 替身测试、云端接口文档与真板验收分别记录。

## 实施边界

| 项目 | 已核对依据与决定 | 尚待验证 |
| --- | --- | --- |
| 声卡 | 仓库 ALSA 实现为 48 kHz / S16，Mode1 四槽 `[mic0,mic1,refL,refR]`，播放双声道；保留该配置 | 当前 Codec、I2S/TDM、BSP 与同时录放音是否允许 16 kHz；回采点、增益和延迟 |
| 3A | 实际调用 `rkaudio_preprocess_init/short/destory`，使用 `RKAUDIOParam`；保留 256 点块及 320 点交付适配 | 匹配 SDK 的 ABI、是否支持其他块长、闭源处理顺序、实际声学效果 |
| Snowboy / VAD | 保留 Snowboy 局部旧 ABI 桥、模型路径与参数；VAD 负值继续作为故障 | 安装模型实际格式、唤醒与分类效果、AEC 后近讲表现 |
| 网络音频 | 上下行均 16 kHz / S16_LE / mono，完整消息 320 samples；保留 generation、sequence、START/END/STOP | 新版本真实局域网与云端联动延迟 |
| 版本配套 | hello/ready 都必须声明整数 `sample_rate:16000`；旧 24 kHz 配套程序在握手时被拒绝 | 更换板端程序时须同时更新服务端 |
| TTS | 配套服务端直接请求 CosyVoice 16 kHz PCM；不在云端增加重采样 | 目标账户模型/音色权限、实际音色、首包延迟和云端取消 |

20 ms 是现有网络组帧单位，未用于推定 vendor 算法块长或声卡能力。数字回采并不代表麦克风听到的实际扬声器声音。

## 已删除与保留的机制

- 删除 batch ASR fallback、整句 PCM 备用副本及相关配置/HTTP 路径。实时 ASR 失败时当前轮明确失败，重新开始提问；这是一项异常场景行为变化，不能称为与旧版完全等价。
- 收敛客户端网络的轮次/连接组合布尔状态，仍保留上下行序号、下行 END 与 generation 过滤。
- 声学处理减少 vendor 输入/输出中转；播放直接转换至最终交错双声道，删除 mono 中间帧和重复缓冲。滤波尾音必须交付后才等待 ALSA drain。
- 应用主流程继续按“回复 → 录音/语句 → 触摸”展开；剔除重复输入复位，旧 generation 的 Barge 及其 PCM 不能替换当前回答。
- 网络 DONE 和声卡 PlaybackDone 分别记录；无论哪个先到，必须等齐才进入追问，极短回答播完后仍可接收当前轮晚到的字幕/错误。
- 必须保留：500 ms 句首历史、插话确认历史、跨线程有界交接、厂商块适配、1.5 s 播放队列、WSS 取消隔离与节拍/最后一帧判定。
- `AudioPipeline` 保留声卡/算法资源生命周期、断点恢复及 AEC 播放事实的组合职责。单设备服务端串行执行 provider 操作的边界保留，避免阻塞 socket 读取与取消。
- 应用的 Listening 与 Uploading 继续区分“尚未开口”和“正在发送”，Offline 继续表示连接不可用；没有用互相依赖的布尔值替代这些真实事实。显示状态仍将 Listening/Uploading 合并为聆听画面。
- 唤醒、VAD、流式问答、插话确认、三秒追问、音量、触摸/UI、配网、摄像头及断线处理均保留。服务端作为课程配套黑箱，不增加学生的服务端开发要求。

## 跨模块接口

| 改动 | 输入/输出和调用者 | 旧路径 |
| --- | --- | --- |
| hello/ready `sample_rate` | 客户端 VoiceLink 与 Go transport 双向校验 16000 | 旧版无采样率字段的握手被拒绝 |
| 下行 PCM 640 bytes | Go 帧编码 → VoiceLink → VoiceAudio → AudioTasks | 960-byte / 24 kHz 下行被拒绝 |
| 播放转换直接输出 stereo | AudioPipeline 调用 AudioConverter，输出有效 frames | 单声道中转与 MakeStereo 删除；EOS 仍取滤波尾音 |
| `Cancel(ctx, retract)` | Go session worker 同时取消当前轮并按需撤回历史 | 删除可选 CompletedResponseDiscarder 接口、类型断言与分开的公共操作 |

## 验证记录

| 验证 | 本轮结果 | 证据范围 |
| --- | --- | --- |
| Linux host 完整 CMake 构建 | 通过，严格警告检查；保留旧 BSP FFmpeg API 的 deprecated 警告 | 配置强制真实 WSS 测试；编译生产音频/ALSA glue，但板端完整可执行文件不在 host target 内 |
| CTest | 24/24 通过 | 应用54项行为断言、真实TLS/WSS、共享JSON/PCM、20个音频场景 |
| ASan / UBSan | 20个音频场景 + 应用行为通过 | 无检测到的越界/未定义行为；不是线程竞争或真板声学证明 |
| Python | 16/16 + 共享协议fixture通过 | 运行脚本与协议校验 |
| Go | 全套 test / vet，改动包 race 通过 | 本地HTTP/WebSocket云端替身，不调用付费API |
| C++ ↔ Go WSS | TestCppWSSHappyPath通过 | 真实TLS与协议联通，provider为替身 |
| Windows服务端构建 | CGO_ENABLED=0 / windows-amd64通过 | 生成单EXE，未启动或部署；跨Windows/WSL工作树的VCS探测失败后使用 `-buildvcs=false`，不嵌入自动Git标记 |

关键行为覆盖：pre-roll与实时帧不重不漏、末帧先发送后停录音、1/73/320样本尾脉冲、短回答、已取旧帧取消、END后取消、DONE与尾播双向先后、断线重连不恢复旧轮。Go取消必须先清旧ASR并安排下一轮准备，再通知完成；新增关闭门闩测试检查取消后立即提交的路径。音频线程harness替换整个AudioPipeline，真实重采样另做模块测试；生产Render→EOS→ALSA drain未运行真声卡，不能把组合路径仅编译说成运行验收。

本轮命令（Linux/WSL；Go使用项目原有 `build/teaching-tools/go/bin/go`）：

```sh
cmake -S . -B build/handoff-host-linux -G 'Unix Makefiles' \
  -DBOOMPI_BUILD_UI_SIMULATOR=OFF -DBOOMPI_REQUIRE_HOST_TRANSPORT_TEST=ON \
  -DBOOMPI_STRICT_WARNINGS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/handoff-host-linux --parallel 4
ctest --test-dir build/handoff-host-linux --output-on-failure
python3 scripts/verify_protocol_fixtures.py
python3 -m unittest discover -s scripts/tests -p 'test_*.py' -q
# sanitizer配置增加 -fsanitize=address,undefined -fno-omit-frame-pointer
ctest --test-dir build/handoff-sanitized -R audio-engine- --output-on-failure
ctest --test-dir build/handoff-sanitized -R voice-client-behavior --output-on-failure
cd server
../build/teaching-tools/go/bin/go test ./...
../build/teaching-tools/go/bin/go vet ./...
../build/teaching-tools/go/bin/go test -race ./internal/backend/qwenpipeline ./internal/session ./internal/app ./internal/transport
CGO_ENABLED=0 GOOS=windows GOARCH=amd64 ../build/teaching-tools/go/bin/go build \
  -buildvcs=false -trimpath -o ../build/handoff-release/boompi-server.exe ./cmd/boompi-server
```

云端协议、默认模型和音色的官方来源及旧配置维护提示见[服务端说明](../../server/README.md#云端依据与未验证项)。

本轮服务端EXE位于 `build/handoff-release/boompi-server.exe`，SHA-256：`f1341c23d8858a60d13b5b4696396be34510968ac6902260c294c057a196e2cb`。没有对应的新板端ELF，不能把该EXE与目录中的旧客户端当作已验收套件。

## 同口径体量与缓冲

统计起点是本轮开始时的完整工作区快照，不是旧纪要提交，也不是 HEAD 的干净源码。生产文件没有新增框架或模块；测试复用现有入口。ELOC 为非空、非注释行；纯注释行单列，混合代码/注释行按代码计。客户端包括入口、src、include 与私有硬件实现；排除模拟器、生成资源、第三方和构建目录。Go包括cmd/internal，测试另列。辅助统计脚本与原始JSON保存在工作区外 `../refactor_work/measure_handoff.py`、`handoff-metrics.json`；客户端可用仓库 `scripts/measure_client.py`复核。

| 范围 | 文件数 前→后 | 物理行 前→后 | ELOC 前→后 | 纯注释行 前→后 |
| --- | --- | --- | --- | --- |
| 客户端实现 | 19→19 | 6417→6448 | 4946→4970 | 1001→1008 |
| 客户端头文件 | 23→23 | 1423→1412 | 727→718 | 520→518 |
| 客户端测试/替身/HIL | 13→13 | 3664→3824 | 3051→3199 | 396→401 |
| Go服务端实现 | 21→21 | 4453→4209 | 4040→3814 | 113→108 |
| Go测试/替身/HIL | 24→24 | 3491→3513 | 3255→3286 | 17→16 |

客户端生产ELOC为5673→5688（+15）。删除的代码被必要的滤波尾音、严格格式校验和两个完成事实处理抵消，不能声称本轮客户端行数大幅减少。实际收益主要是缓冲、重复状态和取消路径收敛。

两端生产ELOC合计9713→9502（减少211行）；Go实现减少226行。没有以“减少50%”或某个千行规模验收。本轮未改动的22个UI、配置、摄像头、资源和网络配置文件已与起始快照逐字节比对一致，已有修改保留。

固定PCM数组按单套音频实例计算：

| 数组 | 原字节 | 新字节 | 减少字节 |
| --- | --- | --- | --- |
| 75槽TTS PCM | 72000 | 48000 | 24000 |
| 3A输入块 | 3072 | 1536 | 1536 |
| vendor输出中转 | 512 | 0 | 512 |
| Pipeline mono播放中转 | 3840 | 0 | 3840 |
| Converter播放scratch | 3840 | 0 | 3840 |
| Converter 24k补帧数组 | 960 | 0 | 960 |

实例内固定PCM数组减少34688字节；新增EOS共享只读静音640字节后，所列数组净减少34048字节。未把临时事件vector、标量、结构对齐、FFmpeg内部内存或RSS测量混入此数。服务端另删最多60秒的整句PCM备用副本及Commit复制，ASR32帧写队列/写线程和Actor8项下行队列；保留真正隔离阻塞的上行队列。

## 已知受限项

- 使用独立 `build/handoff-rv1106` 尝试 RV1106 配置，缺少 `BOOMPI_RV1106_TOOLCHAIN_ROOT` / `BOOMPI_RV1106_SDK_ROOT`，未生成新 ARM ELF，未做架构、loader、依赖或 RPATH 验证。
- 未连接开发板，未验收实际 16 kHz 硬件能力、Mode1 通道、AEC、自激、双讲、实时调度、音量和 UI/摄像头效果。
- 未使用真实 DashScope Key 调用付费接口；本地 mock provider 和真实 WSS 联测不证明云端模型权限、音色和实际延迟。

真板验收由用户指定时间，届时按[统一验收清单](host-validation.md)执行，重点验证短回答、句首尾字、播放中插话、尾播取消、追问与断线恢复。
