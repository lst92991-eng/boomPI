# 本轮预算、实现与验证

起点：`8018787795119011bf930f6afcb9c6203fbe07a4`，工作区干净。开始时保存HEAD、状态、patch和源码快照到工作区外`../refactor_work/budget-rewrite-start/`。仅更新AGENTS中与本轮职责/预算冲突的条款；硬件、TLS、资源回收和课程黑箱边界保留。

**预算未全部达到，不能将本轮称为740–1090行版本。** 以下按实际职责合计，未把拆分/迁移计为删除。

## 逐模块行数

单元格为“物理行 / 非空非注释行（ELOC）”。ALSA共享配置全部计入采集，输出函数计入播放；主流程包括CLI入口、应用以及输入处理线程。头文件包含现存inline代码，ABI桥与所有其余自有胶水另列后计总量。UI/摄像头实现、资源、测试、vendor代码不属于这张语音表，未计入减代码量。

| 职责 | .cpp预算 | 修改前 | 修改后 |
| --- | --- | --- | --- |
| ALSA采集及共享配置 | 70–100 | 307 / 260 | 266 / 230 |
| Rockchip 3A | 130–180 | 241 / 186 | 178 / 154 |
| Snowboy | 50–80 | 56 / 50 | 56 / 50 |
| WebRTC VAD | 30–50 | 185 / 155 | 32 / 32 |
| 语句、pre-roll及业务策略 | 70–110 | 182 / 164 | 269 / 245 |
| WSS收发及协议 | 180–260 | 819 / 731 | 701 / 628 |
| 播放缓冲及ALSA输出 | 90–130 | 471 / 436 | 455 / 419 |
| 主流程、CLI及输入线程 | 120–180 | 721 / 654 | 655 / 589 |
| **以上.cpp合计** | **740–1090** | **2982 / 2636** | **2612 / 2347** |
| Snowboy ABI桥 | 单列 | 93 / 65 | 93 / 65 |
| 额外自有胶水 | 单列 | 836 / 644 | 841 / 647 |
| 头文件及inline | 单列 | 724 / 435 | 701 / 426 |
| **语音全部自有代码** | 含上述全部 | **4635 / 3780** | **4247 / 3485** |

VAD原先混入的策略现在归speech，因此应同时看两者合计：**367/319 → 301/277**，不能把VAD单行的减少全部当作删除。额外胶水包含重采样、电平计算、线程优先级、网络发现/准备及配置解析；新增audio_level.cpp是两处共用的RMS实现，已计入，不作为被删除代码。

复核命令：`python scripts/measure_voice_budget.py --before 8018787`。`--json`给出每组的源文件/ALSA函数归属。代码按.clang-format排版，无压行、宏生成或头文件藏实现。

## 真正删除的机制

- 删除输入线程的listener reset/arm命令、应答状态、100ms握手等待；wake/VAD与业务策略由应用线程串行操作。输入线程持续处理原始PCM和3A，不等待网络。
- VAD删除语句起止、AEC预热/尾音和播放状态，只返回-1/0/1；策略集中到speech，合并重复准入状态。
- 主业务缩为Idle/Listening/WaitingReply/Speaking；连接、是否已START取网络已有事实，60s媒体上限归语句模块，不复制应用帧计数。
- 删除3A的clean→交付帧PCM副本；直接写最终帧。refR在软件转换前丢弃，不再对未使用通道复制/重采样，仍完整读取原始四槽。
- 播放不再重复校验网络sequence，不再同时保存当前轮次和第二份highest_generation；取消后current.generation保留退休水位。
- BPV4固定文本控制替换设备侧JSON：删除cJSON运行依赖、JSON对象树、重复键/转义/数字词法分支。固定字段、规范数字、UTF-8、长度、鉴权和PCM校验继续存在。没有旧协议双栈。
- Go会话直接接管socket消息的PCM，删除再次append复制；仅保留隔开socket读取与云调用的必要有界队列。batch fallback和旧聚合层均未恢复。

策略迁移后，播放快照随PCM排队，防止应用延迟处理时看到“未来”的结束状态；它是带时间关系的事实快照，不是另一份业务状态机。

## 保留与超预算的具体原因

- **ALSA**：Mode1控件按名字/枚举值查找、写入后读回、探测后重新open，以及精确格式/period/buffer设置；短读/XRUN丢不连续前缀，中断与释放。没有证据允许删除这些板级配置或假设已预置。
- **3A/ABI**：参数树部分初始化回滚、签名/feature检查、256点块与320点交付及metadata延迟；Snowboy旧ABI、异常隔离和模型格式检查仍在独立桥中。
- **语句策略**：120ms开口、700ms句尾、400ms追问、600ms预热、300ms尾音、同句插话的候选/静音/低参考/清尾音/确认，以及500ms历史。没有闭源库提供的已验证近讲事件可替代这些规则。
- **WSS**：真实TLS/SPKI、握手/心跳/重连，两个阻塞边界的有界交付，取消时保留退休命令顺序，旧轮过滤与sequence连续性。JSON已删，但尚未将这一职责压到260行内；不能据此声称无法进一步简化。
- **播放**：180ms起播、30ms欠载宽限、40ms补水、队列满明确拒绝、短回答、滤波尾音与ALSA drain、prepare/write/drain期间取消、真实失败锁存及join回收。取消与设备失败不能合并为一个成功结果。
- **主流程**：本表包含输入处理线程和CLI，未把线程生命周期/断点恢复藏到附加文件。应用本身仍保留初始化回滚、发送失败分流、媒体/墙钟超时、字幕、触摸和音量处理；预算仍超出。

这些是当前保留用途，不是“已证明最小”的结论。WSS、播放、主流程及策略仍是未达预算项。

## 实际初始化与处理

App_Init中的真实初始化顺序（每次失败都返回具体阶段，main统一App_Close）：

```cpp
voice_input::open();    // 原始输入、重采样和3A资源
wake::open();
vad::open();
playback::open(view_.volume);
voice_input::start();   // 两路PCM都已配置后才开始首次read
voice_net::open(config);
```

输入线程的实际调用：

```cpp
alsa_audio::read(raw.pcm.data(), &raw.discontinuity);
audio_convert::capture(raw, &channels);
rockchip_3a::process(channels, &frame.pcm, &metadata);
frame.output = playback::observe();
```

应用的实际调用顺序：

```cpp
voice_input::read(&frame, 20ms);
wake::detect(frame.pcm, &frame.wake);
const int voiced = vad::process(frame.pcm); // <0立即失败
frame.vad_now = voiced == 1;
const auto result = speech::update(frame, speaking, frame.output);
// Start/Barge先start；逐帧send；末帧成功后end。失败立即cancel并停止访问借用PCM。
voice_net::start(generation_, barge);
voice_net::send(generation_, result.frames[i]->pcm.data());
voice_net::end(generation_);
```

上面为源码调用摘录，完整条件/错误分支在`client/src/application/voice_client.cpp`与`client/src/audio/voice_input.cpp`。下行直接write播放队列；DONE只finish，实际Drained后才进入追问。

## 测试结果与未完成项

- Linux严格构建、24/24 CTest通过；真实WSS、协议、实际音频任务与算法胶水均运行，声卡/vendor核心仍使用替身。
- ASan/UBSan：20个音频场景、应用行为及协议检查通过。
- Windows MSVC严格构建及2/2 Host测试通过；修复了size_t收窄警告。Windows缺网络测试依赖，WSS验证在Linux完成。
- Go全套test/vet/race及C++↔Go真实本机WSS联测通过。修复错误pin在两层测试中被改两次的偶发问题；连续10次联测通过。真实TLS校验没有绕过。
- Python 16项及BPV4共享fixture通过；HIL源码严格语法检查通过。Windows单EXE构建通过，未运行或部署。
- CI补齐Linux ALSA/FFmpeg开发依赖；Linux专用音频测试不再在macOS寻找ALSA，增加Linux race/跨语言联测。**尚未实际运行本次远程GitHub CI/macOS runner**。
- **交叉编译未完成**：独立budget-rv1106配置缺少BOOMPI_RV1106_TOOLCHAIN_ROOT/SDK；没有新ARM ELF、loader或依赖验收。
- **硬件16k能力未验证**：未获取匹配Codec、I2S/TDM、Mode1回采与全双工证据；保留48k适配不等于认定只能48k。本机软件三通道转换通过相位/样本回归，BSP库仍待验证。
- **真实云端与真板未验证**：未连接板子、未部署、未调用付费接口；实际唤醒/近讲/声学效果、调度和云端延迟仍待授权验收。
- **行数目标未完全完成**：核心2612物理行仍超过1090；不能用上述Host通过替代该目标的完成。
