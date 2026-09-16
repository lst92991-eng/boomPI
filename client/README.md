# RV1106客户端

唯一程序入口是`apps/boompi_client/main.cpp`。读取配置后依次调用App_Init、循环App_Process、App_Close。

```text
输入：ALSA → 格式转换 → Rockchip 3A → Snowboy → WebRTC VAD
应用：读取处理帧 → 语句/pre-roll → START / PCM / END
回复：WSS事件 → 播放环 → 格式转换/音量 → ALSA
界面：应用快照 → UI线程 → LVGL；触摸动作沿反方向交付应用
```

`src/debug.cpp`注册终端日志回调，业务通过`debug::log`交付事件。硬件和第三方细节集中在`src/platform/rv1106`；只有wake.cpp使用Snowboy需要的旧C++ ABI。

## 编译依赖

使用匹配GCC8.3/uClibc的工具链和sysroot，以及Rockchip 3A、Snowboy/OpenBLAS、WebRTC VAD、Boost、OpenSSL3.5.7、LVGL8.2、FreeType、ALSA和FFmpeg转换库。

通过`BOOMPI_RV1106_SDK_ROOT`与SDK中的`boompi-sdk.cmake`提供路径，也可在本机Git忽略的`CMakeUserPresets.json`中设置现有BOOMPI_*路径。

```sh
cmake --preset rv1106-release
cmake --build --preset rv1106-release --parallel
DESTDIR="$PWD/build/rootfs" cmake --install build/rv1106-release
```

模型与字体安装在`/userdata/boompi/models`和`/userdata/boompi/fonts`，不随源码分发。

## 板端运行

```sh
boompi-clientctl start
boompi-clientctl status
boompi-clientctl log
boompi-clientctl stop
boompi-clientctl update /absolute/path/to/new-client
```

首次启动生成`/userdata/boompi/config/client.env`中的设备UUID。默认局域网发现服务端；固定端点时同时配置BOOMPI_SERVER_IP、BOOMPI_SERVER_PORT和BOOMPI_SERVER_SPKI_SHA256。音量由界面保存到ui.settings。

默认以太网优先，Wi-Fi备用。Wi-Fi凭据直接保存在权限0600的`/etc/wpa_supplicant.conf`中；配置改变后重载系统supplicant。客户端复用已有网络服务，不管理AP配网页。

升级启动脚本前先使用原脚本停止旧客户端。设备配置、服务器身份缓存、字体和模型保留。
