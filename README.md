# boomPI

RV1106自研板上的语音AI教学项目：板端C++17客户端，学生电脑运行Go服务端，API Key只留在电脑。

当前非音频候选为`codex/minimal-non-audio`，基于音频验收版本`2ca0cf6`。保留双麦3A、Snowboy、VAD、流式问答、播放中插话、三秒追问、字幕/静态表情/触摸/音量与SC3336预览。界面只有语音、摄像头两页，Wi-Fi直接配置SSID/PSK，不再有桌面、时钟或扫码配网。

## 使用

```text
麦克风/扬声器/屏幕/摄像头
           ↓
   RV1106 boompi-client
           ↕ 局域网WSS
   电脑 boompi-server
           ↕
   DashScope北京区服务
```

一个学生、一台电脑和一块板子。共享Wi-Fi课堂中，教师应给板端预置对应电脑的地址与SPKI，避免首次UDP发现连到邻组；优先每组独立热点或网线直连。

电脑首次双击`boompi-server.exe`配置DashScope Key，之后复用config.yaml和稳定TLS身份。详见[服务端说明](server/README.md)。

板端：

```sh
boompi-clientctl start
boompi-clientctl status
boompi-clientctl log
boompi-clientctl stop
```

以太网优先、Wi-Fi备用。直接编辑`/etc/wpa_supplicant.conf`，权限0600；真实密码不提交Git。**升级之前先用旧clientctl stop清理旧客户端及AP服务，再安装新版本。** 配置和SDK说明见[客户端README](client/README.md)。

## 源码主线

```text
采集：ALSA 48k四槽 → 16k双麦/参考 → 3A → wake/VAD → speech
上行：START → 前滚和实时PCM → END
下行：WSS PCM → 采样环 → 48k双声道ALSA → drain → 追问
显示：UiView → ui::show → page::show → LVGL
网络：网卡准备 → 端点发现/缓存 → 绑定所选接口 → TLS/WSS
```

音频源码本次未改，设备格式、内部profile及vendor256点块适配仍以2ca0cf6为准。非音频实现不再使用DeviceUi/LvglScreen的PImpl转发层。

## 教学与验证

先看[源码阅读顺序](docs/teaching/client-source-reading.md)和[分关实验](docs/teaching/README.md)。当前交付范围、接口和限制见[非音频记录](docs/test/minimal-non-audio.md)，音频见[紧凑插话](docs/test/compact-barge-in.md)。历史设计记录不代表当前接口。

```sh
cmake --preset host-debug
cmake --build --preset host-debug --parallel
ctest --preset host-debug
python3 scripts/verify_protocol_fixtures.py
python3 -m unittest discover -s scripts/tests -p 'test_*.py' -v
python3 scripts/measure_teaching.py
cd server && go test ./... && go vet ./...
```

完整UI测试另需LVGL8.2、SDL2、FreeType及`BOOMPI_BUILD_UI_SIMULATOR=ON`；teaching-ui工作流运行实际页面、Linux端口、netns和ASan/UBSan。Host成功不表示真板已经通过。

交叉构建：

```sh
cmake --preset rv1106-release
cmake --build --preset rv1106-release --parallel
```

SDK/sysroot/库路径从BOOMPI_*或忽略的CMakeUserPresets.json注入，不提交个人路径和vendor二进制。

## 简单安全边界

首次发现使用可信课堂内TOFU，UDP不是认证；随后TLS严格校验已保存SPKI。固定hello口令不是账号系统，不对公网开放WSS。API Key、Wi-Fi密码、config.yaml、state、模型与私有SDK均不入Git，默认不记录原始PCM和完整对话。

当前线协议只有[BPV4](protocol/protocol-v4.md)，无旧协议双栈。Go服务端本轮保持不变；客户端发布前仍须匹配SDK构建、检查ARM ELF及上板验收。
