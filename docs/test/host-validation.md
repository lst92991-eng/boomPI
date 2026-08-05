# boomPI 验证入口

Host、交叉构建和真板体验分别证明不同问题，不能互相替代。

## 1. Host 回归

在仓库根目录执行：

```sh
python3 scripts/verify_protocol_fixtures.py
python3 -m unittest discover -s scripts/tests -p 'test_*.py' -v
cmake --preset host-debug
cmake --build --preset host-debug --parallel
ctest --preset host-debug
```

这些测试覆盖线协议、客户端状态机、音频有界队列、rebuffer、取消乱序和有界退出，不打开
真实 ALSA、触摸、摄像头或 vendor 设备。

## 2. Go 服务端

```sh
cd server
go test ./...
go vet ./...
go build -trimpath -o boompi-server ./cmd/boompi-server
```

默认测试不调用付费 Qwen，也不打印 API Key 或 TLS 私钥。

## 3. RV1106 交叉构建

设置 [外部依赖](../architecture/audio-backends.md)列出的 `BOOMPI_*` 路径，然后执行：

```sh
cmake --preset rv1106-release
cmake --build --preset rv1106-release --target boompi_client --parallel
python3 scripts/probes/verify_rv1106_elf.py \
  build/rv1106-release/client/boompi-client \
  --readelf "$READELF" --max-glibcxx 3.4.25
```

ELF 应为 ARM EABI5 hard-float、使用 `/lib/ld-uClibc.so.0`、没有 RPATH/RUNPATH 或开发机绝对
路径。交叉构建通过不代表真板声学通过。

## 4. 板端只读检查

先确认目标板 SSH host key，再运行：

```powershell
ssh rv1106-board-3 "uname -a; cat /proc/asound/cards; cat /proc/asound/pcm"
ssh rv1106-board-3 "sha256sum /userdata/boompi/bin/boompi-client; boompi-clientctl status"
ssh rv1106-board-3 "ps -eLo pid,tid,cls,rtprio,ni,comm,args | grep boompi"
ssh rv1106-board-3 "tail -F /userdata/boompi/log/client.log"
```

源码写有 `SCHED_FIFO 40/30` 不代表内核已经授予，以 `ps` 的实际 `CLS/RTPRIO` 为准。

## 5. 可选 AEC HIL

```sh
cmake --preset rv1106-release \
  -DBOOMPI_BUILD_AEC_LOOP_HIL=ON \
  -DBOOMPI_AEC_LOOP_DELAY_SAMPLES=0
cmake --build --preset rv1106-release --target boompi_aec_loop_hil --parallel
```

停止客户端后，把 `boompi-aec-loop-hil` 和 24 kHz mono S16_LE fixture 放到板端执行。输出中的
`reference_channels` 固定为 1。该探针能验证真实 Mode1/3A 数据进入代码，但不能代替真人
double-talk 和最终壳体声学测试。

底层声卡、屏幕、触摸和摄像头 BSP 测试放在相邻 `board_support/hardware-tests`，不在应用仓库
复制第二套脚本。

## 6. 人工体验回归

固定板子、镜像、音量和参数，依次验证：

1. 唤醒后句首完整，500 ms pre-roll 生效；
2. 长回复连续播放，无周期性静音；
3. 播放中讲话，旧 TTS 停止且新问题被提交；
4. 三秒追问可用；
5. 人保持安静时扬声器不自激、不产生新 turn；
6. 音量、静态表情、Wi-Fi 页面和 SC3336 预览正常；
7. 断网恢复后会话可重建。

同时记录 XRUN、capture discontinuity、queue overrun、core、TTS rebuffer、CPU/RSS、线程
优先级和连接状态。没有运行本轮真人/仪器测试时只能写“未验证”。
