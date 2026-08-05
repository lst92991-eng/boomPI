# boomPI

boomPI 是一个面向 RV1106 自研板卡的语音 AI 教学项目。板端运行 C++17 客户端，学生电脑运行 Go 服务端；API Key 只保存在电脑上，板子通过局域网连接服务端。

当前版本保留已经实测的体验：双麦 AEC、Snowboy 唤醒、VAD、流式问答、连续 TTS、播放中打断并提交新问题、三秒追问、LVGL 触摸桌面、音量、Wi-Fi 配网和 SC3336 本地预览。清洗后的目标不是增加框架，而是让学生能沿着真实数据流读懂代码。

## 使用方式

推荐一个学生、一台电脑、一个服务端 EXE 和一块板子。40～80 人的班级各自运行自己的服务端，不需要统一账号系统或数据库。课堂内优先让板子与电脑网线直连或使用每组独立热点；若所有设备共用同一 Wi-Fi，则应预先给板端写入对应电脑的服务端地址和 SPKI，避免 UDP 自动发现连到邻组。

```text
双麦 / 扬声器 / 屏幕 / 触摸 / 摄像头
                    │
          RV1106 boompi-client
                    │  局域网 WSS
                    ▼
             boompi-server
                    │
                    ▼
        Qwen 中国内地（北京）区
```

### 1. 启动服务端

从 DashScope 中国内地站点取得 API Key，然后双击 `boompi-server.exe`。第一次运行会提示粘贴 Key，保存 `config.yaml`、生成本机 TLS 身份，并直接启动；以后双击即可。

```powershell
.\boompi-server.exe
```

服务端只保留实际使用的 `ASR → Qwen → 流式 TTS` 链路。高级模型参数可写入 `config.yaml`，普通教学使用只需要 Key。详见 [server/README.md](server/README.md)。

### 2. 启动板端

```sh
boompi-clientctl start
boompi-clientctl status
boompi-clientctl log
boompi-clientctl stop
```

板端优先使用以太网，Wi-Fi 作为备用；首次 Wi-Fi 配网运行 `boompi-clientctl provision`。详见 [client/README.md](client/README.md)。

## 真实音频数据流

```text
ALSA 48 kHz / 4 ch [mic0,mic1,refL,refR]
  → 重采样到 16 kHz
  → Rockchip 3A [mic0,mic1,refL]
  → Snowboy + WebRTC VAD
  → application 状态机
  → WSS 上传 16 kHz mono

WSS 下发 24 kHz mono
  → 有界 TTS 环形缓冲
  → 48 kHz stereo ALSA playback
```

四通道采集仍完整保留，但 TTS 左右声道相同，因此 Rockchip AEC 只接收一个参考通道。采集和播放线程独占 ALSA；网络、状态机和 LVGL 不进入实时音频线程。默认参数保持为：

```text
Snowboy sensitivity = 0.7
VAD admission       = -35 dBFS
barge-in            = -25 dBFS
playback volume     = 60%
```

## 目录

```text
client/                 RV1106 客户端、LVGL 和板端脚本
server/                 跨平台 Go 服务端
protocol/               客户端与服务端共用的 v1 线协议
docs/architecture/      当前系统与音频数据流
docs/hardware/          硬件事实和 BSP 边界
docs/test/              可复现验证入口
third_party/            第三方源码与许可
```

客户端只按真实职责拆分为 `application / audio / config / network / platform / ui`。这些目录不是企业分层模板：每层都对应一个可观察的数据或硬件边界。

## 构建与离线验证

Host 测试不打开真实板端设备：

```sh
cmake --preset host-debug
cmake --build --preset host-debug --parallel
ctest --preset host-debug
python3 scripts/verify_protocol_fixtures.py
python3 -m unittest discover -s scripts/tests -p 'test_*.py' -v
cd server && go test ./... && go vet ./...
```

板端只有一个交叉构建入口。私有 SDK、sysroot 和库路径通过 `BOOMPI_*` 环境变量或 Git 忽略的 `CMakeUserPresets.json` 注入：

```sh
cmake --preset rv1106-release
cmake --build --preset rv1106-release --parallel
```

## 简单安全边界

- 服务端自动生成并复用 TLS 身份；客户端首次收到 UDP 发现回复时按 TOFU 保存 SPKI，随后在 TLS 握手中校验并拒绝身份变化。UDP 发现本身不是认证。
- hello 保留固定教学口令，用于避免非 boomPI 客户端误接入；学生不需要配置它。
- 没有账号、数据库、证书后台或复杂配对，适合可信课堂局域网。
- 不要把 WSS 端口暴露到公网；不要提交 `config.yaml`、`state/`、API Key、Wi-Fi 密码、模型或私有 SDK。
- 默认不保存原始 PCM 和完整对话。

协议以 [protocol-v1.md](protocol/protocol-v1.md) 为准，开发规则见 [AGENTS.md](AGENTS.md)，架构总览见 [system-overview.md](docs/architecture/system-overview.md)。
