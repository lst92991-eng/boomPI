# boomPI 板端客户端

`boompi-client` 是 RV1106 上唯一的产品进程。它直接调用已经验证的 ALSA、Rockchip 3A、Snowboy、WebRTC VAD、WebSocket++ 和 LVGL，不提供未使用的通用 backend 或插件框架。

## 数据流

```text
ALSA 48 kHz [mic0,mic1,refL,refR]
  → 16 kHz [mic0,mic1,refL]
  → Rockchip AEC/BF/ANR
  → Snowboy / VAD / 对话状态机
  → WSS 16 kHz mono

WSS 24 kHz mono
  → 1.5 s 有界 TTS ring
  → 48 kHz stereo ALSA playback
```

采集和播放分别由 `SCHED_FIFO 40`、`SCHED_FIFO 30` 的线程独占；设置失败会记录 warning 并继续。application actor 只处理会话状态，UI worker 只处理 LVGL/触摸，摄像头 worker 只处理 SC3336。

小智页使用五个静态表情表示空闲、聆听、思考、说话和错误状态。它保留字幕、触摸唤醒/打断和音量交互，但不再包含动态语音球。

## 配置

安装后的配置为 `/userdata/boompi/config/client.env`，每行使用 `BOOMPI_NAME=value`，不要添加 `export`、引号或行尾注释。控制脚本会自动生成设备 UUID。

```text
BOOMPI_DEVICE_ID=<自动生成>
BOOMPI_VOLUME_PERCENT=60
BOOMPI_SNOWBOY_SENSITIVITY=0.7
BOOMPI_VAD_MIN_DBFS=-30
BOOMPI_BARGE_MIN_DBFS=-25
BOOMPI_CAPTURE_LEFT_POLARITY=1
BOOMPI_CAPTURE_RIGHT_POLARITY=1
```

VAD 门限用于常规说话，barge-in 门限用于播放期 AEC 后的近讲确认，两者不能合并。服务端地址和 SPKI 可留空，由 UDP 发现并在首次连接时保存。固定教学口令由客户端内部提供，不需要学生配置。

当前 BSP 固定使用 `hw:0,0`、`/userdata/boompi/models/common.res`、
`/userdata/boompi/models/snowboy.umdl`、100% 板级增益和始终开启的 barge-in。
旧配置文件中对应的环境变量会被忽略。需要固定服务端时，同时配置
`BOOMPI_SERVER_IP`、`BOOMPI_SERVER_SPKI_SHA256`，端口可用
`BOOMPI_SERVER_PORT` 覆盖默认值 `17806`。

## 交叉构建

先设置 `CMakePresets.json` 列出的外部依赖路径，再从仓库根目录执行：

```sh
cmake --preset rv1106-release
cmake --build --preset rv1106-release --parallel
```

工具链、sysroot、Rockchip 3A、Snowboy、WebRTC VAD、OpenSSL、Boost、LVGL 和 BSP 库必须与目标镜像 ABI 一致。外部二进制、模型和个人绝对路径不进入仓库。

## 安装与运行

Snowboy 的 `common.res`、模型文件属于外部资产，放在 `/userdata/boompi/models/`。

```sh
DESTDIR=/tmp/boompi-rootfs cmake --install build/rv1106-release
boompi-clientctl update /tmp/boompi-client
boompi-clientctl start
boompi-clientctl log
```

管理入口：

```text
boompi-clientctl start|stop|restart|status|log|update|provision
```

Wi-Fi 首次配网以 root 运行 `boompi-clientctl provision`。客户端按“以太网优先、Wi-Fi 备用”选择网络；SSID 和密码不写入普通日志。

协议和取消语义见 [protocol-v1.md](../protocol/protocol-v1.md)，依赖边界见 [audio-backends.md](../docs/architecture/audio-backends.md)。
