# 客户端语音链路结构重写交接

## 起点与项目约定

本轮起点为`f179051485927007b635516779cc6598f8238ec2`，分支`codex/p1-fast-vertical-integration`，开始时工作区干净。HEAD、状态与源码快照保存在工作区外`../refactor_work/namespace-rewrite-start/`，没有回退或覆盖前一轮成果。

动代码前已检查生效的[AGENTS.md](../../AGENTS.md)，删除“保留底层音频/网络类”“AudioTasks拥有两个线程”“VoiceAudio::ProcessEvents返回PCM事件vector”等旧结构要求。新条款明确：namespace承载真实实现和资源，应用直接使用语句/播放/网络模块，删除旧对象和兼容转发；硬件参数、错误处理、同步、容量、取消与鉴权要求继续有效。同步改写当前架构和教学入口；历史设计文档仅作对照。

## 实际主流程

### 初始化和退出

应用源码按顺序执行：

```text
audio_capture::open → playback::open → audio_capture::start → voice_net::open
voice_net::close → playback::close → audio_capture::close
```

open只准备采集资源，start才启动采集线程，保证两路PCM配置后才首次read。线程join后释放句柄。这里的start直接创建线程，没有新增runtime包装。

### 采集线程

以下是[CaptureTask](../../client/src/audio/audio_capture.cpp)中按顺序出现的真实调用，失败分支与断点处理也在同一函数内：

```cpp
alsa_audio::read(raw.pcm.data(), &raw.discontinuity);
audio_convert::capture(raw, &channels);
rockchip_3a::process(channels, &clean);
wake::detect(frame.pcm, &frame.wake);
vad::process(&frame, playback::observe());
```

处理结果直接进入采集交接队列。不存在AudioTasks→AudioPipeline→多个无参成员方法的业务转发。

### 应用与上行

[App_ReadSpeech](../../client/src/application/voice_client.cpp)先取帧、判定，再发送：

```text
audio_capture::read(&frame, 20ms)
  → speech::update(frame, speaking)
  → Start/Barge：voice_net::start(generation, supersede)
  → 按顺序遍历借用PCM：voice_net::send(generation, pcm)
  → 最后PCM成功后：voice_net::end(generation)
```

speech只有三个公共操作：listen/reset/update。Result借用原句首环或当前帧，不再复制一份AudioEvent PCM批次；reset/listen会使借用失效，所以发送失败或END后立即return。Barge先停播放，但不清掉正在借用的语句历史。

### 下行与播放

```text
voice_net::poll → 首AUDIO时playback::begin → playback::write
  → DONE时playback::finish
  → 播放线程：队列 → audio_convert::playback → 音量/限幅 → alsa_audio::write
  → 排有效滤波尾音 → alsa_audio::drain → status(Drained) → 追问
```

应用直接调用playback，下行PCM不经过speech。只有finish之后可以产生Drained，取消产生Idle，真实失败产生Failed；因此应用无需再存reply_done/playback_done两个布尔。generation仍校验本轮归属。Failed保留到close/open，不能被并发cancel或新generation清掉。

## 删除、替换与保留

| 原实现或机制 | 当前替代 | 原因 |
| --- | --- | --- |
| VoiceAudio、AudioEvent、ProcessEvents及PImpl | speech::update借用PCM + 应用直接播放 | 去掉输入/下行/线程生命周期的混合职责与整批PCM副本 |
| AudioTasks、AudioPipeline及PImpl | audio_capture、playback直接拥有任务/资源 | 删除两个组合转发层，真实数据处理顺序放回任务 |
| AlsaAudio、AudioConverter、RockchipVoiceDsp、SpeechDetector自有类 | alsa_audio、audio_convert、rockchip_3a、wake、vad函数模块 | 资源留在对应实现文件，无旧类代理 |
| VoiceLink类和Impl转发 | voice_net私有状态与普通函数 | 网络仍隔离第三方，但不需要一实例类套PImpl |
| BPV2 flags/reserved及音频START/END标记 | BPV3独立START/PCM/END/CANCEL，12字节PCM头 | 主流程直接表达输入边界，去掉末帧标记协调 |
| 服务端为末PCM加END保留完整一帧 | 满帧立即发，只保留不足一帧的组帧余数 | 完成由DONE表达，正常流不必等下一帧才能发上一帧 |
| App的supersede、reply_done、playback_done缓存 | START参数与DONE→finish→Drained的因果顺序 | 去掉可由流程和模块事实直接表达的重复状态 |
| 测试中的假AudioPipeline | ALSA设备接口替身 + vendor C核心桩 | 运行真实任务、转换、算法胶水和尾播，避免整个产品主线被替换 |

旧.h/.cpp、CMake选源、旧网络头、v2协议文件与fixture均已删除或更名，未保留别名/双栈兼容。音频harness更名为audio_flow_test，仍是原有一个测试程序和20个场景。

保留的复杂度：

- 48kHz/四槽Mode1采集、48kHz双声道输出及底层重采样：尚无匹配BSP/整板16k证据，不强改。
- Rockchip实际RKAUDIOParam/rkaudio_preprocess API、当前256点块及320点交付FIFO、metadata延迟；不混入RKAP/bin API，不推断闭源执行顺序。
- Snowboy局部旧ABI桥、VAD错误、ALSA短读/短写/XRUN及中断退出。
- 80ms采集交接、500ms句首/32帧插话历史、1.5s播放队列、网络有界交接：分别对应时间历史和阻塞边界。
- 插话的候选→静音→低参考→尾音→确认，以及600ms AEC预热、300ms尾音保护；没有改为VAD即取消。
- Listening与Uploading：分别表示尚未START和已经开始上传，是必要业务事实；仍共用一个聆听UI。
- TLS/SPKI、鉴权、generation/sequence、END后取消、背压、超时、旧轮隔离；唤醒、追问、音量、UI/触摸、Wi-Fi配网、摄像头全部保留。

## 公共接口迁移

| 模块 | 当前操作/输入输出 | 调用者 |
| --- | --- | --- |
| audio_capture | open/start/close；read输出CaptureFrame和Frame/Timeout/Failed；reset_listener/arm_playback在帧边界握手；error | 应用；playback仅请求arm |
| speech | listen/reset；update输入一帧和是否在播，输出准入决定、借用PCM、end及探测scale | 应用与HIL |
| playback | open/close；begin(gen)、write(gen,bytes,size,seq)、finish(gen)、cancel；音量/scale；status/error | 应用；采集读取内部播放事实 |
| voice_net | open/poll/start/send/end/cancel/close；save_wifi保留真实配置职责 | 应用及现有CLI |

线协议与Go handler/session/transport及跨语言校验同时更新，见[BPV3](../../protocol/protocol-v3.md)。握手必须`version:3,sample_rate:16000`，旧客户端/服务端拒绝配套。服务端继续作为Key-only课程黑箱，不增加学生Go或云端SDK开发要求。

## 同口径统计

起点为f179051源码，统计包括客户端入口、src、include及私有硬件实现；排除模拟器、资源、第三方和构建输出。ELOC为非空非注释行，测试另列。辅助脚本/JSON在工作区外`../refactor_work/measure_namespace.py`和`namespace-metrics.json`；客户端可用scripts/measure_client.py复核。

| 范围 | 文件数 前→后 | 物理行 前→后 | ELOC 前→后 |
| --- | --- | --- | --- |
| 客户端实现 | 19→20 | 6448→5610 | 4970→4595 |
| 客户端头文件 | 23→25 | 1412→963 | 718→549 |
| 客户端测试 | 13→15 | 3824→3197 | 3199→2947 |
| Go实现 | 21→21 | 4209→4209 | 3814→3816 |
| Go测试 | 24→24 | 3513→3610 | 3286→3381 |

客户端生产ELOC 5688→5144，减少544；两端生产合计9502→8960，减少542。文件增加来自拆开实际wake/VAD资源及把测试替身下沉，不是新增框架。未设某个千行规模或减少百分比作为验收配额。

## 验证与未完成项

| 检查 | 结果与边界 |
| --- | --- |
| Host完整CMake严格构建 | 通过；BSP旧FFmpeg API的deprecated警告保留，未为Host升级vendor依赖 |
| CTest | 24/24通过，包括真实WSS、共享协议、应用与20个音频场景 |
| ASan/UBSan | 20个音频场景与应用行为通过；运行真实任务/转换/EOS，仅设备/vendor核心使用替身 |
| Python | 16项与BPV3共享fixture通过 |
| Go | go test ./...、go vet ./...；app/session/transport相关包race通过 |
| C++↔Go | 本机真实TLS/WSS联测TestCppWSSHappyPath通过，provider为替身 |
| HIL源码 | Host严格语法编译通过；未执行真板探针 |
| Windows服务端 | CGO_ENABLED=0构建成功，build/namespace-release/boompi-server.exe，未启动或部署 |
| RV1106交叉构建 | 未完成：独立build/namespace-rv1106配置因缺BOOMPI_RV1106_TOOLCHAIN_ROOT/SDK失败，无新ARM ELF |
| 真板/云端验收 | 未完成：按用户要求未连接板子，也未调用付费云端；真实ABI、回采、AEC、延迟、音色及界面体验仍待验收 |

关键回归覆盖句首不重漏、尾帧先PCM后END、1/73/320样本有效尾音、短回答、END后CANCEL、旧轮隔离、满帧不等DONE、取消与prepare/write/drain交错、真实Failed不能被cancel吞掉，以及关闭不再flush旧尾音。硬件帧率/通道与算法参数没有据Host结果改动。

复现入口：`cmake --build build/namespace-host --parallel 4`、`ctest --test-dir build/namespace-host --output-on-failure`；sanitizer在build/namespace-sanitized；Go使用项目已有build/teaching-tools/go/bin/go。完整SDK与真板由用户指定时间补齐，当前没有其他以占位实现替代的结构重写部分。
