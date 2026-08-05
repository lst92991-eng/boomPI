# 音频后端与外部依赖

## 产品边界

`AudioEngine` 是 application 唯一看到的音频接口；真正的板级工作集中在
`client/src/platform/rv1106/`：

- ALSA capture/playback；
- Codec Mode1 数字回采；
- capture 48→16 kHz 与 TTS 24→48 kHz 重采样；
- Rockchip `librkaudio` 3A；
- Snowboy 旧 ABI bridge；
- WebRTC VAD、播放增益和 limiter。

产品没有 backend 工厂或模拟设备分支。Host fake 和 AEC HIL 只用于测试，不链接进
`boompi-client`。

## ALSA 与 Mode1

| 方向 | 格式 | 通道 | period / buffer |
| --- | --- | ---: | --- |
| capture | 48 kHz / S16_LE | 4 | `960 / 1920` frames |
| playback | 48 kHz / S16_LE | 2 | `960 / 3840` frames |

capture 布局固定为 `[mic0,mic1,refL,refR]`。TTS mono 被复制到左右声道，因此两个参考高度
相关；产品 AEC 只消费 `refL`，`refR` 只保留给 HIL 诊断。

## Rockchip 3A

```text
rkaudio_preprocess_init(16000, 16, 2, 1, parameters)
input  = interleaved [mic0,mic1,refL]
output = 16 kHz / S16 / mono
```

vendor 每块处理 256 samples，产品每帧 320 samples。`RockchipVoiceDsp` 用固定 FIFO 对齐，
因此输出比采集固定延迟一帧；发布给 application 的 monotonic timestamp 同步补偿这帧延迟。

当前 profile 保持 AEC + BF、FastAEC、AES、ANR、去混响和 STDT，固定 delay 为 0；vendor AGC
关闭。公开 ABI 没有可靠 DTD 事件，因此打断仍使用 3A 后 PCM 的 VAD 和 `voice_dbfs`。

## Snowboy ABI

只有 `snowboy_legacy_bridge.cpp` 包含 Snowboy C++ 头并使用
`_GLIBCXX_USE_CXX11_ABI=0`。边界外只传 PCM、长度和不透明 handle，旧 ABI 不得扩散到整个
客户端。

## 唯一板端构建入口

```sh
cmake --preset rv1106-release
cmake --build --preset rv1106-release --parallel
```

Host 配置不读取私有 SDK。板端构建要求以下外部输入存在且匹配当前 BSP：

- `BOOMPI_RV1106_TOOLCHAIN_ROOT`、`BOOMPI_RV1106_SYSROOT`；
- `BOOMPI_ROCKCHIP_3A_INCLUDE_DIR`、`BOOMPI_ROCKCHIP_3A_AEC_LIBRARY`、
  `BOOMPI_ROCKCHIP_3A_COMMON_LIBRARY`；
- `BOOMPI_SNOWBOY_INCLUDE_DIR`、`BOOMPI_SNOWBOY_LIBRARY`、`BOOMPI_OPENBLAS_LIBRARY`；
- `BOOMPI_WEBRTC_VAD_INCLUDE_DIR`、`BOOMPI_WEBRTC_VAD_LIBRARY`；
- `BOOMPI_BOOST_INCLUDE_DIR`、`BOOMPI_OPENSSL_ROOT`、`BOOMPI_LVGL_ROOT`。

CMake 只检查路径、头文件、库文件和目标版本，不维护开发机文件哈希清单。私有路径从环境变量
或 Git 忽略的 `CMakeUserPresets.json` 注入，不写入仓库。模型、vendor 库和 BSP 资产未经许可
不得重新分发。

Host 测试只能证明状态和数据 packing，交叉构建只能证明 ABI；Mode1 布局、AEC、自激和最终
声学效果必须在目标板验证。命令见 [验证入口](../test/host-validation.md)。
