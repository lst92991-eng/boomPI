# boomPI 客户端：最小教学候选版

从`apps/boompi_client/main.cpp`开始：读取配置→App_Init→循环App_Process→App_Close。只保留--voice-loop和--check-config。当前实现见[源码索引](../docs/teaching/client-source-reading.md)与[交付记录](../docs/test/minimal-non-audio.md)。

## 一条主线

```text
输入线程：ALSA → 格式转换 → 3A → wake → VAD → 交接
应用：voice_input::read → speech::update → voice_net::start/send/end
回复：voice_net::poll → playback::write/finish → Drained → 追问
显示：ui::show → UI线程 → page::show；ui::poll_action取触摸动作
相机：进入页面open → 完整帧read到page::pixels → 离页close
```

音频冻结于2ca0cf6，包含候选/有界复核；没有改成VAD立即取消。500ms前滚、三秒追问、generation/sequence、DONE与真实尾播区分均保留。

UI为两个固定容器，不再重建桌面或管理AP进程。初始化阶段建立LVGL端口和页面，再启动UI线程；join后回收。帧/显示快照保持必要线程边界，不引入通用消息框架。

## 运行和Wi-Fi

```sh
boompi-clientctl start
boompi-clientctl status
boompi-clientctl log
boompi-clientctl stop
```

直接编辑板端`/etc/wpa_supplicant.conf`：

```conf
ctrl_interface=/var/run/wpa_supplicant
network={
    ssid="YOUR_SSID"
    psk="YOUR_PASSWORD"
}
```

```sh
chmod 600 /etc/wpa_supplicant.conf
```

真实凭据只留在板端，不提交Git或放入命令行。编辑已运行的supplicant配置后，由维护者重载该服务或重启板子；客户端复用现有服务，不每次重连重启它。

默认先以太网再Wi-Fi。有线取得IP但WSS未READY时，下轮先试Wi-Fi；正常Wi-Fi连接不被有线主动抢占。UDP发现与WSS都绑定所选接口。网卡、驱动和服务路径须符合本板BSP，Host dummy网卡测试不证明实际射频/DHCP通过。

`/userdata/boompi/config/client.env`首次只生成设备UUID。教师需要固定某组服务器时，预置以下成对字段：

```text
BOOMPI_DEVICE_ID=<安装时生成的UUID>
BOOMPI_SERVER_IP=<电脑IPv4>
BOOMPI_SERVER_PORT=17806
BOOMPI_SERVER_SPKI_SHA256=<电脑稳定SPKI>
```

不指定地址/pin则走发现与缓存；首次发现不是认证，后续TLS检查已保存公钥。音量从ui.settings读取，释放滑块时保存；学生没有声学校准环境变量。

## 构建与检查

```sh
cmake --preset host-debug
cmake --build --preset host-debug --parallel
ctest --preset host-debug
python3 scripts/verify_protocol_fixtures.py
python3 -m unittest discover -s scripts/tests -p 'test_*.py' -v
python3 scripts/measure_teaching.py
```

完整Linux检查要求OpenSSL、Boost、cJSON、ALSA、FFmpeg开发依赖，并设置BOOMPI_REQUIRE_HOST_TRANSPORT_TEST=ON。UI另需LVGL8.2、SDL2和FreeType：

```sh
cmake --preset host-debug -DBOOMPI_BUILD_UI_SIMULATOR=ON \
  -DBOOMPI_LVGL_ROOT="$BOOMPI_LVGL_ROOT" -DBOOMPI_REQUIRE_HOST_TRANSPORT_TEST=ON
cmake --build --preset host-debug --parallel
ctest --preset host-debug
```

发布使用教师准备的匹配SDK：

```sh
export BOOMPI_RV1106_SDK_ROOT=/absolute/path/to/teaching-sdk
sh scripts/build_teaching_release.sh
```

根目录可提供boompi-sdk.cmake映射既有BOOMPI_*路径。匹配组件包括GCC/uClibc工具链与sysroot、Rockchip3A、Snowboy/OpenBLAS、WebRTC VAD、Boost、OpenSSL3.5.7和LVGL8.2。wake.cpp单独旧C++ ABI。脚本检查ELF后生成安装目录，不连接开发板；没有SDK时不能把Host产物当板端程序。

## 升级与验收

覆盖客户端及启动脚本**之前**，先用旧版本`boompi-clientctl stop`停止旧客户端和旧AP服务，确认退出后再安装本分支。不要直接用新脚本清理正在运行的旧AP：新版本已删除该管理功能。

保留旧可执行文件、Wi-Fi配置、server.conf以及服务端config.yaml/state。新clientctl仍支持有限重启和更新失败回滚，不删除已有身份。本分支不需要修改设备树、镜像或Go服务端。

先测试2ca0cf6音频，再单独测试此分支的页面、触摸、音量、摄像头进退、有线/无线回退及断网恢复。当前Host测试、交叉构建和真板验收状态必须分开报告。
