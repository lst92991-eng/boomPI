# boomPI 客户端：教学版 v2

教学复现先从[分关实验](../docs/teaching/README.md)开始：配置、固定帧、播放队列、语句输入、WSS、六态问答、插话、界面，最后完成真板验收。每关复用真实产品源码和已有测试，不用课程宏拼出多个产品。

这是一份源码上的递进补写实验，不是已经导出的独立阶段源码快照。已有基础、只想先理解完整业务时，再读 `src/application/voice_client.cpp`。服务端是配套 EXE，学生只配置 Key；无需学习 Go 或云端 SDK。

```sh
python3 scripts/teaching_lab.py
python3 scripts/teaching_lab.py 1 --build-dir build/lesson-host
```

## 一条主线

```text
Offline → Idle → Listening → Uploading → Waiting → Speaking
                   ↑                            │
                   └──── 物理播放完成后追问 ─────┘
Speaking ── 确认近讲 ──→ Uploading（新 generation，撤回旧回答）
```

- `VoiceApp::Run` 初始化模块后依次处理超时、网络、音频和 UI；`Enter` 同步修改状态、计时和显示。
- `OnAudio` 只理解 Wake、SpeechStart、Pcm、Barge、PlaybackDone、Fault。
- `OnNetwork` 只理解 Online、Offline、Text、Audio、Done、Error。
- `VoiceAudio` 拥有声学判定、500 ms pre-roll、32 帧打断历史和播放生命周期。
- `VoiceLink` 拥有发现、TLS、握手、心跳、重连、协议与有界队列。
- `DeviceUi::Show(UiView)` 发布一个固定大小的显示快照，`Poll` 返回触摸和音量动作。

业务主线不读取 dBFS、硬件参考、SPKI 或握手状态。底层实现仍开放给进阶课程阅读。

`VoiceAudio::Process`明确承担采集判定的推进职责；不是只读取一个事件。`ListenMode::Wake/FollowUp`区分两种听音方式；`StopAndListen`表示停止当前轮后继续等追问。固定20ms的下行音频一包入一个播放槽，仅末帧允许不足一槽，不再提供任意长度跨槽拼包。

## 阅读顺序

| 课程 | 源码入口 | 学生需要解释的事情 |
| --- | --- | --- |
| 1 | application/voice_client.cpp：Run、Enter、OnAudio | 一次发言如何进入上传，最后一帧如何结束 |
| 2 | network/voice_codec.cpp 与 protocol-v2.md | START/END、generation、sequence 的用途 |
| 3 | audio/voice_audio.cpp | 为什么保留句首；播放中近讲如何成为 Barge |
| 4 | audio/audio_engine.cpp | 两条实时线程和固定容量队列的所有权 |
| 5 | platform/rv1106/audio_backend.cpp | 48→16 kHz、3A、Snowboy、VAD 的先后关系 |
| 6 | platform/rv1106/alsa_audio.cpp | Mode1 四通道、period、XRUN 与中断退出 |
| 7 | ui/lvgl_screen.cpp、ui/device_ui.cpp | 页面、音量、摄像头资源的生命周期 |
| 8 | platform/rv1106/display_touch.cpp | SPI屏幕、I²C触摸、复位与故障恢复 |

上述路径相对 `client/src/`。课程按同一份产品代码递进，不用宏拼出多个产品。

读正常问答时，先顺着 `OnAudio → SendInputFrame → OnNetwork → PlayReplyFrame`。
需要了解异常再看 `HandleAudioFault`、`StopAndListen` 和 `OnTimeout`。主状态中不处理声学门限和TLS细节。

采集端从 `AudioBackend::ProcessCapture20ms` 向下看：修正极性、重采样、3A、语音判定和发布。
`audio_format.h` 定义帧格式，`board_voice_profile.h` 保存板级标定，公开音频接口只依赖前者。

平台选择放在CMake。板端编译真实网卡操作和Linux线程调度；Host回归从 `tests/support/` 选择替身。
产品源码不再用条件编译混合测试实现，STDT和AEC delay沿用固定profile。

本项目用根目录 `.clang-format` 统一排版：每行一个语句，条件和循环带大括号，常用列宽96。
注释说明线程归属、硬件时序、数据单位和异常原因；能从代码直接看出的赋值和调用不逐行复述。

## 运行与设置

```sh
boompi-clientctl start
boompi-clientctl status
boompi-clientctl log
boompi-clientctl stop
```

有线优先，Wi-Fi 为备用。无有线时运行 `boompi-clientctl provision`，或从 WiFi 页启动配网。

学生无需配置声学环境变量。新生成的 `/userdata/boompi/config/client.env` 只包含自动 UUID：

```text
BOOMPI_DEVICE_ID=<由脚本生成>
```

音量只从 `ui.settings` 读取并在滑块释放时保存。旧 client.env 的音量/声学键会输出迁移提示，不再改变 profile。教师在共享课堂网络中应预置该组电脑的地址和 pin，避免首次发现邻组服务端：

```text
BOOMPI_SERVER_IP=<该组电脑IPv4>
BOOMPI_SERVER_PORT=17806
BOOMPI_SERVER_SPKI_SHA256=<该电脑稳定SPKI>
```

地址和 pin 必须成对，`--check-config` 会拒绝非法 IPv4。发现本身没有认证，TLS 始终检查已保存的 SPKI。

## 固定板级 profile

`include/boompi/audio/board_voice_profile.h` 是维护者标定入口：

- 左右麦极性 +1/+1；
- Snowboy 0.7；
- raw mic VAD -30 dBFS；
- AEC 后 barge -25 dBFS；
- AEC delay 0。

20 ms、320/480/960 samples、PCM 路径和模型位置由固定帧契约推导/给出。学生不逐板调参。更换硬件或模型后由维护者重新验收整个 profile。

采集保持 48 kHz / S16_LE / 4ch `[mic0,mic1,refL,refR]`，3A 输入为双麦+refL，上传 16 kHz mono；TTS 24 kHz mono 重采样到 48 kHz stereo。原有 AEC/VAD 标定默认值保留，声学效果仍需要真板验收。

## 构建

Host：

```sh
cmake --preset host-debug
cmake --build --preset host-debug --parallel
ctest --preset host-debug
python3 scripts/verify_protocol_fixtures.py
python3 -m unittest discover -s scripts/tests -p 'test_*.py' -v
```

发布验收必须设置 `BOOMPI_REQUIRE_HOST_TRANSPORT_TEST=ON` 并安装 Host OpenSSL、Boost 1.83 与 cJSON，防止真实网络测试被跳过。

教师准备一次 SDK 后，学生只设置根目录：

```sh
export BOOMPI_RV1106_SDK_ROOT=/absolute/path/to/teaching-sdk
sh scripts/build_teaching_release.sh
```

SDK 根目录可以包含教师维护的 `boompi-sdk.cmake`，映射已有的 BOOMPI_* 路径；也可以使用以下布局：

```text
toolchain/bin/arm-rockchip830-linux-uclibcgnueabihf-{gcc,g++,readelf}
sysroot/
rockchip/include/ + rockchip/lib/{libaec_bf_process.so,librkaudio_common.so}
snowboy/include/ + snowboy/lib/{libsnowboy-detect.a,libopenblas.a}
webrtc/include/ + webrtc/lib/libwebrtc_vad.a
boost/include/
openssl/     # 匹配目标ABI的 OpenSSL 3.5.7 config package
lvgl/        # LVGL 8.2
```

不把私有库或模型复制进 Git。Snowboy 旧 C++ ABI 仍仅限 bridge。脚本检查 ELF 后才生成 rootfs 安装目录，不会连接开发板。

## v1 升级

v2 客户端必须与同批 v2 服务端配套。旧 v1 程序留在基线快照/Git 历史，不能混用。首次更新前保留旧客户端、旧服务端、config.yaml 与 state；复用原有身份而非重新配对。协议详见 [protocol-v2.md](../protocol/protocol-v2.md)，人工验收见 [host-validation.md](../docs/test/host-validation.md)。
