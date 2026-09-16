# boomPI

RV1106 Linux语音客户端与配套Go服务端。客户端提供唤醒、声学处理、语句检测、流式对话、插话、追问、小智屏幕和触摸音量。

## 目录

- `client/`：板端C/C++程序、界面资源、CMake和启动脚本。
- `server/`：配套服务端，配置DashScope API Key后运行。
- `protocol/`：两端共用的BPV4业务协议说明。
- `scripts/`：发布构建和ARM ELF检查。
- `docs/hardware/`：代码依赖的板级接口约束。
- `third_party/`：WebSocket++头文件及许可信息，其余SDK依赖在项目外。

## 构建

客户端需要匹配的幸狐RV1106 SDK、GCC/uClibc工具链和厂商库，具体见[客户端说明](client/README.md)。

```sh
cmake --preset rv1106-release
cmake --build --preset rv1106-release --parallel
```

服务端使用Go 1.26：

```sh
cd server
go build -trimpath -o boompi-server ./cmd/boompi-server
```

发布客户端、Windows服务端及rootfs安装目录：

```sh
sh scripts/build_release.sh
```

服务端使用方式见[服务端说明](server/README.md)。API Key、Wi-Fi密码、TLS身份、模型和SDK二进制不提交Git。
