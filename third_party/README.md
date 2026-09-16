# Third-party dependencies

本目录当前跟踪WebSocket++ 0.8.2头文件及许可信息。项目还使用下表中的依赖，由匹配SDK或本机CMake配置提供；没有复制进本目录不代表没有使用。

接入顺序是固定版本和来源、阅读真实API、确认格式/ABI/生命周期，再决定直接调用还是增加必要适配。源码、预编译库、模型和工具链分别管理，不默认所有依赖都能从公开源码重新构建。

增加或升级依赖时记录版本或 commit、上游地址、许可证、RV1106 ABI 和是否允许再分发。没有结论时只能作为本机外部输入，不能提交。

| 依赖 | 固定版本/来源 | 许可与仓库策略 |
| --- | --- | --- |
| [WebSocket++](https://github.com/zaphoyd/websocketpp) | 0.8.2；当前仓库内为所需头文件子集 | BSD 3-Clause；保留 `websocketpp/NOTICE.debian` 和 `websocketpp/README.boompi.md` |
| [Kitt-AI Snowboy](https://github.com/Kitt-AI/snowboy/commit/c9ff036e2ef3f9c422a3b8c9a01361dbad7a9bd4) | commit `c9ff036e2ef3`; RPi archive SHA-256 `346db1193490a9cc404d49fcfb22ca612cd3a0e649c4863f411553eb1c4f9f1f` | 仓库许可证适用于其代码、库、资源和默认 `snowboy.umdl`；其他模型需单独检查。runtime/model 保持外部输入 |
| OpenBLAS | commit `1bd74ad3d1e8d21f86d1a6be35abfcdf27c0208a` | BSD 3-Clause；只作为 Snowboy bridge 的外部静态库 |
| [OpenSSL 3.5.7](https://github.com/openssl/openssl/releases/tag/openssl-3.5.7) | 3.5.7 | Apache-2.0；RV1106 package 保持外部输入，CMake 只校验目录与 package 版本 |
| Rockchip 3A | 与目标 BSP `media/common_algorithm/out` 匹配 | Vendor SDK 条款；再分发未确认，头文件、配置和库不得提交 |
| WebRTC VAD | 与目标镜像 ABI 匹配的外部头文件和静态库 | 上游许可随实际来源记录；仓库只保存接入代码 |
| LVGL 8.2 / FreeType / Boost | 与 CMake 入口匹配的外部源码或 sysroot 依赖 | 遵循各自上游许可；大型源码、完整字体和构建产物不进入本目录 |

构建输入与ABI约束见[客户端说明](../client/README.md)和[硬件约束](../docs/hardware/README.md)。秘密、下载缓存、构建产物和许可证不明的资产不得进入源码树。

## 从依赖读到本项目调用

| 依赖接口 | 本项目调用位置 | 适配理由 |
| --- | --- | --- |
| ALSA `snd_pcm_*` | `audio_capture.cpp`、`playback.cpp` | 声卡格式、短读写、断流和生命周期 |
| `rkaudio_preprocess_*` | `rockchip_3a.cpp` | SDK参数树、通道排列、256点块到320点交付 |
| `snowboy::SnowboyDetect` | `wake.cpp` | 模型初始化、结果转换，旧C++ ABI只局限此文件 |
| `WebRtcVad_*` | `vad.cpp` | 直接调用WebRTC VAD部分，区分错误/静音/人声 |
| `swr_*` | `audio_convert.cpp` | 共同重采样、声道矩阵与播放滤波尾音 |
| WebSocket++ / Boost / OpenSSL | `voice_net.cpp` | 持久WSS、TLS身份、异步连接与协议交付 |
| LVGL / FreeType | `device_ui.cpp`、`lvgl_screen.cpp` | 单页小智界面、中文字体与SPI/I²C端口 |

这里的WebRTC依赖指VAD组件，不是另一套完整实时音视频框架。Go服务端的依赖由`server/go.mod`和`go.sum`固定，与板端SDK分开。
