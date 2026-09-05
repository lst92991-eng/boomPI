# 客户端分关复现实验

目标不是先读完所有文件，而是每次实现一段行为，并用可重复的结果证明它正确。服务端只作为配套程序配置，不纳入逐行课程。

## 当前提供什么

本目录提供**同一份正式源码上的分关补写与验证**：每关指出实现顺序、已有支持代码、验证命令和常见失败。它不是九个已裁剪、各自独立运行的阶段客户端。应用和传输测试会覆盖完整模块，尚未学习的函数先保留参考实现；不能把其它目录删空后期待完整项目仍能配置。

这样可以先复现一段真实生产代码，而不维护九套协议、声学策略或驱动。完整客户端始终只有一个产品入口，教学检查复用既有CMake目标和测试程序，不在产品源码中增加课程宏。

每关按以下顺序进行：

1. 先运行原实现，记录预期结果，确认环境本身可用。
2. 只在自己的练习副本补写本关指定函数，保持签名和支持代码不动。
3. 运行本关检查。失败时先看第一条断言，不通过修改测试来掩盖问题。
4. 说明输入、输出、所有权、失败动作；然后再对照参考实现。
5. 学完一关后运行前面相关检查，防止新改动破坏已完成行为。

## 环境与统一命令

推荐Linux Host或Windows上的WSL。需要CMake、C++17编译器和Python 3.9以上；第2/5关还需要Host OpenSSL、Boost 1.83及cJSON开发文件。这里只运行本地测试，不调用云端API、不需要云端Key，也不会连接开发板。

在仓库根目录配置一次：

```sh
cmake -S . -B build/lesson-host -DCMAKE_BUILD_TYPE=Debug \
  -DBOOMPI_BUILD_UI_SIMULATOR=OFF -DBOOMPI_BUILD_TESTS=ON \
  -DBOOMPI_STRICT_WARNINGS=ON -DBOOMPI_REQUIRE_HOST_TRANSPORT_TEST=ON
python3 scripts/teaching_lab.py
python3 scripts/teaching_lab.py 1
```

本文单独使用`build/lesson-host`，避免复用已有Windows Visual Studio的`build/host-debug`缓存。Windows与WSL的构建目录必须分开。测试依赖放在教师提供的非系统目录时，由教师设置CMake的`BOOMPI_HOST_BOOST_INCLUDE_DIR`、`BOOMPI_HOST_CJSON_INCLUDE_DIR`和`BOOMPI_HOST_CJSON_LIBRARY`。不要把Windows库路径传给WSL编译器，也不要把个人路径写进源码。

使用现有构建目录：

```sh
python3 scripts/teaching_lab.py 3 --build-dir build/lesson-host
python3 scripts/teaching_lab.py 3 --build-dir build/lesson-host --dry-run
```

`--dry-run`只显示命令。正常执行会先构建本关目标，再检查所需测试是否全部登记，最后运行CTest。依赖缺失导致测试没有生成时，实验明确失败，不把“零个测试”当通过。详细硬件和发布检查仍见[Host与真板验收](../test/host-validation.md)。

## 01 配置：先学会在启动前拒绝错误

**本关新增概念：** 数据结构、字符串校验、成功/失败返回；不讲线程。

源码：`client/include/boompi/config/voice_client_config.h`、`client/src/config/voice_client_config.cpp`。

实现顺序：先写有最大值的`ParseDecimal`，再组合`IsIpv4`；接着检查UUID和pin的编码形式；最后在`LoadVoiceClientConfigFromEnvironment`中表达必填、缺省和成对字段关系。`ReadEnvironment`和标准库作为支持代码。

```sh
python3 scripts/teaching_lab.py 1
```

检查名：`voice-client-config-contract`。应看到合法字段被接受，非法UUID、地址、端口或不成对的地址/pin被拒绝，缺省端口不变。

自行解释：为什么先判断范围再转成`uint16_t`？为什么读取配置不等于生成UUID，更不等于联网成功？常见错误是直接调用转换后截断、忽略空串，或把错误字段的原值写进日志。

## 02 协议：一帧数据从结构变成字节

**本关新增概念：** 采样点/字节数、大小端、首尾标记与编号。

源码：`client/include/boompi/audio/audio_format.h`、`client/src/network/voice_codec.h/.cpp`；契约见[协议v2](../../protocol/protocol-v2.md)。

先根据采样率推导20ms的320/480/960样本；再写整数读写，最后实现`EncodeAudio`、`DecodeAudio`。严格JSON校验先作为已给出的支持代码，理解二进制格式后再回看转义、重复键和类型检查。

```sh
python3 scripts/teaching_lab.py 2
```

检查名虽然叫`protocol-json-contract`，也会读取共享fixture逐字节比对音频编码和解码。上行PCM必须640字节，下行非末帧必须960字节；头部大端，PCM小端。

自行解释：`generation`为什么不是设备ID？`sequence`为什么不能代替generation？常见错误是把320个样本当320字节，或把flags偏移4/5误读成两个独立字段。

## 03 播放：先把一包音频完整地交出去

**本关新增概念：** 有界环、单生产者/消费者、准备/播放/收尾。

源码：`client/src/audio/audio_engine.cpp`与公开头。硬件替身在`client/tests/support/audio_backend.cpp`，只在Host测试中选入；本关真实执行产品队列和线程代码，不打开ALSA设备。

先复现`ClearQueue`与`QueueTts24k`，再看`BeginPlayback`、`EndPlayback`，最后顺着`PlaybackLoop`理解取帧与结束。

当前契约是一包一槽：1～480样本，短帧只能是最后一包，后面不允许继续追加。队列满必须拒绝，不能覆盖尚未播放的槽。使用槽数定位入队位置，样本数只表示实际音频容量与水位。

```sh
python3 scripts/teaching_lab.py 3
```

本关检查队列边界、短尾帧、采集准备与播放准备次序，以及采集/控制/退出的有界等待。应能验证1样本和完整480样本的回答都能结束、481样本被拒绝、75槽容量满时返回背压。

常见错误：收到END立即置完成、把短帧当损坏数据、结束前清空剩余音频、删除采集帧边界的AEC准备握手。先理解这些约束，再读锁与条件变量的具体次序；不要用直接跨线程调用后端替代握手。

## 04 输入：确认开口之后仍保留句首

**本关新增概念：** 历史帧、开口/结束边沿、首次听音与追问。

源码：`client/src/audio/voice_audio.cpp`。先写`SaveHistory`、`QueueBufferedInput`，再看`HandleCapture`中的听音/采集分支和`FinishInput`。

`VoiceAudio::Process`既推进采集帧处理，也返回语义事件，并非单纯读取邮箱。应用必须持续调用；20ms参数只限制一次底层等待，不保证整个调用总耗时不超过20ms。

```sh
python3 scripts/teaching_lab.py 4
```

检查`voice-preroll`和`voice-follow-up-boundary`。合成输入的句首按原顺序补出，开始事件先于PCM，结束标志附着在有效末帧；追问中被拒绝的短句不能把旧END带入下一句。

`ListenMode::Wake`与`ListenMode::FollowUp`明确表达两种准入策略。500ms是普通历史的目标容量，不代表Idle阶段已缓存此前半秒。常见错误是确认开口才开始保存、取消输入时顺带结束播放、混淆算法状态复位与已经采集到的PCM。

## 05 连接：把固定帧通过真实WSS发送

**本关新增概念：** 建链与ready、发送队列、连接级事件。

源码：`client/src/network/voice_link.cpp`、`network_setup.cpp`。先沿`Connect`、`SendHello`、`OnMessage`理解连接，再实现`SendNextFrame`。公钥验证、握手身份、长度边界作为必须保留的支持逻辑，不为获得“能连上”的效果跳过它们。

```sh
python3 scripts/teaching_lab.py 5
```

`voice-transport-loopback`使用真实TCP/TLS/WebSocket及本机测试对端。测试网卡发现是替身，不覆盖真实广播、DHCP或家中Wi-Fi，也不调用云服务。

应看到合法握手和消息往返成功，错误pin、坏消息、超时和重连路径满足断言。常见错误：把`Open`成功当作ready，把本地发送入队当远端收到，队列满时静默跳过PCM。

## 06 问答：把模块接成一个业务流程

**本关新增概念：** 状态×事件→动作，带截止时间的等待。

源码：`client/src/application/voice_client.cpp`，入口`client/apps/boompi_client/main.cpp`。

复现顺序为`Enter`、`WaitForSpeech`、`BeginUpload`、`SendInputFrame`、`OnAudio`、`OnNetwork`。先手写一张六态转换表，再实现对应分支。不要增加第二个状态机来重复表达同一业务。

```sh
python3 scripts/teaching_lab.py 6
```

应用harness直接包含生产VoiceApp，只替换模块I/O和时钟。它验证完整业务，不意味着只写完正常路径就能通过全部检查。

应能沿日志解释Offline→Idle→Listening→Uploading→Waiting→Speaking→追问。音频END和网络Done均不能提前结束Speaking；只有当前generation的PlaybackDone可以开启音频回答后的追问。纯文本没有声卡完成事件，处理方式不同。

`StopAndListen`的名字表示停止后还会重新听音；`ReplyHistory::Keep/Retract`表示是否撤回未听完的回答。常见错误是旧轮完成改变新轮状态，或者停止命令失败后仍显示正常追问。

## 07 插话：从“停声音”到“真正开始新问题”

**本关新增概念：** 回声与近讲、主动停止、跨线程迟到结果。

源码：`VoiceAudio::HandleBarge/ConfirmBarge`、应用`StopAndListen`以及网络`AdvanceLocked`。这是进阶关，不要求第一次学录放音时同时掌握。

先在纸上走一遍候选人声→临时静音→等待低参考→清尾音→确认。再追确认后的顺序：停旧播放，交出保留PCM，应用分配新generation，首帧带START|SUPERSEDE。

```sh
python3 scripts/teaching_lab.py 7
python3 scripts/teaching_lab.py 6
python3 scripts/teaching_lab.py 5
```

音频检查覆盖插话生命周期，以及渲染/drain阻塞时的停止；应用和网络回归继续证明新轮开始与退休控制不能丢失。Host合成帧通过不代表真板没有误触发。

常见错误：只drop扬声器、不替换远端旧轮；删除尚未发送的退休控制；将正常追问也当作撤回上一轮；用用户音量代替会话临时衰减，导致探针结束后音量恢复错误。

## 08 界面：先页面，再看硬件端口

**本关新增概念：** UI快照、动作邮箱、LVGL运行期单线程归属。

先读`ui_view.h`和`LvglScreen`，复现`BuildHome/BuildVoice/RenderVoice`及点击、音量回调；再看`DeviceUi::Show/Poll`如何交接数据。最后到`platform/rv1106/display_touch.h/.cpp`学习具体SPI屏幕与I²C触摸，不把页面动作和寄存器时序混在一个阅读步骤。

本关需要教师提供LVGL 8.2源码、Host SDL2和FreeType，并设置`BOOMPI_LVGL_ROOT`：

```sh
cmake -S . -B build/lesson-ui -DCMAKE_BUILD_TYPE=Debug \
  -DBOOMPI_BUILD_UI_SIMULATOR=ON -DBOOMPI_STRICT_WARNINGS=ON \
  -DBOOMPI_LVGL_ROOT="$BOOMPI_LVGL_ROOT"
python3 scripts/teaching_lab.py 8 --build-dir build/lesson-ui
build/lesson-ui/client/boompi-ui-simulator --demo-voice
```

第8关脚本只保证页面和真实板级显示端口严格编译。`--demo-voice`进入小智页面；`--demo-app 0/1/2/3`分别选择小智、相机、时钟、Wi-Fi（一次只传一个数字）。字体可用`--font /absolute/path/to/font.ttc`指定，默认查找系统Noto Sans CJK。检查文字、状态、音量和画面；截图相同也不能证明真实SPI时序、GT911复位和触摸恢复正确。

相机最后加入：页面只接收最新一帧，采集管线和进程生命周期归`CameraCapture`。保留原有Wi-Fi、时钟和相机功能，不为分课删除正式产品能力。

## 09 合并与真板验收

```sh
python3 scripts/teaching_lab.py 9
python3 scripts/verify_protocol_fixtures.py
python3 -m unittest discover -s scripts/tests -p 'test_*.py' -v
```

第9关要求全部22个既有CTest都存在并通过。Windows原生Host只有部分目标，完整关卡请在Linux/WSL完成，不能把两个Windows测试通过描述成全部客户端验证通过。

然后由教师准备匹配RV1106 GCC/uClibc工具链、sysroot和依赖库，按既有`build_teaching_release.sh`交叉构建并检查ELF。没有匹配SDK时停止在“Host已验证”，不能交付伪装成板端产物的Host程序。

人工验收至少包括：

- 唤醒后不说话，正常回待机；说话时句首完整。
- 短回答、长回答和末尾不足20ms的音频能正常播尽。
- 追问无需再次唤醒；很短的无效声音不污染下一句话。
- 播放中插话确实开始新问题，旧回复和迟到完成不会复活。
- 安静环境不被自己的扬声器反复触发；音量为零与恢复后行为可解释。
- 断网、重连、输入拥塞和退出不会留下继续播放或阻塞线程。
- 字幕、触摸、音量保存、配网、进入/离开相机页面正常。

## 教师如何判断学生真的完成了

不以“能背出类名”或“测试输出绿色”为唯一标准。随机改变一个输入，让学生先预测结果，再运行验证。例如：把最后一包缩成1个样本；在新一轮开始后投递旧完成事件；让发送队列满；在服务端发完后延迟声卡完成。

如果学生能指出由哪个模块拒绝或处理、哪些状态保持不变，以及为什么不应该删掉该检查，就说明这段实现已经能够复现和维护。

## 本轮基座调整与验证记录（2026-09-05）

以`0c0b53c`为基线。正式客户端从34文件/5671 ELOC变为36文件/5746 ELOC（+75）；物理行6975→7080。统计含私有音频与显示驱动，不含测试、教材、资源和第三方。只看DeviceUi时为631→349 ELOC，但移出的347 ELOC显示/触摸端口仍计入客户端总量，不能把移动目录当作删掉代码。

生产改动预算为最多2个新文件、净增加不超过110 ELOC，未新增线程或框架。新增具体DisplayTouch边界、ListenMode/ReplyHistory及明确页面身份；删除任意长度跨槽拼包、持久tts_tail和重复相机状态映射。固定板级参数、协议v2、缓冲时长与已有功能保持。

验证：Linux严格构建与22/22 CTest、Windows基础2/2 CTest、Python16/16、共享协议fixture、HIL的Host编译和`--help`、UI及真实Linux显示端口严格编译。九个实验入口均实际执行；第8关仅编译，不将页面视觉或触摸误报为自动验收。小智/相机/Wi-Fi三张模拟器BMP与基线逐字节一致；11个迁移硬件函数体及87字节面板初始化表经独立比对保持。

未进行匹配RV1106 SDK的交叉构建、部署或真板声学验收。以上为本轮提交前的验证记录，提交版本与远程同步状态以Git为准。剩余复杂度包括音频跨线程时序、AEC/近讲策略和厂商块长适配。独立阶段源码快照尚未制作，不能用这份实验索引替代阶段产物。
