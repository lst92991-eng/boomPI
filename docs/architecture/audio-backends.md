# 音频模块与外部依赖

## 产品边界

application直接调用voice_input、speech、playback；两条任务直接执行各算法与声卡模块。
板级工作集中在`client/src/platform/rv1106/`：

- ALSA capture/playback；
- Codec Mode1 数字回采；
- capture 48→16 kHz 与 TTS 16→48 kHz 重采样；
- Rockchip `librkaudio` 3A；
- Snowboy 旧 ABI编译单元；
- WebRTC VAD。播放增益和limiter直接位于playback。

产品没有 backend 工厂或模拟设备分支。Host fake 和 AEC HIL 只用于测试，不链接进
`boompi-client`。

`voice_input.cpp`直接展开`raw → channels → frame.pcm → wake/VAD → 交付`：audio_convert负责格式、rockchip_3a负责3A分块、wake和vad负责检测。详见[顺序音频教学](../teaching/audio-pipeline.md)；
需要检查 PCM 参数协商、XRUN 或有界 drain 时再进入 `audio_capture.cpp`。
ALSA头和句柄留在私有设备模块；不存在VoiceAudio/Engine/Backend兼容层。

## ALSA 与 Mode1

| 方向 | 格式 | 通道 | period / buffer |
| --- | --- | ---: | --- |
| capture | 48 kHz / S16_LE | 4 | 请求40ms延迟，ALSA协商period/buffer |
| playback | 48 kHz / S16_LE | 2 | 请求80ms延迟，ALSA协商period/buffer |

capture 布局固定为 `[mic0,mic1,refL,refR]`。TTS mono 被复制到左右声道，因此两个参考高度
相关；产品 AEC 只消费 `refL`。ALSA仍读取四通道，应用不再额外复制一份HIL诊断平面。

应用首包直接调用playback::write；仅主流程决定何时收集插话。
播放线程独自prepare/write/drain；没有采集控制命令槽和100ms检测器握手。

## Rockchip 3A

```text
rkaudio_preprocess_init(16000, 16, 2, 1, parameters)
input  = interleaved [mic0,mic1,refL]
output = 16 kHz / S16 / mono
```

当前调用以256 samples块处理，产品每帧320 samples；这不证明SDK只能使用256点。rockchip_3a用固定FIFO对齐，
因此输出比采集固定延迟一帧；电平与参考快照已删除，不再需要附带元数据延迟。

当前3A配置保持 AEC + BF、FastAEC、AES、ANR、去混响和 STDT，board_voice_profile.h 中 delay 为 0；vendor AGC
关闭。一次语句确认只采用3A后PCM的VAD；不把vendor双讲功能位当作已验证的用户插话事件，真实声学效果待验。

## Snowboy ABI

只有wake.cpp包含Snowboy C++头，并以`_GLIBCXX_USE_CXX11_ABI=0`单独编译。namespace接口只传PCM及整数/布尔值，不让string、异常或厂商对象跨ABI；旧C桥和额外句柄已经删除。

## 唯一板端构建入口

```sh
cmake --preset rv1106-release
cmake --build --preset rv1106-release --parallel
```

Host 配置不读取私有 SDK。推荐教师提供一个 BOOMPI_RV1106_SDK_ROOT，详见 [客户端README](../../client/README.md)。其清单映射以下已有外部输入，全部必须匹配当前 BSP：

- `BOOMPI_RV1106_TOOLCHAIN_ROOT`、`BOOMPI_RV1106_SYSROOT`；
- `BOOMPI_ROCKCHIP_3A_INCLUDE_DIR`、`BOOMPI_ROCKCHIP_3A_AEC_LIBRARY`、
  `BOOMPI_ROCKCHIP_3A_COMMON_LIBRARY`；
- `BOOMPI_SNOWBOY_INCLUDE_DIR`、`BOOMPI_SNOWBOY_LIBRARY`、`BOOMPI_OPENBLAS_LIBRARY`；
- `BOOMPI_WEBRTC_VAD_INCLUDE_DIR`、`BOOMPI_WEBRTC_VAD_LIBRARY`；
- `BOOMPI_BOOST_INCLUDE_DIR`、`BOOMPI_OPENSSL_ROOT`、`BOOMPI_LVGL_ROOT`。

CMake 只检查路径、头文件、库文件和目标版本，不维护开发机文件哈希清单。私有路径从环境变量
或 Git 忽略的 `CMakeUserPresets.json` 注入，不写入仓库。模型、vendor 库和 BSP 资产未经许可
不得重新分发。

Host回归编译tests/support/audio_thread.cpp，使用普通线程；板端由platform/rv1106/audio_thread.cpp设置SCHED_FIFO。实际是否获得40/30优先级，仍需在板端读取线程策略。

Host模块测试会执行真实重采样、DSP分块和检测策略，但厂商内核及语音分类用窄替身控制。交叉构建还需检查ELF与动态依赖，不能据此保证厂商二进制实际兼容；Mode1布局、AEC、自激和最终声学效果必须在目标板验证。命令见[验证入口](../test/host-validation.md)。
