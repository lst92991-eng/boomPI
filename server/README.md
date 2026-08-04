# boomPI 教学版服务端

每个平台只需要一个 `boompi-server` 可执行文件。程序会自行创建本地 TLS 身份和配置，
不需要安装数据库、容器或后台服务。

`v1.0.0` Release 提供 Windows amd64、Linux amd64、macOS amd64 和 macOS arm64 单文件产物；
默认使用 Qwen 中国内地（北京）区。完整架构、校验和与验收边界见
[v1.0.0 发布说明](../docs/releases/v1.0.0.md)。

## 三步启动

1. 第一次运行程序，让它生成 `config.yaml`：

   ```powershell
   # Windows
   .\boompi-server.exe
   ```

   ```bash
   # Linux / macOS
   chmod +x ./boompi-server
   ./boompi-server
   ```

2. 打开第一步输出路径中的 `config.yaml`，只替换这一项并保存：

   ```yaml
   qwen_api_key: "sk-你的中国内地（北京）区-Qwen-API-Key"
   ```

3. 再运行一次。看到 `boomPI server starting` 表示初始化完成、WSS 与 UDP 监听正在启动；若端口绑定失败，程序会紧接着报错退出；
   按 `Ctrl+C` 安全退出。

`config.yaml` 带有与教学版板端一致的默认设备令牌，正常教学使用无需修改。它只适合可信
局域网；正式部署应在服务端和板端设置相同的随机 `BOOMPI_DEVICE_TOKEN`。该环境变量优先于文件。API Key 也可用
`DASHSCOPE_API_KEY` 覆盖文件值：

```powershell
$env:DASHSCOPE_API_KEY = "sk-..."       # Windows PowerShell
.\boompi-server.exe
```

```bash
export DASHSCOPE_API_KEY="sk-..."       # Linux / macOS
./boompi-server
```

默认使用中国内地（北京）区公共 DashScope 端点，只需要对应区域的 API Key。需要连接新加坡区时，
把 `region` 显式设置为 `singapore`。已有专属 Workspace 的用户可选设置
`DASHSCOPE_WORKSPACE_ID`；ASR 和对话请求会使用同区域的 Workspace 端点，TTS 则继续使用同区域
公共端点并通过请求头携带 Workspace ID。`--check-config` 只显示配置来源和非敏感摘要，不会输出
API Key、Workspace ID 或设备令牌。

从旧的新加坡区部署迁移时，应同时完成三件事：把 `region` 改为 `china-beijing`，换用中国内地
（北京）区的 API Key，并确认可选的 `DASHSCOPE_WORKSPACE_ID` 也属于该区域。环境变量
`BOOMPI_REGION` 会覆盖 YAML；旧环境若设置过 `BOOMPI_REGION=singapore`，需要删除或同步改为
`china-beijing`。暂不迁移的部署可继续显式使用 `singapore`，但 Key、Workspace 和 region 必须属于
同一区域。

## UDP 发现

服务端默认监听 UDP `17807`。局域网客户端发送精确文本：

```text
BOOMPI_DISCOVER_V1
```

服务端返回一行固定格式文本：

```text
BOOMPI_SERVER_V1 17806 <SPKI-SHA256-Base64>
```

三个字段依次是协议标识、WSS 端口和证书 SPKI SHA-256 的 Base64 值。发现包不包含 API Key、
设备令牌或证书私钥，也不承担配对功能。

## 从源码构建

在本目录执行：

```text
go test ./...
go vet ./...
go build -trimpath -o boompi-server ./cmd/boompi-server
```

Windows 输出文件名可改为 `boompi-server.exe`。项目保持 `CGO_ENABLED=0` 可构建；Windows、
Linux 和 macOS 各自生成一个独立的单文件可执行程序。`config.yaml` 和运行后创建的 `state/`
属于本机私密状态，不应提交到 Git 或发送给他人。

## 常用选项

```text
boompi-server --check-config
boompi-server --config path/to/config.yaml
boompi-server --help
```

除 API Key 外均有教学版默认值。完整可调项目见 `configs/config.example.yaml`；通常无需复制
或修改它。

首响应等待 15 秒时的本地状态提示属于板端状态机职责：现有 v1 协议没有独立的等待提示事件，
服务端不能在不改变协议语义的情况下代替板端显示。因此服务端只提供 30 秒
`first_response_timeout`；15 秒提示必须由客户端本地计时实现。
