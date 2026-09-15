# 音频链重构交付记录

## 范围与起点

起点：`fa640e32db262ae566c81bd49d3834eba79ca146`，分支`codex/p1-fast-vertical-integration`，开始时工作区干净。提交号、状态、补丁及源码备份保存在仓库外`refactor_work/audio-unified-start`，没有回退旧提交或改写历史。

用户最后澄清：**纯音频约1200行，不是所有客户端代码**。本表计入采集、算法、语句、播放、输入处理循环，以及直接服务于它们的全部头文件、重采样、线程适配；不含问答应用、WSS、CLI、网络配置、UI、服务端、测试、第三方和构建产物。应用内移出的处理调用不当作整套程序的删除，应用和WSS另行列数。

每格为物理行 / 非空非注释行。复核命令：`python scripts/measure_voice_budget.py --before fa640e3 --audio`；去掉`--audio`可查看更宽的语音客户端范围。

| 职责 | 重构前 | 当前 |
| --- | ---: | ---: |
| ALSA采集及Mode1配置 | 266 / 230 | 134 / 123 |
| Rockchip 3A | 178 / 154 | 150 / 125 |
| Snowboy | 56 / 50 | 50 / 43 |
| WebRTC VAD | 32 / 32 | 32 / 32 |
| 语句与pre-roll | 269 / 245 | 46 / 39 |
| 播放及ALSA输出 | 455 / 419 | 259 / 246 |
| 音频输入任务 | 182 / 167 | 177 / 163 |
| 重采样及原电平计算 | 252 / 216 | 144 / 114 |
| 线程优先级 | 28 / 16 | 28 / 16 |
| 原Snowboy C桥 | 93 / 65 | 0 / 0 |
| **音频头文件（含内联）** | **437 / 285** | **218 / 142** |
| **音频合计** | **2248 / 1879** | **1238 / 1043** |

问答应用本身（另列）：376 / 347 → 230 / 218。WSS及协议（另列）：701 / 628 → 599 / 534.

## 实际删除

- 删除四阶段插话试探、静音/恢复/冷却、二次确认、专用历史裁剪、400ms追问确认，以及额外预热、尾音屏蔽、原始电平门限和参考等待。普通提问、追问、插话共用120ms连续VAD、700ms静音句尾。
- 删除音频电平计算、采集时间戳/序号、原始电平/参考状态、播放快照及对应元数据延迟。输入只交付PCM、wake、VAD和断点。
- 删除通用FrameQueue、完整CaptureFrame历史、重复录音事件PCM副本。前滚只保存25帧纯PCM，录音确认后借用当前帧；句首、当前帧和尾帧只交付一次。
- 删除采集任务重复的opened/stop状态。主程序负责固定初始化顺序，ALSA中断使读取返回-ECANCELED，输入线程退出后释放资源。
- 删除混合的alsa_audio封装。audio_capture只持有输入声卡；播放直接持有输出声卡，取消标志和I/O位于同一模块，移除一套输出转发接口和停止标志。
- 删除Snowboy C桥及额外句柄分配。wake.cpp直接持有厂商对象，并且只有该源文件使用旧C++ ABI；对外只传PCM与整数/布尔值，不传string或厂商对象。模型格式检查和异常处理仍在边界内。
- 删除采集48k中间数组和拆平面再交织。FFmpeg通道矩阵直接读取四槽并选双麦/refL；播放输出工作数组减为一块48k双声道。
- 删除播放器begin、generation、播放观察快照、每包Slot长度、180ms软件预缓冲和欠载重缓冲策略；首包直接起播，短尾由finish推动。
- 删除应用重复轮次号、语句状态枚举和测试专用时钟。网络拥有轮次与序号；尾播转追问不再清掉已经开口的前滚。
- 网络删除自有发送PCM队列和三套事件数据容器，直接使用WebSocket++队列并移动消息负载。实际库会整批发送，软件待发+在途批次容量合计约1.28s；库的pong超时负责连接失活，应用仍保留回复超时。

## 保留的具体成本

- 原生16k双工尚无匹配整板证据：暂留48k四槽采集、48k双声道输出及真实重采样。没有以旧代码使用48k推断硬件只能48k。
- Mode1控制需要按枚举名称查找并读回；短读继续补齐，XRUN立即丢弃不连续前缀并报告断点。这部分使采集超过此前单模块100行参考值。
- 当前SDK256点与业务320点不同，3A保留块余数及640点输出缓冲。容量来自固定块长不变量，不再逐块重复检查自身不变量；厂商失败返回仍立即终止处理。
- 播放保留1.5s采样环、短写恢复、音量/限幅、阻塞I/O取消、真实drain及资源回收。这些使播放超过此前130行参考值；总音频范围约1200行。
- 输入与播放各有独立任务，隔开声卡阻塞I/O与应用等待；算法之间没有队列。硬件断流与应用交接溢出都取消残缺输入，不拼接音频。
- UI、触摸、配网、摄像头仍使用产品实现。已删除的旧聚合层和batch ASR fallback没有恢复。

## 实际初始化与处理

`App_Init`调用`voice_input::open → playback::open → voice_input::start → voice_net::open`。输入open按`audio_capture → audio_convert → rockchip_3a → wake → vad`初始化；Mode1和两路PCM配置完成后才首次读取。失败与正常退出共用App_Close，先中断/join再释放。

下面是当前输入任务完整函数，非占位示意：

```cpp
void CaptureTask() {
  audio::SetAudioThreadPriority("boompi-capture", 40);
  for (;;) {
    // 读取 → 必要格式适配 → 3A → 唤醒 → VAD → 交付。
    const int captured = audio_capture::read(raw.data());
    if (captured < 0) {
      if (captured != -ECANCELED) {
        Fail("ALSA capture read failed", captured);
      }
      break;
    }
    audio::CaptureFrame frame{};
    if (captured == 0) {
      rockchip_3a::close();
      if (!audio_convert::reset_capture() || !rockchip_3a::open() || !wake::reset() ||
          !vad::reset()) {
        Fail("audio processing reset after discontinuity failed");
        break;
      }
      frame.discontinuity = true;
    } else {
      // Snowboy要求外部VAD句尾后Reset；只由本线程调用，不等待业务线程握手。
      if (wake_reset.exchange(false) && !wake::reset()) {
        Fail("Snowboy reset failed");
        break;
      }
      if (!audio_convert::capture(raw, channels)) {
        Fail("capture resampler lost frame alignment");
        break;
      }
      if (!rockchip_3a::process(channels, frame.pcm)) {
        Fail("Rockchip 3A rejected a frame");
        break;
      }
      const int detected = wake::detect(frame.pcm);
      if (detected < 0) {
        Fail("Snowboy processing failed");
        break;
      }
      frame.wake = detected > 0;
      const int voice = vad::process(frame.pcm);
      if (voice < 0) {
        Fail("WebRTC VAD processing failed");
        break;
      }
      frame.vad_now = voice == 1;
    }
    // 队列满显式发布断点，应用必须取消残缺输入，不能悄悄跳过PCM。
    std::lock_guard<std::mutex> lock(mutex);
    if (frame.discontinuity || pending == kCaptureSlots) {
      frame.discontinuity = true;
      read_at = pending = 0;
    }
    frames[(read_at + pending) % kCaptureSlots] = frame;
    ++pending;
    condition.notify_all();
  }
}
```

应用仅在Listening/Speaking时调用speech::update，依次START、发送借用PCM、END；DONE只finish播放输入，Drained才打开追问窗口。真实应用分支见`client/src/application/voice_client.cpp`。

## 验证及产物（首次交叉构建阶段）

- Linux Host严格构建及当前9个CTest入口通过；使用真实转换、分块、线程与应用代码，声卡/vendor核心替换。测试不代表声学验收。
- Windows当前2个Host入口通过。删除了绑定旧策略、假时钟与重复场景的测试，CI不再重复执行一次相同WSS测试。
- 当前C++与Go真实TLS/WSS联测通过；云端响应由替身提供，没有收费请求。线协议仍为配套BPV4，服务端运行代码本阶段未改，已重新构建Windows服务端。
- 已在原RV虚拟机的幸狐SDK完成GCC8.3/uClibc完整客户端构建，链接真实3A、Snowboy和VAD库。仅wake.cpp使用旧ABI。
- 产物为ELF32/little-endian/ARM EABI5 hard-float，加载器`/lib/ld-uClibc.so.0`，无RPATH/RUNPATH，最高GLIBCXX需求3.4.22。实际板上动态库与运行结果仍待验证。
- 修复了CMake编译器探测未继承工具链缓存路径，以及网络配置POSIX open/close被namespace函数遮蔽的问题。真实network_setup.cpp已纳入已有Host系统源编译目标。
- 新程序位于Git忽略的`build/audio-refactor-release`。ARM客户端SHA256：`1d5eaa0aea8c50b5e0013b39aa5b2166b6e23408b63bb01a21c6020d8157db38`。

**上述构建检查完成时，尚未执行真板部署或收费云端调用。** 后续实际运行见下节。原生16k双工、回采点/延迟、真实模型唤醒、3A声学效果、插话自激率、弱声、流式延迟与音量体验，仍需分别验收。策略已经改变，不宣称与旧策略体验完全等价。

## 2026-09-15 真板对话测试与待分析问题

- 经用户授权，已部署 ARM 客户端并启动配套 Windows Go 服务端，使用已有 DashScope Key 进行人工对话。实际板端成功加载 vendor 库并建立 WSS 连接；这只证明程序可运行，不代表完整声学验收通过。
- 本次音频调参代码提交为 `748286538b6476ae0f53de14829edce1588ce97e`。`speech.cpp` 的连续人声确认从 120 ms 改为 300 ms，普通提问、追问、插话共用；500 ms pre-roll、700 ms 句尾静音和其余算法参数保持原值。
- 300 ms 版本已用同一幸狐 SDK 增量交叉构建，Host 句首/当前帧/尾帧检查及真实应用 harness 通过。板端安装后 SHA256 为 `d9942028d9aa1b15f8ef0a3b5a14a04798a4d4ce982c5aa382841eb1ec46d72d`，客户端重启后于 21:34:35 重新连接服务端。
- **人工体验未通过：用户观察到客户端被自己的回复打断，呈现自激行为；提高到 300 ms 后问题仍存在。** 服务端在本次更新后仍连续记录 `status=canceled`，与频繁取消一致。日志中的 `failure_stage=tts` 或 `llm_stream_or_tts_input` 本身不能证明云端服务故障。
- 待分析范围是播放回声到 VAD 的链路：Mode1 参考是否实际有效、通道排列/极性、参考与麦克风的增益及时延、3A 配置与输出残留。尚未取得同步原始麦克风、参考和 3A 输出的测量证据，因此不能把根因定为闭源 3A 算法本身，也不能宣称单纯加长确认时间已解决自激。
- 本次未新增协议、模块或测试入口；生产代码行数不变。原生 16 kHz 双工及完整人工回归仍未完成。原始运行日志留在本地，API Key、配置及日志不提交到仓库。
