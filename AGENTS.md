# boomPI 开发约定

本仓库是 RV1106 语音 AI 教学项目。让学生能顺着输入、处理、交付理解真实产品，不展示企业框架。当前非音频精简由用户于2026-09-16授权，独立分支为`codex/minimal-non-audio`。

## 1. 当前范围与冻结基线

- 音频源码、相关头文件与内部profile、Go服务端、BPV4协议以`2ca0cf63d241a032292f4d83a3120002b91a066e`冻结。本次只改非音频；问答应用只适配UI调用，不重写语音状态机。
- 保留双麦/单参考3A、Snowboy、WebRTC VAD、500ms前滚、流式问答、播放中插话和三秒追问。
- UI只保留320×240语音与摄像头两页、静态表情、字幕、触摸和音量；移除桌面、独立时钟、二维码/AP配网。
- 保留以太网优先、Wi-Fi回退、发现/服务器身份缓存和SC3336预览。SSID/密码直接编辑板端`/etc/wpa_supplicant.conf`，权限0600。
- YOLO、音乐、长期记忆、账号系统、OTA和多模态上传不实现，不预留空接口。
- 旧设计文档只作历史对照，当前实现和接口以`docs/test/minimal-non-audio.md`、`docs/test/compact-barge-in.md`及实际源码为准。

## 2. 简化和教学原则

1. 初始化顺序和任务内“读取→处理→交付”必须可见。普通函数或简洁类服从真实职责，不按语法形式机械替换。
2. namespace承载真实实现和资源，不给旧类再套转发壳。不要恢复VoiceAudio/AudioEngine/AudioTasks/AudioPipeline聚合层，不新增manager、消息总线或通用worker。
3. 同一个状态只有一个owner。内部前提成立后不层层重复校验；设备、网络、文件和生命周期边界的真实失败必须处理。
4. 不压行、不合并多条语句、不把实现搬到头文件、脚本或测试来伪造删除。注释解释硬件、时序、并发与原因，教学步骤用简短自然中文。
5. 新增/删除跨模块接口时汇报用途、输入输出、调用者和旧接口去向。参数只传实际消费者需要的事实，不为未来保留字段。
6. 不修改官方/第三方源码、库二进制或生成资源；兼容问题只在自有调用、桥接和构建配置中处理。
7. 不静默丢PCM、跳过sequence hole、无界排队或无限阻塞重试；预览图像允许只保留最新完整帧，语音不可照搬这个丢帧策略。
8. 用户提供的XIAOZHI_CODE和vscode_motor_gateway只参考实际业务流程；排除测试/演示/生成资源，不移植其芯片、RTOS、协议和算法预置。

## 3. 目录和所有权

```text
client/src/application/      App_Init/App_Process/App_Close与四态问答
client/src/audio/            输入任务、语句/pre-roll、播放任务
client/src/platform/rv1106/  ALSA/重采样/3A/wake/VAD及显示触摸硬件
client/src/network/          网卡准备/发现，以及独立WSS协议
client/src/config/           启动配置与身份校验
client/src/ui/               ui/page/camera_capture直接实现
server/                     配套Go程序，学生只配置Key
```

| 上下文 | 职责 |
| --- | --- |
| application主线程 | speech、四态问答、UI快照、动作消费 |
| 输入线程，SCHED_FIFO40 | ALSA→格式适配→3A→wake→VAD→有界交接 |
| 播放线程，SCHED_FIFO30 | TTS采样环→重采样/增益→ALSA；取消/尾播 |
| 网络线程 | 网卡准备、TLS/WSS、心跳重连、generation/sequence |
| UI线程 | 运行期LVGL、触摸、字幕、音量提交、页面切换 |
| 摄像头线程 | 管线读取、整帧拼装、最新帧交接、管线回收 |

UI资源在启动线程前由主线程初始化；join后再销毁，两阶段不能与运行期交叉。UI线程不把LVGL对象指针交给应用或采集。UI/网络/文件等待不能进入音频实时线程。调度设置失败记录warning，不能盲目提升UI优先级。

## 4. 不可凭猜测修改的音频事实

```text
capture: 48kHz / S16_LE / 4ch / 20ms
layout: [mic0,mic1,refL,refR]
3A input: 16kHz [mic0,mic1,refL]
3A output / TTS: 16kHz mono
playback: 48kHz stereo
```

TTS左右相同，仅refL进入3A。当前使用`rkaudio_preprocess_init/short/destory`与RKAUDIOParam树，256点vendor块适配320点交付；不与RKAP/.bin API混用。vendor AGC关闭。AEC delay/极性/Mode1回采等未经实测不更改。

两路PCM配置完成后才首次采集；任务退出后释放句柄。wake/VAD由输入线程独占，Snowboy外部VAD句尾Reset只通过单向标志请求；不恢复主线程直接reset vendor或通用握手命令槽。wake.cpp单独旧C++ ABI，不把`_GLIBCXX_USE_CXX11_ABI=0`扩散。

普通提问与插话共用PCM/pre-roll及交付，不要求相同准入条件。当前插话候选→有界参考复核必须保留：确认前不得退休旧轮；暂停消费TTS而非乘零吞字，拒绝/超时恢复，确认才取消旧播放。参考和已写静音观测不等于完成声学验收。允许保留无敏感数据的probe/rejected/confirmed诊断。

学生侧不开放声学校准、极性和模型路径；内部profile由维护者依据板型验证。应用不处理dBFS/reference、hello或heartbeat。此次音频以固定SHA冻结，不为1200物理行配额删保护。

## 5. UI、网络和安全

- 两页一次创建，切换容器隐藏/显示；不恢复通用路由、PImpl转发、启动回执或配网子进程。
- 页面只处理控件与动作。UI接收一份完整显示快照；摄像头直接交付到页面稳定像素缓冲，不增加UI中间整帧副本。
- LVGL8.2中`lv_init`已建立进程级FreeType缓存，不重复初始化。页面只释放自己的字体；端口负责显示draw_ctx回收。
- ST7789P3原生240×320→横屏320×240、SPI目标80MHz、GT911地址/时序/确认/恢复保持当前实现。没有板端证据不改寄存器表和等待。
- 网络有线优先，失败有界；有线有IP但WSS未READY时可换Wi-Fi。发现与WSS均绑定所选接口。不在正常Wi-Fi工作中强制抢占回有线。
- 复用系统已有supplicant/DHCP，不重复创建服务；配置改动后由维护者重新加载系统服务。真实凭据不进Git、日志或命令行。
- WSS保留TLS/SPKI校验、固定教学hello口令、有界收发、超时与取消。首次UDP发现是可信课堂内TOFU，不是认证；不得暴露公网。
- generation/sequence由网络分配和校验，应用不再复制轮次状态；START/PCM/END/CANCEL分别传输，DONE与声卡Drained严格区分。断线不重放旧问题。
- Go服务端无数据库，API Key只在电脑端配置；本轮不改服务端，不调用付费API。
- 默认不记录原始PCM或完整对话。明文Wi-Fi仅存板端权限0600配置；服务端身份与密钥不能随升级重新生成。

## 6. 构建、统计和验证

```sh
cmake --preset host-debug
cmake --build --preset host-debug --parallel
ctest --preset host-debug
python3 scripts/verify_protocol_fixtures.py
python3 -m unittest discover -s scripts/tests -p 'test_*.py' -v
python3 scripts/measure_teaching.py
cd server && go test ./... && go vet ./...
```

完整UI检查还需`BOOMPI_BUILD_UI_SIMULATOR=ON`和匹配LVGL8.2源码；`teaching-ui`工作流编译真实页面及Linux端口，执行页面/生命周期、netns和sanitizer回归。测试仅替换实际硬件或外部I/O，不能把产品主线整体替换后声称验收。

```sh
cmake --preset rv1106-release
cmake --build --preset rv1106-release --parallel
```

优先复用`BOOMPI_RV1106_SDK_ROOT`、已有BOOMPI_*及忽略的CMakeUserPresets.json；缺变量先查明确的本地preset/主机记录，不搜索相邻SDK。匹配GCC/uClibc工具链严格交叉编译，检查ELF架构、loader、依赖和RPATH。缺SDK明确报告未完成，不用Host程序冒充ARM产物。

C++17、Go使用gofmt；使用根.clang-format，一行一条语句，条件/循环带大括号。普通namespace函数用snake_case，App入口保留前缀，第三方/C ABI不改。原子变量默认标准内存顺序，未测量不自行优化。

统计包含入口、src/include、私有驱动和头文件，物理行/ELOC分别列出；脚本、Go、测试、资源和vendor单列。预算是计划，未达到就给实际数字和原因，不压排版凑数。

提交前核对冻结范围、正常路径、失败回收、接口调用和构建选源。汇报严格区分实现、Host检查、交叉构建、部署、真板验收；CI绿色不代表音频零自激或实际Wi-Fi/触摸通过。

保留用户已有修改，不强推、不重写历史、不删除v1.0.0 tag。原音频验收分支保持2ca0cf6。升级前先用旧clientctl停止旧客户端/AP，再安装新版本；刷镜像、改设备树、分区或板端状态必须另行确认目标和恢复路径。
