# boomPI 教学版服务端

`boompi-server` 是一个无数据库、无需容器的单文件程序。它接收板端 16 kHz PCM，依次调用中国内地 DashScope 的 ASR、Qwen 对话模型和流式 TTS，再把文本与 24 kHz PCM 发回板端。

课堂推荐每位学生在自己的电脑运行一个服务端并连接一块板子；40～80 人不共享同一账号数据库或中心服务。优先使用板子到电脑的直连网线或每组独立热点；共享 Wi-Fi 时预先固定该组的服务端地址和 SPKI。

## 第一次运行

先把 `boompi-server.exe` 放进一个普通可写目录，再双击运行；也可以在终端运行：

```powershell
.\boompi-server.exe
```

程序会提示粘贴中国内地 DashScope API Key，随后完成三件事：

1. 在 EXE 同目录生成只有 `qwen_api_key` 的 `config.yaml`；
2. 在 `state/` 自动生成并长期复用本机 TLS 身份；
3. 直接启动 WSS 服务和 UDP 发现，无需再次运行。

以后直接运行 EXE。也可以预先设置 `DASHSCOPE_API_KEY`，这样首次启动不询问输入。`DASHSCOPE_WORKSPACE_ID` 是可选的专属 Workspace 覆盖。

```powershell
$env:DASHSCOPE_API_KEY = "sk-..."
.\boompi-server.exe
```

默认配置路径始终是 EXE 旁边的 `config.yaml`，不受终端当前目录影响。只有调试多个配置时才使用：

```text
boompi-server --config path/to/config.yaml
boompi-server --check-config
boompi-server --help
```

`--check-config` 只显示非敏感摘要，不会打印 API Key。

## 固定数据流

```text
板端 16 kHz PCM
  → qwen3-asr-flash
  → qwen3.6-flash（reasoning_effort=none）
  → qwen3-tts-flash-realtime / Cherry
  → 24 kHz PCM + UTF-8 文本增量
```

当前教学版只保留这条实际使用的链路。`internal/backend.ConversationBackend` 是替换模型平台的唯一接口；设备协议、会话 actor 和 Qwen 实现不互相泄漏类型。模型、音色、提示词、超时等高级参数见 `configs/config.example.yaml`，最小配置仍然只需要 Key。

## 局域网连接与安全边界

- WSS 默认监听 IPv4 `0.0.0.0:17806`。
- UDP 发现固定监听 IPv4 `17807`，返回 WSS 端口和 TLS SPKI SHA-256。
- 服务端首次运行自动生成 TLS 身份；客户端保存并校验 SPKI。
- hello 仍校验与教学客户端一致的共享口令。

这套安全模型用于可信、隔离的课堂局域网：没有账号、数据库、证书后台或复杂配对。共享口令不是互联网身份系统，不应把 WSS 端口直接暴露到公网。`config.yaml`、`state/` 和 API Key 不得提交到 Git 或发送给他人。

## 从源码构建

在 `server/` 目录执行：

```sh
go test ./...
go vet ./...
go build -trimpath -o boompi-server ./cmd/boompi-server
```

项目保持 `CGO_ENABLED=0`。默认测试不调用付费 Qwen；真实 provider 测试必须显式启用。唯一线级格式是 [protocol-v2.md](../protocol/protocol-v2.md)，客户端和服务端须一起升级。

v2 的一条连接只有一个递增 generation。上行 START 创建输入，END 提交；播放中打断用新 generation 的 START|SUPERSEDE，无需等待取消 ACK。普通 START 保留已完成历史，SUPERSEDE 或 STOP 的 retract=true 撤回最后一段未听完的回答；STOP 的 generation 是新的退休栅栏。

WSS handler 直接读取并设置代际栅栏，然后投递到唯一的有界输入队列。固定一个 session worker 串行执行 provider 取消和新输入，取消最长 700 ms，PCM 排队最长 800 ms。队列、序号或期限违规会关闭连接，残缺输入不会被伪造为正常提交；连续打断不会创建无界 worker。下行按 20 ms 节奏发送，保留一帧以便真实末帧携带 END，纯文本回答不生成空音频。
