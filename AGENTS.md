# boomPI 开发约定

## 当前变更：2026-09-16 紧凑插话修复

用户已授权针对真板自打断重新设计紧凑复核并推送测试版。本节覆盖第9节历史阶段中“删除静音试探/参考复核/冷却、共用完全相同准入、约1200行目标”的限定，其他边界保持。

- 普通提问与插话共用PCM/pre-roll和交付，但播放期间允许候选→有界复核；确认前不得退休旧轮次。
- 复核暂停消费TTS而非乘零吞字；拒绝/超时恢复，确认后直接取消旧播放。不得恢复旧聚合层或新增通用框架。
- 只保留实际消费者需要的参考与已写静音观测；VAD/Snowboy仍由输入线程独占。允许主线程输出无敏感数据的复核诊断。
- 以实际增量和数据/资源边界验收，不为1200行硬上限删除必要逻辑。实现、接口、容量、验证边界见 [紧凑插话记录](docs/test/compact-barge-in.md)。不得把Host/CI通过写成零自激或完整声学验收。

本仓库是 RV1106 语音 AI 教学项目。目标是让学生能顺着真实数据流读懂、构建和调试完整产品，而不是展示企业级框架。

## 1. 当前产品边界

已实现并需要保持：

- 双麦、单参考 Rockchip 3A；
- Snowboy 唤醒、WebRTC VAD、500 ms pre-roll；
- 持久 WSS、Qwen ASR/对话、CosyVoice 16 kHz 流式 TTS；
- 播放中打断旧回复并立即提交新 generation，三秒追问；
- LVGL 320×240 桌面、静态小智表情、字幕、触摸和音量；
- 以太网优先、Wi-Fi 配网和 SC3336 本地预览。

YOLO、在线音乐、长期记忆、账号系统、OTA 和多模态上传暂不实现，也不为它们预留空接口。

## 2. 简化原则

1. 优先调用已经验证的 ALSA、Rockchip 3A、Snowboy、WebRTC VAD、WebSocket++、LVGL 和 Qwen。
2. 抽象必须对应真实硬件、协议或所有权边界。禁止为未来需求增加 manager、adapter、committer、通用 worker 或消息总线。
3. 一个状态只能有一个 owner。实时线程只做固定成本工作，跨线程队列必须有容量和明确的满队列语义。
4. 教学可读性优先：顺序代码、小函数和数据流注释优于模板技巧、宏分支和过度防御。
5. 不静默丢 PCM、掩盖 sequence hole、无限重试或无限排队。超时、有界缓冲、TLS 和硬件恢复仍需保留。
6. 硬件与vendor参数根据匹配资料和可复现证据修改；业务策略可按用户需求重写。策略改变的真实声学效果必须另验，不能用Host结果代替。

## 3. 目录职责

```text
client/src/application/      顺序问答主流程与语句交付
client/src/audio/            namespace语句整理、采集任务、播放任务与各自必要缓冲
client/src/config/           板端环境配置
client/src/network/          网络选择、发现和持久 WSS
client/src/platform/rv1106/  ALSA、重采样、3A、Snowboy、VAD
client/src/ui/               LVGL、触摸和摄像头预览
server/internal/app/         服务组合与单设备会话
server/internal/backend/     Qwen pipeline 和唯一 provider 接口
server/internal/protocol/    与客户端配套的START/PCM/END/CANCEL协议
protocol/                    跨语言线协议
```

这些是职责目录，不是 App/Driver/Inf 模板。不要增加第二套状态机、协议实现、音频队列或可切换产品 backend。

## 4. 音频事实

```text
capture:  48 kHz / S16_LE / 4 ch / 20 ms
layout:   [mic0,mic1,refL,refR]
3A input: 16 kHz [mic0,mic1,refL]
3A output:16 kHz mono
TTS:      16 kHz mono → 48 kHz stereo playback
```

TTS 左右声道相同，所以 `refR` 不进入 Rockchip 3A。四通道原始采集仍保留给硬件诊断。vendor AGC 关闭，避免与硬件/数字增益叠加。

以上声卡参数是当前实现契约，不代表本轮完成真板验收。全链 16 kHz 是目标；在 Codec、I2S/TDM、驱动、Mode1 回采和同时录放音得到匹配真板证据前，保留底层 48 kHz 适配。数字回采点与增益/延迟仍需 BSP 及测量证据。

3A 使用 `rkaudio_preprocess_init/short/destory` 与 `RKAUDIOParam` 参数树；当前按 256 点 vendor 块衔接 320 点交付，不与 RKAP/.bin API 混用，也不把功能位顺序当作闭源执行顺序。20 ms 是当前业务与网络单位，不要求 vendor 块或未来 ALSA period 与它相等。

默认参数：

```text
Snowboy sensitivity = 0.7
playback volume     = 60%
pre-roll            = 500 ms
```

打断成功的定义是：确认近讲、停止旧 TTS、以新 generation 的 START(supersede)退休旧回复，并把保留的近讲 PCM 作为新 turn 提交。不等待 cancel ACK；仅停止扬声器不算成功。

## 5. 线程所有权

下表描述当前分工，不冻结线程数。调整必须保持资源单一所有者、实时采集与阻塞I/O隔离，不为减少线程数增加复杂控制。

| 上下文 | 唯一职责 |
| --- | --- |
| application主线程 | speech语句整理、四态问答与追问窗口 |
| 输入线程，`SCHED_FIFO 40` | 顺序读取、转换、3A、wake、VAD与PCM交付 |
| playback，`SCHED_FIFO 30` | TTS ring、重采样、ALSA write/drop/drain |
| voice_net网络线程 | 网络建链、TLS/WebSocket、握手心跳重连和协议事件 |
| UI worker | 所有 LVGL、触摸和音量配置写入 |
| camera worker | SC3336 拉流和固定大小帧交接 |

UI、网络和文件写入不得进入 ALSA 热路径。`SCHED_FIFO` 失败时记录 warning 并继续，不能盲目提高 UI 或网络优先级。

## 6. 服务端与安全

- 服务端是无数据库、`CGO_ENABLED=0` 的单个 Go 程序。
- 第一次运行提示输入中国内地 DashScope Key，生成 `config.yaml` 和稳定 TLS 身份后直接启动。
- 普通学生只配置 Key；模型等高级参数才进入 YAML。
- WSS 使用自动 TLS 身份和客户端 SPKI 保存/校验；hello 使用代码内固定教学口令。
- 该模型只适用于可信课堂局域网。不要增加账号、证书后台或配对数据库，也不要把端口暴露到公网。
- API Key 只在服务端配置中存在，不进入源码、日志、测试 fixture 或板端。
- 默认不保存 PCM、Wi-Fi 密码和完整对话。

## 7. UI 与网络

- LVGL 8.2，320×240；ST7789P3 SPI 目标 80 MHz，GT911 触摸。
- 小智页面只用静态表情表达状态，不恢复动态语音球。
- application 只发布小型 UI 状态；UI worker 独占 LVGL 对象。
- 音量实时值只更新原子 gain，提交时才由 UI worker 持久化。
- 点击小智与唤醒词进入同一状态机；离开摄像头页必须释放预览资源。
- 以太网优先、Wi-Fi 备用；SSID 和密码不得进入普通日志。

## 8. 构建

Host：

```sh
cmake --preset host-debug
cmake --build --preset host-debug --parallel
ctest --preset host-debug
python3 scripts/verify_protocol_fixtures.py
python3 -m unittest discover -s scripts/tests -p 'test_*.py' -v
cd server && go test ./... && go vet ./...
```

RV1106：

```sh
cmake --preset rv1106-release
cmake --build --preset rv1106-release --parallel
```

工具链、sysroot 和外部库优先通过 `BOOMPI_RV1106_SDK_ROOT` 及教师维护的 SDK 清单注入；已有 `BOOMPI_*` 或 Git 忽略的 `CMakeUserPresets.json` 仍可用于维护。当前终端缺少变量时，先查已有本地preset及构建主机记录，不直接判定SDK不存在。不要搜索相邻 SDK，不要提交个人绝对路径、模型、vendor 二进制、日志、core、生成配置或 build 目录。

Snowboy的wake.cpp单独使用旧 C++ ABI；不得把 `_GLIBCXX_USE_CXX11_ABI=0` 扩散到整个客户端。

## 9. 代码和体量

用户于 2026-09-14 提供 [XIAOZHI_CODE](https://github.com/lst92991-eng/XIAOZHI_CODE) 和 [vscode_motor_gateway](https://github.com/lst92991-eng/vscode_motor_gateway) 作为教学风格参考。具体入口、版本和观察见 [样例分析](docs/architecture/client-readability-refactor.md)。后续重构先对照其 App_Application、App_Audio、App_task 的真实业务流程：

- XIAOZHI_CODE 只参考实际语音业务代码；排除 test/text、演示入口、测试图像/动画（含夹在 App_Display 内的 TestImage 代码）、生成资源及构建输出。先筛选业务调用链，不依据目录叫 App 就把测试实现纳入风格依据。
- 让初始化顺序和任务内“读取 → 处理 → 交付”可见。函数名说明模块、动作和必要的数据去向；不能只用主函数行数、短名字或把代码移到别处来判断可读性。
- 允许简短中文步骤注释帮助初学者跟读；同一事实只解释一次，避免长段接口说明反复遮住代码。
- 保持现有模块内命名一致；模仿样例时不增加 App_* 转发外壳或机械复制目录。普通函数或简洁类按实际责任选用，官方 C++ 库及其必需边界继续保留。
- 样例的芯片、RTOS、音频编码、阻塞方式和算法参数不直接移植到 RV1106。用户功能、第三方库不可修改、学生零声学校准及接口变更汇报继续有效；旧内部接口和旧策略不是兼容性要求。

- 本次交付必须完成结构重写：应用直接调用namespace模块，旧VoiceAudio/AudioEngine/AudioBackend及更名后的AudioTasks/AudioPipeline组合转发层均删除，不在旧对象上再包namespace。旧内部接口不构成兼容性要求。
- 保留App_Init/App_Process/App_Close作为产品入口，主流程显式读取采集结果、调用speech语句整理、提交voice_net、操作playback。speech不持有音频线程、不接收/转发下行PCM、不管理声卡生命周期。
- ALSA只配置/读取/关闭原始PCM。输入任务顺序执行读取、必要格式适配、3A、wake和VAD；应用处理语句和交付。算法间不排队，网络等待不能进入输入任务。
- 普通提问、追问和插话共用一次语句确认与pre-roll。删除静音试探、参考下降复核、二次确认、专用历史裁剪与冷却；额外软件预热、尾音和电平门控不作为硬件约束冻结；简单语句策略的真实声学效果以上板体验验收。
- wake/VAD由输入线程连续处理；断流复位受影响算法，外部VAD句尾用一个无回执标志通知Snowboy Reset（厂商API要求）。speech不调用vendor，不恢复通用listener命令槽、arm或阻塞握手。
- 主业务状态优先Idle/Listening/WaitingReply/Speaking；连接是否在线由网络拥有，上传是否已START由语句/协议边界明确表达，不复制成另一套业务状态。
- 保持两路PCM配置后才首次采集，线程退出后才释放资源；初始化顺序可按本轮职责调整，不固定旧namespace命名或旧内部接口。
- namespace必须承载原实现与资源，不能只转发旧类；移除失效源文件、公共头、PImpl生命周期和CMake选源。必要第三方对象及直接硬件资源所有权可保留，硬件事实和失败回收不得因改成普通函数而丢失。
- 官方源码、第三方库源码和库二进制不修改；兼容问题只在自有桥接、调用和构建配置中解决。
- 学生侧不设置声学校准参数。必要硬件事实与仍使用的声学预置保留在内部 `client/src/platform/rv1106/board_voice_profile.h`，不通过公共音频配置对象传递。
- 新增、重命名或删除跨模块接口时，逐项汇报用途、输入输出、调用方和旧接口去向。优先复用，不新增通用框架。
- App入口继续使用App_前缀；新语音namespace公共函数使用open/read/process/update/write/close等简单snake_case，字段snake_case，枚举项PascalCase，常量k前缀。UI等无关模块沿用原命名。第三方/C ABI遵循原契约。
- 语句整理直接交付已有PCM，句首与实时帧不重不漏；取消或发送失败后不再访问本批借用。返回结构按实际数据流设计，不为旧事件接口保留额外PCM副本。
- voice_net直接复用VoiceClientConfig。线上START/END/CANCEL与PCM分开，generation和sequence继续承担在途隔离与缺帧校验；DONE和实际尾播完成必须区分。协议变更同步更新两端和共享fixture，不保留旧双栈。
- 当前BPV4使用固定文本命令；后续可因具体需求修改，必须两端配套。轮次号和sequence由网络分配及校验；应用不重复保存轮次号，播放以取消完成屏障隔离旧I/O。仅在实际有消费者时传递附加事实，不为已删除的声学门控保留播放快照。
- 原子变量默认采用标准内存顺序，只有测量证明有必要时再引入显式内存顺序优化。
- 正常流程不逐轮打印状态与统计；错误和必要的恢复提示保留在实际负责的边界。删除仅为已移除日志服务的计数状态。
- C++17 与 `gofmt`；数值名带单位，例如 `*_ms`、`*_frames`、`*_dbfs`。
- 注释解释硬件事实、并发所有权、时序和“为什么”；教学主流程允许简短步骤注释，避免给每条显然赋值附上重复说明。
- 错误必须指出阶段且不输出 secret；返回值不能混合背压、断线和协议错误。
- 头文件只暴露必要边界，不跨层 include 私有 vendor 头。
- 不用压缩排版、合并语句或生成代码伪造低行数。
- 本阶段纯音频代码目标约1200物理行：采集、3A、唤醒、VAD、语句/pre-roll、播放、输入处理循环，以及直接服务于它们的头文件、重采样和线程适配均计入。问答应用、WSS、CLI、配置、UI和服务端单列，不把其总量当作音频预算；同职责文件合计物理行/ELOC，不通过移动实现或压缩排版达标。
- 声学参数仅由 board_voice_profile.h 的维护者profile拥有，学生配置不包含门限、极性或模型路径。应用模块不读取dBFS/reference或处理hello/heartbeat。
- 产品源码与Host替身由CMake选源，不在业务、网络或音频算法中插入平台/测试条件编译。第三方库和外部库配置按各自要求保留。
- 使用根目录.clang-format；一行一个语句，条件和循环带大括号。复杂判定可提取有明确含义的小函数，不新增通用框架。
- 注释使用自然中文；步骤注释服务于数据流阅读，约束注释说明线程、时序、单位与失败后果，不重复宣传“唯一边界”。

## 10. 完成标准

1. 先完成代码与体量清理，再编译并运行直接覆盖本次数据完整性、资源退出和协议隔离的必要检查。删除绑定旧策略的测试；不以测试数量或模拟体验作为验收。
2. 修改过的板端 C++ 用匹配 GCC/uClibc 工具链严格交叉构建。
3. 检查 ELF 架构、loader、依赖与 RPATH。
4. 用户指定上板时再部署并配合真实对话体验调整；此前不连接板子、不调用付费接口。
5. 人工音频回归覆盖唤醒、句首、长回复、播放中打断并提交、追问、安静时不自激和断网恢复。
6. 汇报必须区分代码实现、离线验证、交叉构建、部署和人工验收。

保留用户已有修改，不重写 Git 历史，不删除 `v1.0.0` tag。刷镜像、改设备树、分区或覆盖板端状态前必须确认目标板和恢复路径。
