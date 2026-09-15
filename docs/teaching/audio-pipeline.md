# 按一帧音频的流动顺序复现

本页对应顺序音频模块重构后的源码。先看数据和调用，再进入具体算法；不需要先读完应用、网络和界面。

## 1. 数据在哪里变化

数据类型统一在`client/include/boompi/audio/audio_frames.h`。这些对象不拥有线程、声卡或模型，只保存固定容量的数据。输入使用只读引用，输出由调用者预分配并反复使用。

| 顺序 | 类型/结果 | 当前格式 | 负责产生它的模块 |
| --- | --- | --- | --- |
| 1 | RawCaptureFrame | 48kHz、四通道、960个采样时刻 | AlsaAudio / ReadCapture20ms |
| 2 | CaptureChannels | 16kHz双麦和refL，各320样本 | AudioConverter |
| 3 | CleanAudioFrame | 16kHz单声道320样本，带对齐后的元数据 | RockchipVoiceDsp |
| 4 | CaptureFrame | PCM加唤醒、VAD、近讲等检测结果 | SpeechDetector |
| 5 | AudioEvent | 开始事件、有序PCM、有效末帧 | VoiceAudio语句整理 |
| 6 | 二进制协议消息 | 16字节头加640字节上行PCM | App_Process / VoiceLink |

48kHz四通道的20ms是`960 × 4 × 2 = 7680`字节；16kHz单声道的20ms是`320 × 2 = 640`字节。ALSA的frame表示一个采样时刻的所有通道，不要把它与一段20ms业务帧混为一谈。

## 2. 先看采集主链

`AudioTasks::ReadMicrophoneTask`先处理帧边界控制命令，再读取一帧原始数据：

```cpp
pipeline.ReadCapture20ms(&raw);
pipeline.ProcessCapture20ms(raw, &frame);
SendCaptureToQueue(frame);
```

上面只摘出顺序，源码在每步之间检查失败与退出标志。`raw`由引擎预分配，不在每轮申请堆内存，也不在实时线程栈上新增一块大数组。

进入`client/src/platform/rv1106/audio_pipeline.cpp`的`ProcessCapture20ms`，实际的数据交接是：

```cpp
converter.ConvertCapture(raw, &channels);
dsp.Process(channels, &clean);
detector.Detect(clean, frame);
detector.GateNearVoice(playback, frame);
```

组合模块直接拥有这些成员，错误处理在源码中逐步保留。每一步都有明确输入与输出。

### 格式转换：只负责格式与电平测量

读`audio_converter.h/.cpp`：先复制原始数据到转换器的工作缓冲，修正两路麦克风极性，再对四通道一起降采样，最后拆出双麦及refL。

原始帧不被修改。不能为四路声音各建一个独立重采样器，也不能将极性修正挪到重采样后；滤波相位和-32768反相饱和的次序需要保持。此模块同时计算本帧原始麦电平与参考是否活跃，但不判断是否开始问答。

### 3A：声音与元数据一起过延迟

读`rockchip_voice_dsp.h/.cpp`。当前调用以256样本块与320样本交付衔接；这不是“SDK只能接受256点”的结论。输入只保留一个待填满的块，vendor直接写输出FIFO，删除了独立输出中转数组。首帧预置静音，因此输出固定晚一帧。

`CaptureMetadata`把时间戳、原始电平和参考活跃状态放在一起；3A输出PCM时，同时交出前一帧metadata。首帧没有历史时，电平是-120dBFS、参考不活跃，时间戳沿用原先的当前采集时刻兜底。检测器不再自行延迟这些字段。

不要使用本帧的参考判断上一帧PCM，也不要在普通重新听音时重建3A。只有前端时间线断裂时，才按原顺序复位重采样、3A和检测历史。

### 检测：输入明确的声音和播放事实

读`speech_detector.h/.cpp`。`Detect`先消费处理后的声音，给出唤醒、VAD当前值和120/700ms的开口/结束边沿。

随后后端读取`PlaybackState`，再调用`GateNearVoice`。这个显式输入说明播放是否已经开始渲染、输出是否可闻、是否自然结束或主动取消；检测器不再直接读取声卡或跨线程原子变量。

原来的600ms预热、300ms尾音、零音量旁路和恢复音量后的重新武装保持。PlaybackState由多个原子字段依次读取，不是一个强一致的多字段原子事务。它只服务于原有的帧级门控，不用于重新设计并发协议。

## 3. 语句整理与插话控制分开阅读

检测帧经过已有采集队列回到`VoiceAudio`。`ProcessCaptureFrame`只做分流：先处理断点；正在播放时走插话控制；否则进入`ProcessListeningFrame`。

先复现`ProcessListeningFrame`及其调用的历史缓冲函数：

- 待机时报告唤醒。
- 听音时保存句首，确认开口后先发开始事件，再交出缓存PCM。
- 追问使用已有较长准入窗口，丢掉不应带到下一句话的旧结束边沿。
- 采集中持续交出PCM，最后一帧本身仍包含有效声音，再附上end标志。

然后再读`ProcessBargeFrame`：它是独立的反向控制过程，会要求临时静音、恢复或停止播放。不能把它伪装成无状态的音频转换，也不应让格式转换器调用播放控制。

## 4. 播放是一条独立的数据链

网络帧进入既有有界TTS队列，仍然一包一槽。播放线程取出一槽后，`Render20ms`按顺序执行：

```cpp
converter.UpsamplePlayback(pcm16, samples, &stereo);
const long peak = AudioConverter::Peak(stereo);
AudioConverter::ApplyVolume(&stereo, gain, peak);
pcm.WritePlayback(stereo.pcm.data(), stereo.frames);
```

实际源码先检查错误，并在计算peak后、施加gain前发布“开始渲染/输出可闻”事实。16 kHz单声道直接转换到48 kHz双声道，矩阵明确为L=mono、R=mono，保留95%峰值限幅、舍入和饱和。

只保留最终`StereoPlaybackFrame`，不再经过单声道播放中转。短尾不补成整20ms；EOS通过`UpsamplePlayback(nullptr, 0, &stereo)`取剩余有效音频。极短输入需要推进滤波器，内部补入的静音不作为额外回答发送；总输出时长恰为有效输入时长。1、73、320样本及最后一采样点脉冲由真实FFmpeg测试检查。

`EndPlayback`表示后面没有新包，不等于扬声器已经播完。正常结束先交出滤波尾音再等ALSA drain，主动停止仍drop；下一轮Reset清除旧滤波历史。应用等网络DONE与PlaybackDone都到达后才进入追问，两者到达先后均可。

## 5. 每一步怎样验证

Host音频模块测试现在需要真实`libswresample`与`libavutil`开发文件。教师准备好Linux/WSL依赖后，在已配置的独立Host目录运行：

```sh
python3 scripts/teaching_lab.py 3 --build-dir build/lesson-host
python3 scripts/teaching_lab.py 4 --build-dir build/lesson-host
python3 scripts/teaching_lab.py 7 --build-dir build/lesson-host
python3 scripts/teaching_lab.py 9 --build-dir build/lesson-host
```

| 检查 | 实际验证什么 | 不证明什么 |
| --- | --- | --- |
| pipeline-format | 真实FFmpeg转换、极性、样本数、输入不变、补零、限幅、双声道；真实3A适配FIFO和metadata延迟 | 声卡真实通道、厂商ABI、实际AEC消除效果 |
| pipeline-detection | 生产VAD准入/边沿、参考预热、尾音、静音旁路、取消及断点规则 | Snowboy实际唤醒率、WebRTC实际分类准确度 |
| 原有18个音频场景 | 引擎线程、队列、停止、句首、追问和插话生命周期 | 真板实时调度和声音体验 |

新测试直接链接转换、检测和DSP适配源码，不通过整体假AudioPipeline绕过它们。只有厂商算法内核、WebRTC分类结果和Snowboy命中结果由目标私有C替身控制。没有新增测试程序、生产条件编译或线程。

Host的FFmpeg版本把旧BSP使用的接口标记为deprecated，因此只对转换器源允许该类兼容警告不升级成错误，警告仍显示；其余严格编译检查保留。不能为了让Host编译干净，直接升级板端依赖接口。

## 6. 复现顺序与停止条件

按“读原始帧 → 转换 → 3A → 检测 → 语句整理”的顺序逐个补写，播放链单独验证，最后接插话控制。每一步都用固定输入预测输出，再用测试核对。模块内部保留自己的滤波/FIFO/检测历史，上层不逐项改这些计数器。

全部Host检查通过之后，再用匹配SDK交叉编译并上板确认采样、唤醒、AEC、句首、长短回答、插话、追问、零音量及断网恢复。Host测试和教学模块拆分都不能代替这一关。

## 历史模块拆分记录（原工作区记录，非本轮验收）

下面保留此前的统计与结论；当前16 kHz重构的同口径统计、接口变更和验证边界见[2026-09-15交接](../test/refactor-handoff-20260915.md)。

- 基线为`d337956`。生产文件36→41，ELOC 5746→5982（+236，预算约250以内）；统计包含新增模块与类型，不含测试、教材和第三方。音频后端618→299物理行，具体处理实现计入新模块，不算作被删除。
- 新增AudioConverter、SpeechDetector及audio_frames.h；删除后端中的隐式共享处理数组、分散的delayed字段和内嵌算法实现。没有新增线程、队列、业务状态机、条件编译或通用框架。
- 3A的PCM与metadata由同一个对象管理一帧延迟；正常语句整理与插话控制有各自的阅读入口。板级参数和正常音频行为保持，非法NaN/Inf播放增益改为明确拒绝，初始化错误按实际失败模块报告。
- Linux客户端24/24 CTest、Windows基础2/2、Python16/16、共享协议fixture通过。新增模块的两个场景通过ASan/UBSan；真实后端通过Host严格语法检查，HIL通过Host编译与帮助入口检查。
- 已尝试在独立目录配置RV1106构建，因缺少`BOOMPI_RV1106_TOOLCHAIN_ROOT`及匹配SDK而停止。未部署或访问开发板，不能据Host结果宣称真实ABI、AEC或实时效果已验收。
- 本轮实现尚未提交或推送；旧逐行讲义锁定旧提交，阅读当前实现以本页和当前源码为准。
