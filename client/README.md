# RV1106客户端

唯一程序入口是`apps/boompi_client/main.cpp`。无参数直接运行，main只组织App_Init、循环App_Process、App_Close。App_Init按进程准备、配置读取、模块初始化、启动线程的顺序展开。停止或故障后App_Close统一回收并返回退出码，正常为0、故障为1。

```text
输入：ALSA → 格式转换 → Rockchip 3A → Snowboy → WebRTC VAD
应用：读取处理帧 → 语句/pre-roll → START / PCM / END
回复：WSS事件 → 播放环 → 格式转换/音量 → ALSA
界面：应用快照 → UI线程 → LVGL；触摸动作沿反方向交付应用
```

`src/debug.cpp`注册终端日志回调，业务通过`debug::log`交付事件。硬件和第三方细节集中在`src/platform/rv1106`；只有wake.cpp使用Snowboy需要的旧C++ ABI。

代码排版采用四空格缩进、换行花括号。对外使用一级模块namespace，内部函数和变量用static限定在实现文件；类型别名使用typedef。

## 冻结版语音行为

设备配置完成后，输入任务持续执行采集、转换、3A、唤醒和VAD。采集初始化同时设置并回读Mode1、左右ADC模拟增益23和高通On；这些值保存在板级配置中，使每次启动复现已验收的输入条件。

WebRTC VAD使用模式3，区分当前帧语音命中、语音延续和静音。普通监听使用语音活动确认开口；播放期间只累计当前帧命中，连续达到300ms才确认插话。句尾仍保留延续，连续静音700ms结束输入。500ms前滚补齐句首，确认后直接交付实时帧。

插话确认后取消旧播放并开始新轮次；END之后仍可取消，DONE之后等待声卡尾播完成再进入追问。采样格式和声学使用条件见[板级接口约束](../docs/hardware/README.md)。

## 编译依赖

先按[third_party说明](../third_party/README.md)克隆并准备依赖。CMake默认从本项目third_party读取OpenSSL、Snowboy、OpenBLAS、VAD、Boost、LVGL和Rockchip库。

幸狐SDK提供GCC8.3/uClibc工具链和匹配的Buildroot sysroot；ALSA、FreeType、libswresample及libavutil由该sysroot提供。只需设置BOOMPI_RV1106_SDK_ROOT，不需要逐项填写临时构建目录。

```sh
export BOOMPI_RV1106_SDK_ROOT=/path/to/luckfox-pico
cmake --preset rv1106-release
cmake --build --preset rv1106-release --parallel
DESTDIR="$PWD/build/rootfs" cmake --install build/rv1106-release
```

模型与字体由课程资源包预置到 `/userdata/boompi/models` 和 `/userdata/boompi/fonts`。默认模型对应Snowboy克隆目录的 `resources/common.res`、`resources/models/snowboy.umdl`。字体使用 `NotoSansCJK-Regular.ttc`，或基础镜像内的simsun字体。

rootfs安装目录包含程序与两份3A动态库；模型、字体及其余运行库仍需按上述清单准备，不能把安装目录当成完整系统镜像。

安装目录还包含匹配SDK的libaec_bf_process.so和librkaudio_common.so，安装到/usr/lib供系统加载器查找。其余运行库由BSP提供，运行不需要设置LD_LIBRARY_PATH。SDK库只在许可范围内用于本机构建/部署，不提交Git。

## 板端运行

```sh
/userdata/boompi/bin/boompi-client
```

程序在前台运行，日志直接输出到终端。Ctrl+C或发送SIGTERM会结束主循环并回收线程、算法和设备；程序不自行fork到后台。

终端可观察3A初始化结果、服务端握手、唤醒命中、进入监听、VAD当前帧命中/语音延续/静音变化、START/END入队、首包回答音频、DONE和声卡尾播完成。VAD持续检测输入，首次有效结果及每次状态变化各打印一条；日志顺序随实际输入和线程调度出现。START/END表示交付到发送队列，DONE表示服务端结束发送，声卡播完有独立日志。

这些消息由 `debug.cpp` 的注册回调写入标准错误输出。Windows CMD查看中文日志前执行 `chcp 65001`，再连接板端查看日志。

首次启动会创建`/userdata/boompi/config/client.conf`并持久化设备UUID。程序在系统已联网的子网上发现服务端，把地址和公钥指纹保存到同一文件；后续发现只接受相同指纹，发现超时后尝试已保存地址。教师也可预置以下字段完成配对，地址和pin必须成对：

```ini
device_id=<自动生成的UUID>
server_ip=<电脑IPv4>
server_port=17806
server_spki_sha256=<电脑稳定SPKI>
```

文件是普通key=value配置，不执行Shell、不读取BOOMPI_*运行环境变量。已有配置无效时明确失败，不覆盖或重新生成身份。音量仍由界面保存到ui.settings。

升级时，程序会把已有`server.conf`中的配对地址及指纹迁入`client.conf`，保存成功后移除旧文件。旧配对记录无效时报告配置错误，维护者确认后处理，保持原有信任身份。

## 系统联网前提（教师准备）

2026-09-18起的课堂手动初始化版镜像由学生在串口中按课准备网口、麦克及所需模块，完成联网后再启动客户端。该版已移除额外DHCP/麦克开机脚本和旧IPC自启动入口。下面的系统服务配置用于需要开机自动联网的部署环境。

DHCP、Wi-Fi和路由由基础镜像的系统服务准备。客户端使用系统路由建立WSS连接，并在各UP广播接口所在子网发送发现请求。

先检查最终镜像中的系统联网服务。2026-09-17核对的本板基础镜像已有`S41eth0dhcp`，不能只根据`interfaces`中仅有lo判断缺少联网配置。教师应确认实际服务与路由；若所用镜像通过`S40network`的`ifup -a`联网且没有其他DHCP服务，可保留原条目并为课堂有线接口增加：

```text
auto eth0
iface eth0 inet dhcp
```

板子连接到提供DHCP的课堂网络或电脑直连网段后，确认`ip -4 addr show eth0`已有地址，`ip -4 route`有通往服务端的路由，再启动客户端。Wi-Fi作为可选系统接入方式，由镜像启动supplicant并配置地址与路由；凭据放在权限0600的`/etc/wpa_supplicant.conf`中。

应用断线后重新发现并连接；网卡切换策略由系统配置。课堂按一台配套服务端对应一块板子准备独立网络或预配对，避免首次发现其他组服务端。学生只在配套服务端填写Key。

## 开机与升级

手动运行不依赖任何启动脚本。需要开机运行时，由BSP的BusyBox init直接启动，例如在系统`/etc/inittab`配置：

```text
::once:/userdata/boompi/bin/boompi-client
```

该入口在系统初始化完成后启动一次，日志进入系统控制台。它不负责应用自动重试或升级。升级前发送SIGTERM并等旧进程退出，保留旧二进制与配置，再安装新程序；不要覆盖正在运行的可执行文件。
