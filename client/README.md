# RV1106客户端

唯一程序入口是`apps/boompi_client/main.cpp`。无参数直接运行，读取配置后依次调用App_Init、循环App_Process、App_Close。程序自己持有单实例锁，退出时释放。

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

安装目录还包含匹配SDK的libaec_bf_process.so和librkaudio_common.so，安装到/usr/lib供系统加载器查找。其余运行库由BSP提供，运行不需要设置LD_LIBRARY_PATH。SDK库只在许可范围内用于本机构建/部署，不提交Git。

## 板端运行

```sh
/userdata/boompi/bin/boompi-client
```

程序在前台运行，日志直接输出到终端。Ctrl+C或发送SIGTERM会结束主循环并回收线程、算法和设备；程序不自行fork到后台。

首次启动会创建`/userdata/boompi/config/client.conf`并持久化设备UUID。默认局域网发现服务端；固定端点时在同一文件补充以下字段，地址和pin必须成对：

```ini
device_id=<自动生成的UUID>
server_ip=<电脑IPv4>
server_port=17806
server_spki_sha256=<电脑稳定SPKI>
```

文件是普通key=value配置，不执行Shell、不读取BOOMPI_*运行环境变量。已有配置无效时明确失败，不覆盖或重新生成身份。音量仍由界面保存到ui.settings。

默认以太网优先，Wi-Fi备用。Wi-Fi凭据直接保存在权限0600的`/etc/wpa_supplicant.conf`中；配置改变后重载系统supplicant。客户端复用已有网络服务，不管理AP配网页。

## 开机与升级

手动运行不依赖任何启动脚本。需要开机运行时，由BSP的BusyBox init直接启动，例如在系统`/etc/inittab`配置：

```text
::once:/userdata/boompi/bin/boompi-client
```

该入口在系统初始化完成后启动一次，日志进入系统控制台。它不负责应用自动重试或升级。升级前发送SIGTERM并等旧进程退出，保留旧二进制与配置，再安装新程序；不要覆盖正在运行的可执行文件。
