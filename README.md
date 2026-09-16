# boomPI

RV1106 Linux小智客户端：唤醒、声学处理、语句检测、流式问答、插话、追问、屏幕和触摸音量。配套服务端作为课程黑箱使用，学生只配置Qwen/DashScope API Key。

## 学习与开发顺序

1. 按[第三方依赖说明](third_party/README.md)，先把固定版本克隆到 `third_party/`，阅读真实头文件和API。
2. 配置匹配的幸狐SDK，按[客户端说明](client/README.md)编译。
3. 从 `client/apps/boompi_client/main.cpp` 开始，阅读应用、音频任务和板级调用。
4. 启动教师提供的服务端，再直接运行板端 `boompi-client`。

参考ESP32项目时只看正式App/Inf/Driver业务源码，不使用其test/text示例作为实现依据。

## 目录

- `client/`：板端C/C++、小智界面资源和CMake；只有一个程序入口。
- `third_party/`：上游源码克隆、SDK专有库及各库自己的ARM构建产物。
- `protocol/`：客户端使用的BPV4业务协议。
- `server/`：配套服务端源码与维护说明，不属于学生开发要求。
- `scripts/`：维护者的发布构建和ARM ELF检查。
- `docs/hardware/`：板级格式、接口和待实测约束。

## 客户端构建

第三方依赖准备好后，在Linux虚拟机中设置SDK根目录：

```sh
export BOOMPI_RV1106_SDK_ROOT=/path/to/luckfox-pico
cmake --preset rv1106-release
cmake --build --preset rv1106-release --parallel 4
```

## 配套服务端与发布

学生运行教师提供的 `boompi-server.exe`，首次输入Key，后续自动读取配置。详见[服务端使用说明](server/README.md)。

维护者准备客户端、Windows服务端和rootfs安装目录时运行：

```sh
sh scripts/build_release.sh
```

这是编译打包脚本；小智业务由可执行程序直接启动。维护者打包服务端需要Go 1.26。Key、设备配置、TLS私钥和本机构建产物不提交Git。
