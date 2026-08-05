# Third-party dependencies

本目录只纳入已经审查的 WebSocket++ 0.8.2 头文件子集及其通知文件。模型、SDK dump、工具链和第三方二进制库保留在仓库外；产品实际使用且许可清晰的小型资源（例如 Tabler 图标字体子集）可以随源码保存。

增加或升级依赖时记录版本或 commit、上游地址、许可证、RV1106 ABI 和是否允许再分发。没有结论时只能作为本机外部输入，不能提交。

| 依赖 | 固定版本/来源 | 许可与仓库策略 |
| --- | --- | --- |
| [WebSocket++](https://github.com/zaphoyd/websocketpp) | 0.8.2；当前仓库内为所需头文件子集 | BSD 3-Clause；保留 `websocketpp/NOTICE.debian` 和 `websocketpp/README.boompi.md` |
| [Kitt-AI Snowboy](https://github.com/Kitt-AI/snowboy/commit/c9ff036e2ef3f9c422a3b8c9a01361dbad7a9bd4) | commit `c9ff036e2ef3`; RPi archive SHA-256 `346db1193490a9cc404d49fcfb22ca612cd3a0e649c4863f411553eb1c4f9f1f` | 仓库许可证适用于其代码、库、资源和默认 `snowboy.umdl`；其他模型需单独检查。runtime/model 保持外部输入 |
| OpenBLAS | commit `1bd74ad3d1e8d21f86d1a6be35abfcdf27c0208a` | BSD 3-Clause；只作为 Snowboy bridge 的外部静态库 |
| [OpenSSL 3.5.7](https://github.com/openssl/openssl/releases/tag/openssl-3.5.7) | 3.5.7 | Apache-2.0；RV1106 package 保持外部输入，CMake 只校验目录与 package 版本 |
| Rockchip 3A | 与目标 BSP `media/common_algorithm/out` 匹配 | Vendor SDK 条款；再分发未确认，头文件、配置和库不得提交 |
| WebRTC VAD | 与目标镜像 ABI 匹配的外部头文件和静态库 | 上游许可随实际来源记录；仓库只保存接入代码 |
| LVGL 8.2 / FreeType / Boost | 与 CMake 入口匹配的外部源码或 sysroot 依赖 | 遵循各自上游许可；大型源码、完整字体和构建产物不进入本目录 |

完整的 CMake 输入、ABI 隔离和运行时边界见 [音频后端与依赖契约](../docs/architecture/audio-backends.md)。秘密、下载缓存、构建产物和许可证不明的资产不得进入源码树。
