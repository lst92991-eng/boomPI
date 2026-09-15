# boomPI 客户端：教学版 v2

从 [main](apps/boompi_client/main.cpp) 的四个中文步骤开始阅读：LoadClientConfig、App_Init、循环 App_Process、App_Close。命令行辅助功能和信号处理的定义在本文件下方。完整调用路径见[源码阅读索引](../docs/teaching/client-source-reading.md)，本次接口变化与验证范围见[可读性重构记录](../docs/architecture/client-readability-refactor.md)。

教学复现先从[分关实验](../docs/teaching/README.md)开始：配置、固定帧、播放队列、语句输入、WSS、四态问答、插话、界面，最后完成真板验收。每关复用真实产品源码和已有测试，不用课程宏拼出多个产品。

音频可以从[顺序数据流](../docs/teaching/audio-pipeline.md)进入：`RawCaptureFrame → CaptureChannels → CaptureFrame → speech::Result（借用PCM）`。各处理模块显式接收上一阶段输出，播放是另一条独立链。

这是一份源码上的递进补写实验，不是已经导出的独立阶段源码快照。已有基础、只想先理解完整业务时，再读 `src/application/voice_client.cpp`。服务端是配套 EXE，学生只配置 Key；无需学习 Go 或云端 SDK。

```sh
python3 scripts/teaching_lab.py
python3 scripts/teaching_lab.py 1 --build-dir build/lesson-host
```

## 一条主线

App_Init按采集、播放、网络初始化，App_Process顺序执行收回复、处理语音和触摸。应用直接调用namespace模块：

```text
输入任务：ALSA → 转换 → 3A → wake → VAD
应用：voice_input::read → speech::update → voice_net::start/send/end
voice_net::poll → playback::write/finish → status(Drained)
```

speech只拥有语句确认与句首缓存，应用拥有追问和插话决策，不启动线程或转发回复。输入任务顺序执行ALSA/转换/3A/wake/VAD，应用只处理speech与问答；没有检测器跨线程命令握手或四阶段插话试探。playback拥有播放队列、转换器和声卡，旧聚合层未恢复。

START、PCM、END、CANCEL为不同协议消息；generation隔离旧轮，sequence检查连续PCM。DONE关闭播放输入，实际尾播之后才追问。保持唤醒、VAD、500ms句首、插话确认、三秒追问和全部UI/配网/摄像头功能。

## 阅读顺序

先读[真实数据流](../docs/teaching/audio-pipeline.md)，再看[模块所有权](../docs/architecture/audio-runtime.md)。应用入口为application/voice_client.cpp，采集/语句/播放在audio/三个namespace模块，协议在network/voice_net.cpp和[BPV4](../protocol/protocol-v4.md)。硬件与vendor细节留在platform/rv1106。

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

## 内部板级预置

`src/platform/rv1106/board_voice_profile.h` 只由内部实现引用。学生侧没有声学校准项；以下沿用常量（本轮真板未验证）按硬件事实和声学预置分组：

- 左右麦极性 +1/+1；
- Snowboy 0.7；
- AEC delay 0。

网络每 20 ms 双向均为 320 samples，声卡当前每通道 960 samples；PCM 路径和模型位置由维护者预置。vendor 256 点块独立适配，不能把这些粒度混为一条约束。学生不逐板调参。更换硬件或模型后由维护者重新验收整个 profile。

采集保持 48 kHz / S16_LE / 4ch `[mic0,mic1,refL,refR]`，3A 输入为双麦+refL，上传和 TTS 均为 16 kHz mono；仅在声卡边界转成 48 kHz stereo。hello/ready 均必须声明 `HELLO/READY 4 16000`，旧 24 kHz 服务端不能配套。原有 AEC/VAD 默认值保留，整板 16 kHz 能力、声学效果和新 TTS 实际体验仍需要真板验收。

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

## 旧版升级

v4 客户端必须与同批 v4 服务端配套。旧 v1/v2 程序留在基线快照/Git 历史，不能混用。首次更新前保留旧客户端、旧服务端、config.yaml 与 state；复用原有身份而非重新配对。协议详见 [protocol-v4.md](../protocol/protocol-v4.md)，人工验收见 [host-validation.md](../docs/test/host-validation.md)。

当前职责、实际流程和预算例外以[本轮记录](../docs/test/budget-refactor.md)为准；旧源码阅读记录仅作历史参考。
