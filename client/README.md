# boomPI 客户端：教学版 v2

先读 `src/application/voice_client.cpp`。服务端是配套 EXE，学生只配置 Key；无需学习 Go 或云端 SDK。

## 一条主线

```text
Offline → Idle → Listening → Uploading → Waiting → Speaking
                   ↑                            │
                   └──── 物理播放完成后追问 ─────┘
Speaking ── 确认近讲 ──→ Uploading（新 generation，撤回旧回答）
```

- `VoiceApp::Run` 轮询网络、音频和 UI；`Enter` 是唯一状态赋值入口。
- `OnAudio` 只理解 Wake、SpeechStart、Pcm、Barge、PlaybackDone、Fault。
- `OnNetwork` 只理解 Online、Offline、Text、Audio、Done、Error。
- `VoiceAudio` 拥有声学判定、500 ms pre-roll、32 帧打断历史和播放生命周期。
- `VoiceLink` 拥有发现、TLS、握手、心跳、重连、协议与有界队列。
- `DeviceUi::Show(UiView)` 发布一个固定大小的显示快照，`Poll` 返回触摸和音量动作。

业务主线不读取 dBFS、硬件参考、SPKI 或握手状态。底层实现仍开放给进阶课程阅读。

## 阅读顺序

| 课程 | 源码入口 | 学生需要解释的事情 |
| --- | --- | --- |
| 1 | application/voice_client.cpp：Run、Enter、OnAudio | 一次发言如何进入上传，最后一帧如何结束 |
| 2 | network/voice_codec.cpp 与 protocol-v2.md | START/END、generation、sequence 的用途 |
| 3 | audio/voice_audio.cpp | 为什么保留句首；播放中近讲如何成为 Barge |
| 4 | audio/audio_engine.cpp | 两条实时线程和固定容量队列的所有权 |
| 5 | platform/rv1106/audio_backend.cpp | 48→16 kHz、3A、Snowboy、VAD 的先后关系 |
| 6 | platform/rv1106/alsa_audio.cpp | Mode1 四通道、period、XRUN 与中断退出 |
| 7 | ui/lvgl_screen.cpp、ui/device_ui.cpp | 页面、触摸、音量、摄像头资源的生命周期 |

上述路径相对 `client/src/`。课程按同一份产品代码递进，不用宏拼出多个产品。

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
