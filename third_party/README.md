# 第三方依赖

教学流程：**git clone固定版本 → 阅读真实API → 编译/选择匹配ARM库 → 开发本项目接入代码**。独立依赖集中在这里，CMake不再引用个人临时构建目录。

嵌套Git仓库、二进制及构建目录由父仓库忽略；它们仍实际存在于third_party，能在编辑器中查看。不要把上游源码改写成本项目代码，也不要提交其构建产物。

## 1. 克隆公开仓库

从boomPI根目录执行；已有目录先核对版本，不覆盖本地修改：

```sh
git clone --depth 1 --branch openssl-3.5.7 https://github.com/openssl/openssl.git third_party/openssl

git clone https://github.com/Kitt-AI/snowboy.git third_party/snowboy
git -C third_party/snowboy checkout --detach c9ff036e2ef3f9c422a3b8c9a01361dbad7a9bd4

git clone https://github.com/wiseman/py-webrtcvad.git third_party/webrtc-vad
git -C third_party/webrtc-vad checkout --detach e283ca41df3a84b0e87fb1f5cb9b21580a286b09

git clone https://github.com/OpenMathLib/OpenBLAS.git third_party/openblas
git -C third_party/openblas checkout --detach 1bd74ad3d1e8d21f86d1a6be35abfcdf27c0208a
```

| 目录 | 内容与接入方式 |
| --- | --- |
| `openssl/` | OpenSSL 3.5.7；commit `8cf17aaeb4599f8af87fefd810b5b5fee90fe69e`，在自身build/rv1106目录交叉编译 |
| `snowboy/` | 上游头文件、模型和预编译库；直接使用lib/rpi/libsnowboy-detect.a，核心识别器不以完整源码交付 |
| `webrtc-vad/` | py-webrtcvad中的WebRTC C/C++部分；客户端CMake直接编译，不使用Python绑定或单独的旧静态库 |
| `openblas/` | Snowboy使用的数学库源码；当前课程采用下述已验证ARM库 |
| `boost/boost/` | 1.74头文件，来自配套开发环境的同版头文件包 |
| `lvgl/` | 匹配幸狐SDK的LVGL 8.2.0源码快照 |
| `rockchip/` | 匹配SDK的rkaudio_preprocess.h和两份动态库；这是专有SDK输入，不是公开算法仓库 |
| `websocketpp/` | 已跟踪的WebSocket++ 0.8.2头文件及许可证，网络评估前保持原版 |

公开库保留各自LICENSE。Rockchip资源只在匹配SDK许可范围内本地使用，不提交父仓库。上游自身的测试文件保留原样，不加入客户端构建。

## 2. 构建OpenSSL

在Linux构建主机执行，工具链与客户端使用同一套SDK：

```sh
export BOOMPI_RV1106_SDK_ROOT=/path/to/luckfox-pico
export PATH="$BOOMPI_RV1106_SDK_ROOT/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin:$PATH"
mkdir -p third_party/openssl/build/rv1106
cd third_party/openssl/build/rv1106
../../Configure linux-armv4 \
  --cross-compile-prefix=arm-rockchip830-linux-uclibcgnueabihf- \
  --prefix=/usr --openssldir=/etc/ssl \
  no-shared no-tests no-docs no-module no-comp no-weak-ssl-ciphers
make -j4 build_libs
cd ../../../..
```

CMake读取这个目录生成的OpenSSLConfig.cmake、头文件和静态库，不执行系统安装。

## 3. 准备同版配套资源

以下文件来自教师提供的依赖包或同一套开发环境，目录必须与CMake默认布局一致：

```text
third_party/
  openblas/build/rv1106/libopenblas.a
  boost/boost/asio.hpp
  lvgl/lvgl.h
  rockchip/include/rkaudio_preprocess.h
  rockchip/lib/libaec_bf_process.so
  rockchip/lib/librkaudio_common.so
```

当前基线的来源与SHA-256：

- Snowboy RPi archive：`346db1193490a9cc404d49fcfb22ca612cd3a0e649c4863f411553eb1c4f9f1f`。
- OpenBLAS ARMV7库：`fabfc588e0e0d94f3655d4ad5515e0c90fd161f016be5261e2f11d3df77a3e9d`。课程配套包提供这份已验证ARM产物，客户端直接链接该静态库。
- LVGL源码快照来自幸狐SDK commit `994243753789e1b40ef91122e8b3688aae8f01b8` 的 `project/app/component/lvgl/lvgl`。
- Rockchip头文件和库取自匹配SDK的 `output/out/media_out/include`、`output/out/media_out/lib`。不要替换成其他板卡的同名文件。

工具链、Linux驱动与sysroot仍属于幸狐SDK。ALSA、FreeType和FFmpeg转换库由sysroot提供，不复制整套BSP到third_party。

## 4. 从API读到调用

| 第三方API | 本项目实现 | 需要处理的边界 |
| --- | --- | --- |
| ALSA `snd_pcm_*` | `audio_capture.cpp`、`playback.cpp` | PCM格式、短读写、断流和关闭 |
| `rkaudio_preprocess_*` | `rockchip_3a.cpp` | SDK参数树、通道排列、256/320点适配 |
| `snowboy::SnowboyDetect` | `wake.cpp` | 模型、结果转换与旧C++ ABI隔离 |
| `WebRtcVad_*` | `vad.cpp` | 错误、静音、当前帧命中与语音延续 |
| `swr_*` | `audio_convert.cpp` | 重采样、声道矩阵和滤波尾音 |
| WebSocket++ / Boost / OpenSSL | `voice_net.cpp` | WSS、TLS身份和协议交付 |
| LVGL / FreeType | `device_ui.cpp`、`lvgl_screen.cpp` | 字体、页面与SPI/I²C端口 |

Snowboy旧ABI只用于wake.cpp。WebRTC在这里仅提供VAD。服务端依赖由自己的go.mod/go.sum管理，学生无需学习其内部实现。

VAD封装使用上述固定提交的`WebRtcVad_CalcVad16khz`内核接口，保留当前帧命中与延续结果；内核头文件和静态库由同一源码构建。升级依赖时需一并核对这一返回约定。
