# boomPI 板端客户端

板端程序负责双麦采集、Rockchip 3A、Snowboy、VAD、WSS 流式对话、AEC 打断，
以及 ST7789P3/GT911 的表情、字幕和触摸控制。唯一板端产品可执行文件是 `boompi-client`；
LVGL 桌面模拟器和 HIL 探针都不进入板端安装包。

`v1.0.0` 已在真实 RV1106 板完成语音闭环、连续播放、同句打断、触摸、SC3336 本地预览和
实时音量滑块人工验收。完整冻结参数与证据见 [发布说明](../docs/releases/v1.0.0.md)。

音频 ELOC 由 `scripts/tests/test_client_source_contract.py` 按固定文件集合计算；LVGL UI、网络启动和
测试代码单独统计。2026-08-03 的 2929 ELOC 是当日候选快照，不代表后续工作树。源码冻结后必须
重新运行契约脚本、严格交叉构建和板端 HIL，不能沿用旧哈希或旧验收结论。

## 配置

安装后的配置文件是 `/userdata/boompi/config/client.env`，每行使用
`BOOMPI_NAME=value`，不要加 `export`、引号或行尾注释。首次启动时
`boompi-clientctl` 会生成唯一 device UUID 和下列板级默认值：

```text
BOOMPI_DEVICE_ID=<板端自动生成的 UUID>
BOOMPI_CAPTURE_PCM=hw:0,0
BOOMPI_PLAYBACK_PCM=hw:0,0
BOOMPI_SNOWBOY_RESOURCE_FILE=/userdata/boompi/models/common.res
BOOMPI_SNOWBOY_MODEL_FILE=/userdata/boompi/models/snowboy.umdl
BOOMPI_SNOWBOY_SENSITIVITY=0.7
BOOMPI_VAD_MIN_DBFS=-35
BOOMPI_BARGE_MIN_DBFS=-25
```

`BOOMPI_VAD_MIN_DBFS` 约束原始麦克风启动门限；`BOOMPI_BARGE_MIN_DBFS` 只约束播放期
Rockchip AEC 后的近讲语音。两者分开，避免扬声器声场仅凭 raw mic 电平反复触发静音探针。

服务端地址和 SPKI 可以留空，由 UDP 发现并首次保存；端口默认 `17806`。教学版客户端与
服务端具有相同的默认共享令牌，只适用于可信局域网。正式部署时，在两端设置相同的随机
`BOOMPI_DEVICE_TOKEN`。

## 安装与运行

Snowboy 的 `common.res`/`snowboy.umdl` 和 CJK 字体是授权外部资产，不重复提交到仓库。
制作镜像时分别放入 `/userdata/boompi/models/` 和 `/userdata/boompi/fonts/`。兼容旧镜像的
`/userdata/boompi/qwen-test/` 模型路径也会自动识别。

CMake 安装规则使用板端绝对目录；生成 rootfs staging 时使用 `DESTDIR`：

```sh
DESTDIR=/tmp/boompi-rootfs cmake --install build/rv1106-candidate
```

它会安装程序、配网工具和 `/etc/init.d/S99boompi-client`。对已运行的板子做手工更新时：

```sh
install -m 755 boompi-clientctl boompi-provision /usr/sbin/
mkdir -p /usr/lib/boompi /userdata/boompi/config
install -m 755 boompi-provision.py /usr/lib/boompi/
boompi-clientctl update /tmp/boompi-client
boompi-clientctl start
boompi-clientctl log
```

`boompi-clientctl` 只有 `start|stop|restart|status|log|update|provision` 七个动作。异常退出后
总计最多启动三次；手工更新只保留一个 `boompi-client.bak`，不实现签名更新或 A/B OTA。

## Wi-Fi 二维码配网

以 root 运行：

```sh
boompi-clientctl provision
```

屏幕显示热点二维码。手机扫描后连接 `boomPI-Setup`，密码 `boompi-setup`；系统通常会自动
打开配网页，未弹出时访问 `http://192.168.4.1/`。提交 2.4 GHz Wi-Fi 后，临时热点关闭，
客户端按“以太网优先、Wi-Fi 备用”的顺序重新启动。无以太网且从未保存 Wi-Fi 时，
开机脚本会自动进入该配网流程。SSID/密码只通过标准输入交给客户端，不会写入普通日志。

当前 CJK 字体由部署流程放在
`/userdata/boompi/fonts/DroidSansFallbackFull.ttf`，不把大型字体二进制提交到仓库。
