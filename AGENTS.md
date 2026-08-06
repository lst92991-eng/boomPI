# boomPI 开发约定

本仓库是 RV1106 语音 AI 教学项目。目标是让学生能顺着真实数据流读懂、构建和调试完整产品，而不是展示企业级框架。

## 1. 当前产品边界

已实现并需要保持：

- 双麦、单参考 Rockchip 3A；
- Snowboy 唤醒、WebRTC VAD、500 ms pre-roll；
- 持久 WSS、Qwen ASR/对话/流式 TTS；
- 播放中打断旧回复并提交新问题，三秒追问；
- LVGL 320×240 桌面、静态小智表情、字幕、触摸和音量；
- 以太网优先、Wi-Fi 配网和 SC3336 本地预览。

YOLO、在线音乐、长期记忆、账号系统、OTA 和多模态上传暂不实现，也不为它们预留空接口。

## 2. 简化原则

1. 优先调用已经验证的 ALSA、Rockchip 3A、Snowboy、WebRTC VAD、WebSocket++、LVGL 和 Qwen。
2. 抽象必须对应真实硬件、协议或所有权边界。禁止为未来需求增加 manager、adapter、committer、通用 worker 或消息总线。
3. 一个状态只能有一个 owner。实时线程只做固定成本工作，跨线程队列必须有容量和明确的满队列语义。
4. 教学可读性优先：顺序代码、小函数和数据流注释优于模板技巧、宏分支和过度防御。
5. 不静默丢 PCM、掩盖 sequence hole、无限重试或无限排队。超时、有界缓冲、TLS 和硬件恢复仍需保留。
6. 参数和行为只根据可复现日志修改；不能用主观听感臆测 AEC/VAD 参数。

## 3. 目录职责

```text
client/src/application/      对话状态机与 turn/epoch 编排
client/src/audio/            有界采集和播放队列
client/src/config/           板端环境配置
client/src/network/          网络选择、发现和持久 WSS
client/src/platform/rv1106/  ALSA、重采样、3A、Snowboy、VAD
client/src/ui/               LVGL、触摸和摄像头预览
server/internal/app/         服务组合与单设备会话
server/internal/backend/     Qwen pipeline 和唯一 provider 接口
server/internal/protocol/    v1 控制帧与 PCM
protocol/                    跨语言线协议
```

这些是职责目录，不是 App/Driver/Inf 模板。不要增加第二套状态机、协议实现、音频队列或可切换产品 backend。

## 4. 音频事实

```text
capture:  48 kHz / S16_LE / 4 ch / 20 ms
layout:   [mic0,mic1,refL,refR]
3A input: 16 kHz [mic0,mic1,refL]
3A output:16 kHz mono
TTS:      24 kHz mono → 48 kHz stereo playback
```

TTS 左右声道相同，所以 `refR` 不进入 Rockchip 3A。四通道原始采集仍保留给硬件诊断。vendor AGC 关闭，避免与硬件/数字增益叠加。

默认参数：

```text
Snowboy sensitivity = 0.7
VAD admission       = -30 dBFS
barge-in            = -25 dBFS
playback volume     = 60%
pre-roll            = 500 ms
```

打断成功的定义是：确认近讲、停止旧 TTS、发送 cancel、隔离旧 epoch，并把保留的近讲 PCM 作为新 turn 提交。仅停止扬声器不算成功。

## 5. 线程所有权

| 上下文 | 唯一职责 |
| --- | --- |
| application actor | 会话状态、turn/epoch、唤醒/VAD/打断 |
| capture，`SCHED_FIFO 40` | ALSA capture、3A、Snowboy/VAD、发布 20 ms 帧 |
| playback，`SCHED_FIFO 30` | TTS ring、重采样、ALSA write/drop/drain |
| WSS | TLS/WebSocket I/O 和协议事件 |
| UI worker | 所有 LVGL、触摸和音量配置写入 |
| camera worker | SC3336 拉流和固定大小帧交接 |

UI、网络和文件写入不得进入 ALSA 热路径。`SCHED_FIFO` 失败时记录 warning 并继续，不能盲目提高 UI 或网络优先级。

## 6. 服务端与安全

- 服务端是无数据库、`CGO_ENABLED=0` 的单个 Go 程序。
- 第一次运行提示输入中国内地 DashScope Key，生成 `config.yaml` 和稳定 TLS 身份后直接启动。
- 普通学生只配置 Key；模型等高级参数才进入 YAML。
- WSS 使用自动 TLS 身份和客户端 SPKI 保存/校验；hello 使用代码内固定教学口令。
- 该模型只适用于可信课堂局域网。不要增加账号、证书后台或配对数据库，也不要把端口暴露到公网。
- API Key 只在服务端配置中存在，不进入源码、日志、测试 fixture 或板端。
- 默认不保存 PCM、Wi-Fi 密码和完整对话。

## 7. UI 与网络

- LVGL 8.2，320×240；ST7789P3 SPI 目标 80 MHz，GT911 触摸。
- 小智页面只用静态表情表达状态，不恢复动态语音球。
- application 只发布小型 UI 状态；UI worker 独占 LVGL 对象。
- 音量实时值只更新原子 gain，提交时才由 UI worker 持久化。
- 点击小智与唤醒词进入同一状态机；离开摄像头页必须释放预览资源。
- 以太网优先、Wi-Fi 备用；SSID 和密码不得进入普通日志。

## 8. 构建

Host：

```sh
cmake --preset host-debug
cmake --build --preset host-debug --parallel
ctest --preset host-debug
python3 scripts/verify_protocol_fixtures.py
python3 -m unittest discover -s scripts/tests -p 'test_*.py' -v
cd server && go test ./... && go vet ./...
```

RV1106：

```sh
cmake --preset rv1106-release
cmake --build --preset rv1106-release --parallel
```

工具链、sysroot 和外部库只通过 `BOOMPI_*` 环境变量或 Git 忽略的 `CMakeUserPresets.json` 注入。不要搜索相邻 SDK，不要提交个人绝对路径、模型、vendor 二进制、日志、core、生成配置或 build 目录。

Snowboy bridge 单独使用旧 C++ ABI；不得把 `_GLIBCXX_USE_CXX11_ABI=0` 扩散到整个客户端。

## 9. 代码和体量

- C++17 与 `gofmt`；数值名带单位，例如 `*_ms`、`*_frames`、`*_dbfs`。
- 注释解释硬件事实、并发所有权、时序和“为什么”，不逐行翻译代码。
- 错误必须指出阶段且不输出 secret；返回值不能混合背压、断线和协议错误。
- 头文件只暴露必要边界，不跨层 include 私有 vendor 头。
- 不用压缩排版、合并语句或生成代码伪造低行数。
- 客户端产品胶水以约 2500 ELOC 为方向，LVGL 资源、测试和第三方库单独统计。增加代码时先说明它解决的真实问题。

## 10. 完成标准

1. Host 协议、CMake/CTest、Python 和 Go 测试通过。
2. 修改过的板端 C++ 用匹配 GCC/uClibc 工具链严格交叉构建。
3. 检查 ELF 架构、loader、依赖与 RPATH。
4. 只有体验或硬件行为需要验证时才部署；部署后默认保持客户端关闭。
5. 人工音频回归覆盖唤醒、句首、长回复、播放中打断并提交、追问、安静时不自激和断网恢复。
6. 汇报必须区分代码实现、离线验证、交叉构建、部署和人工验收。

保留用户已有修改，不重写 Git 历史，不删除 `v1.0.0` tag。刷镜像、改设备树、分区或覆盖板端状态前必须确认目标板和恢复路径。
