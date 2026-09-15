# 客户端可读性重构记录

本文记录此前已有工作区修改。当前16 kHz客户端/服务端配套改动与验证以[2026-09-15交接](../test/refactor-handoff-20260915.md)为准；旧接口名仅作迁移对照。

本次以 2026-09-14 工作区已有的未提交代码为基线；没有恢复到 Git HEAD，也没有覆盖原有工作。范围为自有客户端、测试调用和阅读说明。

## 当前入口：普通函数应用模块

在用户继续反馈复杂后，本轮按更接近样例的普通函数模块实现。本轮按样例采用普通函数模块，保留修改前工作区基线以便复核。当前入口以本节和 [阅读顺序](../teaching/client-source-reading.md) 为准，下文的类接口属于历史阶段。

- 删除 VoiceApp 类及其构造/析构接口，没有保留该类再套 App_* 外壳。
- 应用头文件由 73 行变为 15 行，只声明四个普通函数及可控时钟类型。音频、网络、UI、事件容器和对话状态的声明不再出现在应用头文件中。
- main 由 37 行变为 29 行，直接调用 App_Init，循环 App_Process，最后 App_Close；没有应用对象或 try/catch。
- 应用实现拥有一份私有状态，启动、处理、关闭由主线程顺序执行。新的初始化显式复位对话数据；重新初始化前须先关闭旧运行。
- 初始化/处理中的库异常在对应函数内转换为 false 和固定错误说明，main 的统一关闭路径照常运行。错误文本是模块拥有的固定字符缓冲，Close 不清除错误。

| 删除的接口 | 替换接口 | 输入输出及调用方 |
| --- | --- | --- |
| VoiceApp 构造、Init | App_Init(config, clock=系统单调时钟) | 返回 bool；main 提供配置，测试可沿用可控时钟，学生没有新配置参数 |
| VoiceApp::ProcessOnce | App_Process() | 返回 bool；main 循环调用，失败后进入关闭 |
| VoiceApp::Close、析构回收 | App_Close() | void、可重复调用；正常退出与初始化/处理失败均由 main 调用 |
| VoiceApp::LastError | App_GetError() | 返回模块持有的 const char*；main 打印失败原因，关闭后仍有效 |

现有音频、网络和 UI 公共接口没有新增；六个对话阶段、音频事件、线程队列、插话/停止/断线响应继续保留。没有把必需流程删除来得到短入口。生产文件仍为 42 个，有效代码保持 5673 行；移出头文件的代码仍包含在总量中，减少的是应用层公开的类型与对象生命周期概念。

本轮复用了配置/应用测试程序，新增初始化异常和处理异常的用例，要求返回失败并关闭模块；原有正常问答、插话、追问、超时、背压、断线及重入初始化场景继续验证。结果与快照在上级 refactor_work/plain-app/。目标板 SDK 仍缺失，未交叉编译、未部署；未提交、未推送。

## 历史：按教学样例展开任务流程

本轮在保留全部现有功能的前提下调整应用和任务代码。参考正式业务代码，排除 XIAOZHI_CODE 的 test/text 与演示实现。下面各历史阶段的旧接口以本节和 [当前阅读顺序](../teaching/client-source-reading.md) 为准。

- main 直接持有循环，配置、Init、每轮 ProcessOnce、Close 都可见。VoiceApp 持有错误文本，删除借用 main 配置/错误的成员及额外 failed 标志。
- 应用每轮分别处理 ReceiveReplyAndPlayAudio、ReadSpeechAndUpload、ReadUserAction；前两个函数包含取数据与对应分支，不再先在 Run 里取事件、再跳到独立事件分派函数。
- AudioTasks 的采集任务按“处理帧边界命令、读声卡、音频处理、发布”展开；播放任务按“等数据、取消/准备、取一帧、释放锁后播放、收尾”展开。删除 have_slot、finish、must_drop、prepare_failed 四个临时决定变量和单次转发的 Render 辅助函数。
- 原网络、显示、摄像头线程分别命名为 NetworkTask、DisplayTask、CapturePreviewTask，原线程数量、所有权、处理周期和接口保留。
- 插话四阶段名称为 WaitCandidate、WaitReferenceLow、WaitEchoTail、ConfirmNearSpeech；计时、门限及状态转移保持原值。没有从 ESP32 样例引入额外任务、Opus 协议或新的标定参数。

### 本轮接口清单

| 原接口 | 当前接口 | 输入输出与调用方 |
| --- | --- | --- |
| VoiceApp(config, error, clock) | VoiceApp(clock = 单调时钟) | main 默认构造；既有主机测试可传时钟；删除配置借用及外部错误指针 |
| VoiceApp::Open() | Init(const VoiceClientConfig&) | main 提供已有配置，返回初始化结果；网络模块复制其所需配置 |
| VoiceApp::Run(stop) | ProcessOnce() | main 每次调用推进一轮，成功 true、致命故障 false；停止信号由 main 循环检查，旧 Run 删除 |
| 外部 error 指针 | LastError() const → const string& | 新查询接口，main/测试读取应用持有的错误；Close 保留错误文本 |
| AudioEngine / audio_engine.h/.cpp | AudioTasks / audio_tasks.h/.cpp | 原音频线程模块重命名，没有新增类或第二套实现；CMake、调用方与文档同步 |
| AudioEngine::Open(gain) / Close() | AudioTasks::Start(gain) / Stop() | VoiceAudio 与既有音频测试调用；启停、回收和失败规则不变 |
| AudioEngine::Capture(frame, timeout) | AudioTasks::ReadProcessedFrame(frame, timeout) | 返回 CaptureResult，交付一帧已处理 PCM 或超时/失败 |
| AudioEngine::QueueTts24k(bytes, size, sequence) | AudioTasks::QueueReplyFrame(bytes, size, sequence) | VoiceAudio 提交下行帧，仍返回 QueueTtsResult；格式、容量、连续性检查不变 |

其他 AudioTasks 播放控制/查询接口保留；VoiceAudio、VoiceLink、DeviceUi 的公共接口未增加。内部调整包括三个应用数据路径、UploadSpeechFrame、QueueReplyAudio、CheckTimeout，原有任务与 VoiceAudio 的逐帧函数同步改为能说明职责的名称；删除私有 ReplyHistory 二值枚举，StopAndListen 使用明确的 retract_history 参数。不保留旧签名转发壳。

### 同口径体量与验证

计划：新增生产类和文件均为 0，有效代码增长上限 60 行，优先不增长。实际：

| 指标 | 本轮开始 | 当前 |
| --- | ---: | ---: |
| 生产 C/C++ 文件 | 42 | 42 |
| 物理行 | 7856 | 7850 |
| 有效代码行 | 5678 | 5673 |
| 预处理条件组 | 2 | 2 |
| 多语句压行 | 0 | 0 |

统计包括自有驱动与 UI，排除第三方、资源、测试、文档和构建输出。main 当前 37 物理行、28 有效行；增加的三行直接表示应用循环，不以隐藏流程换取更短的 main。没有大幅削减全部源码的宣称，重点是读者能够沿着实际处理步骤跟下去。

验证：完整主机 CTest 24/24、ASan/UBSan 24/24，通过的应用断言共 47 项；Python 16/16、协议 fixtures、既有 Go test/vet 通过。真实声卡/音频任务适配、显示/触摸/摄像头及 UI 模拟器已完成主机构建，HIL 程序严格语法检查通过。检查了启动失败、主循环中的音频故障、关闭后错误保留、正常问答、批次中途取消、插话、队列背压、短尾与阻塞播放中断。

158 个受保护文件（third_party、client/assets、server、protocol）与原基线哈希一致。缺少匹配 RV1106 SDK，仍未完成本版交叉构建、ELF 检查或上板验收；没有部署。工作区快照、差异、统计和测试结果保存在上级 refactor_work/teaching-flow/。未提交、未推送。

## 用户提供的教学样例（2026-09-14）

本次对照两个仓库的实际业务启动、任务循环、音频处理接口和电机/网关业务代码，未编译或修改样例。按用户补充要求，XIAOZHI_CODE 的 test/text、测试和演示代码全部排除，不作为写法参考。

| 来源 | 固定版本与阅读位置 | 观察到的写法 |
| --- | --- | --- |
| XIAOZHI_CODE | [App_Application.c](https://github.com/lst92991-eng/XIAOZHI_CODE/blob/fe42bf4a563db4d1698545727909503498cdfd54/main/App/App_Application.c)、[App_Audio.c](https://github.com/lst92991-eng/XIAOZHI_CODE/blob/fe42bf4a563db4d1698545727909503498cdfd54/main/App/App_Audio.c) | 启动函数顺序初始化显示、网络、按键、缓冲、音频和通信；音频任务名直接说明来源与去向，任务内展开接收、处理、发送和回收 |
| vscode_motor_gateway | [网关 App_task.c](https://github.com/lst92991-eng/vscode_motor_gateway/blob/4e80062871d963dfd535407b165e48ecf1baf76f/gateway_lora_contmooootor/App/App_task.c)、[电机 App_task.c](https://github.com/lst92991-eng/vscode_motor_gateway/blob/4e80062871d963dfd535407b165e48ecf1baf76f/motor_code%20_gitcontrol/App/task/App_task.c) | 启动任务负责初始化与创建任务；显示、按键、PID、通信各有可直接阅读的循环；PID 循环显式写出目标、反馈、计算、限幅和输出 |

XIAOZHI_CODE 的语音参考入口为 App_Application_Start。排除 text.c/text.h、测试/演示入口，以及 App_Display 中夹带的 TestImage 图像/动画实现；构建目录、托管库与生成图像也不纳入教学风格分析。

对当前客户端的指导：

1. 先让任务/处理函数内的数据来源、处理步骤和输出去向可见。仅把细节移到 main 下方或缩短函数名，不代表流程更容易读。
2. 名称表达模块和动作；数据搬运函数可以直接说明来源与去向。长度服从含义，不以短名字为目标。
3. 应用入口清楚列出启动顺序；每个任务以当前实际动作展开。函数只为真实责任划分，不为套用样例名称增加转发壳。
4. 中文步骤注释可用于带读“取一帧、处理、交付”等动作；保留解释单位、时间边界和资源回收的必要注释，减少反复重复的长段契约说明。
5. 接口只传本动作需要的数据，初始化常量就近固定，学生不增加声学校准项。样例使用普通 C 函数；这不自动要求替换本项目仍需使用的 C++ 库或新增一层 C 外壳。
6. 网关解析函数本身包含字段/类型/范围检查，说明易读与校验可以并存。RV1106 的固定音频帧、停止/插话、超时、断线与有界缓冲继续保持。
7. 借鉴样例中的步骤和责任边界，按 RV1106/Linux 的实际线程与协议实现。样例中的 ESP32 编解码、ESP-SR 配置、STM32 HAL、FreeRTOS 任务数量及阻塞方式不是本板的配置依据；不扩大服务端教学范围。

样例阅读阶段仅补充参考分析和项目约定，当时生产体量为 42 个文件、5678 ELOC；后续实现与验证见本页“当前实现”一节。

## 历史：前两轮可读性整理结果

- `main` 当前为 34 行，放在辅助函数定义前；中文注释标明配置、初始化、运行、退出。具体命令分派和信号注册留在本文件下方。
- 原内部 VoiceApp 移到正式头文件，测试通过普通编译链接访问它，不再包含 application 的 `.cpp`。
- 音频线程调用 AudioPipeline；采集、转换、DSP、检测、语句整理与播放各有明确入口。
- 一个内部 FrameQueue 复用采集、播放和句首历史三处环形存储，锁、溢出、历史覆盖与轮次规则仍由原模块负责。
- 音频结果直接返回一批事件，应用用 for 循环处理；删除同线程的第二份事件队列与其溢出分支。停止或句尾后明确放弃本批剩余输入。
- 网络直接复用 VoiceClientConfig，删除 LinkConfig 和逐字段转换；字幕 SetText 只接收一段文本。
- 删除内部 AudioPipeline::Impl 的分配与转发，预热和尾音共用一个剩余帧数；原子变量统一采用标准默认内存顺序，不把手工 acquire/release 优化带进教学主线。
- 学生侧声学校准项为 0。删除 AudioEngineConfig 和 BoardVoiceProfile 参数对象；三个硬件常量与三项已验证声学预置放在内部板级头文件。
- 删除逐轮状态打印、音频会话统计、摄像头周期帧率/负载统计及仅用于这些统计的状态和方法。错误、必要恢复提示、实际流控与退出规则保留。
- 统一公共操作/查询和枚举项命名、字段风格及排版；C ABI 和第三方命名遵循原契约。

## 接口变更清单

| 原接口 | 当前接口或处理 | 输入输出、调用方与理由 |
| --- | --- | --- |
| `RunVoiceClient(config, stop, error)` | 删除；使用现有 VoiceApp 的公开生命周期 | main 构造 VoiceApp 后调用 Open、Run、Close，初始化和退出不再藏在单个函数中 |
| 内部 `VoiceApp::OpenModules` 与析构关闭 | `VoiceApp::Open()`、`Run(stop)`、`Close()` | 构造仍接受配置、错误输出和可选测试时钟；Open/Run 返回成功与否，Close 幂等；析构复用 Close |
| `AudioBackend` / `audio_backend.h/.cpp` | `AudioPipeline` / `audio_pipeline.h/.cpp` | 现有同步处理链重命名，输入输出帧契约不变，不增加中间层或线程 |
| `AudioEngine::Open(AudioEngineConfig)` | `Open(float playback_gain = 1.0F)` | 调用方仅提供正常音量；模型、PCM 路径、极性和门限不再经过公开配置对象传递 |
| `AudioPipeline::Open(config)`、`SpeechDetector::Open(config)` | `Open()` | 内部读取配套预置，删除对不再可变参数的重复校验 |
| `VoiceAudio::Process` / 首轮 `ProcessNextEvent` | `ProcessEvents(std::vector<AudioEvent>& events, milliseconds timeout)` | VoiceApp 持有并复用输出容器；函数清空后交付开始事件、完整句首与 PCM，不再逐个取内部队列；返回 void，空容器表示暂无结果，Fault 表示故障 |
| `VoiceLink::Open(LinkConfig)` | `Open(const config::VoiceClientConfig&)` | VoiceApp 直接交付已有配置；删除重复类型，网络内部 FindServer 同步改用该类型，不增加配置项 |
| `LvglScreen::SetText(first, second)` | `SetText(std::string_view text)` | DeviceUi 和模拟器提交一段文本；字体过滤、默认提示与显示内容保留；无旧签名兼容壳 |
| `VoiceLink::Poll`、`DeviceUi::Poll` | `PollEvent`、`PollAction` | 名字直接表达取出的对象种类，收发协议不变 |
| `healthy`、`last_error`、`playback_done`、`playback_failed` 等 | `IsHealthy`、`LastError`、`IsPlaybackDone`、`HasPlaybackFailed` 等 | 布尔查询使用 Is/Has/Was；其他操作和查询使用 PascalCase |
| `CameraCapture::status`、`RockchipVoiceDsp::is_open` | `Status`、`IsOpen` | 统一查询命名 |
| `playback_active`、各层播放 XRUN 统计查询、`MarkDisplayed` | 删除 | 未使用的包装和仅用于已删除打印的接口，无替代接口 |
| 重复的数组、head/count 和取模操作 | 内部 `FrameQueue<Frame, Capacity>` | 唯一新增生产工具类，供音频引擎与语句整理使用，固定容量且不分配堆内存 |
| 枚举项中的 `k` 前缀 | 统一为 PascalCase | 保留枚举值顺序和协议常量；普通常量仍使用 k 前缀 |

FrameQueue 的完整接口为：

```cpp
bool Push(const Frame& frame) noexcept;
bool Pop(Frame* output = nullptr) noexcept;
void Clear() noexcept;
std::size_t Size() const noexcept;
Frame& operator[](std::size_t index) noexcept;
const Frame& operator[](std::size_t index) const noexcept;
```

Push 满时拒绝，Pop 空时失败；空输出指针用于明确丢弃历史队首。索引由调用方保证小于 Size。Clear 只清逻辑队列，后续入队完整赋值；TTS 每次使用零初始化槽，短尾帧不会带入旧声音。同步与业务策略没有放进这个存储类。

## 保留的行为

固定帧与通道格式、双麦单参考 3A、PCM/元数据对齐、唤醒与 VAD、句首缓存、插话确认时序、STOP/SUPERSEDE 及旧代隔离、物理播放完成、追问、断线恢复、缓冲上限、阻塞解除、TLS 校验和线程资源回收均保留。

声学预置值仍为唤醒灵敏度 0.7、原始麦准入 -30 dBFS、近讲门限 -25 dBFS；硬件极性 +1/+1、同步参考延迟 0。它们的内容没有因本次结构调整而重新标定。

## 代码与文件数量

统计范围与原 `scripts/measure_client.py` 相同：客户端生产入口、src 与 include，包含自有硬件适配；不计第三方、资源、测试、文档和构建产物。

| 指标 | 初始工作区 | 第一轮后 | 本轮最小化后 |
| --- | ---: | ---: | ---: |
| 生产源码文件 | 41 | 42 | 42 |
| 物理行数 | 8491 | 8167 | 7825 |
| 有效代码行数 | 5982 | 5787 | 5656 |
| 多语句压行 | 0 | 0 | 0 |
| 预处理条件组 | 0 | 2 | 2 |

新增的两组条件仅在自有 Snowboy C ABI 桥接头中用于 `__cplusplus`，使同一声明可由 C 和 C++ 编译；业务与音频算法没有增加平台/测试分支。相对初始工作区净增文件为 FrameQueue，累计减少 326 行有效代码（5.45%）；本轮相对第一轮减少 131 行，文件数未增加。物理行减少也包含接口注释收敛，这部分不算实现减少。约 4200 ELOC 的评审方向尚未达到；完整范围仍包括双向音频、WSS、Wi-Fi、摄像头、LVGL 和自有设备驱动。继续删除真实线程边界、必要状态或功能会改变已确认范围，因此本轮在这些边界保留显式实现。

## 验证

- 重构前的 Linux 基线：24/24 CTest 通过。
- 重构后的 Linux 客户端：24/24 CTest 通过，其中应用测试覆盖实际 CLI、显式生命周期失败路径、同批开始/PCM、插话，以及批次中途背压后丢弃剩余输入（本次入口调整后为 41 项断言）。
- ASan/UBSan：24/24 CTest 通过。
- 协议 v2 fixtures：通过；Python 检查：16/16 通过。
- UI 模拟器编译、真实显示/触摸/摄像头封装编译、真实 ALSA/音频流程封装及引擎编译：通过；HIL 程序主机严格语法检查通过。
- 自有 Snowboy 桥接头按 C11 编译检查：通过；不代表验证了厂商二进制 ABI。
- 配套服务现有 Go 测试与 vet：通过，服务源码没有修改。
- clang-format 检查、Git whitespace 检查、当前阅读索引的相对链接检查：通过。
- 项目内 158 个受保护文件（third_party、client/assets、server、protocol）逐文件 SHA-256 与本轮基线一致。没有编辑官方 SDK、第三方库源码或库二进制。

移除了一项只验证假播放 XRUN 统计计数的旧检查，并从摄像头源文件检查中移除了对统计打印的要求；实际音频断点、恢复、关闭、短尾帧和摄像头错误清帧检查继续保留。

本机 FFmpeg 对原 BSP 所用重采样接口仍给出原有弃用警告，沿用原先仅针对该适配源的兼容处理，没有修改官方库。Windows/WSL 挂载目录偶发小幅时间戳偏差会产生 Make 的 clock-skew 提示，最终目标均完成构建并执行了上述测试。

## 尚未完成的目标板验证

已用隔离构建目录尝试 RV1106 preset，因缺少 `BOOMPI_RV1106_TOOLCHAIN_ROOT` / 配套 SDK 路径而停止。尚未生成新的 ARM 客户端 ELF，不能宣称已验证实际 uClibc/厂商 ABI、真板实时性或声学效果。没有部署新程序或更改板端状态。

提供匹配 SDK 路径后，继续严格交叉构建、ELF 依赖检查及真板语音回归。这是交付前仍需完成的验证，不需要再新增架构层。

## 阅读入口与工作区

先读 `client/apps/boompi_client/main.cpp`，再按 `docs/teaching/client-source-reading.md` 进入应用事件、采集链、播放链和插话控制。

未提交、未推送。重构前快照、本轮差异和测试结果保存在工作区上级的 `refactor_work/client-readability/` 与 `refactor_work/client-size-pass/`，用于区分本轮修改与原先已有的未提交工作。

## 入口阅读顺序调整

用户反馈第一眼仍不愿阅读 main。本次仅整理入口的阅读顺序，并缩短现有配置函数名；保留默认运行、--voice-loop、--check-config、--save-wifi、退出信号、错误返回与资源回收。

预算为不增加文件或类，允许不超过 25 行有效辅助代码。实际文件数仍为 42；有效代码 5656 → 5678（+22），物理行数 7825 → 7856。main 本体 35 → 34 行，有效行 33 → 25。这里没有把挪到文件下方的代码算作删除，主要变化是入口不再展开长命名、参数判断和信号注册。

| 接口 | 输入输出与调用方 | 处理 |
| --- | --- | --- |
| LoadClientConfig | VoiceClientConfig*、可选 string* 错误输出；返回 bool；main、配置检查命令、原配置测试调用 | 重命名已有 LoadVoiceClientConfigFromEnvironment，行为不变，旧名删除 |
| IsVoiceMode | argc、argv → bool；仅 main 调用 | 本文件私有函数，识别默认运行和单独 --voice-loop |
| RunCommand | argc、argv → 进程退出码；仅 main 调用 | 本文件私有函数，接收原命令分支；原 SaveWifiFromInput 合入此处 |
| SetupExitSignals | 无参数、无返回值；仅 main 调用 | 本文件私有函数，集中原有三项 signal 注册 |

沿用现有配置与应用测试程序，额外覆盖 --voice-loop 携带多余参数时不启动，并将保存 Wi-Fi 的检查放到设备 UUID 设置之前，验证辅助命令不依赖正常启动配置。完整主机验证结果保存在工作区上级 refactor_work/main-readability/；目标板 SDK 缺失的验证限制保持不变。
