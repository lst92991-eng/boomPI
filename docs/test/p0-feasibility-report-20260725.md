# P0 可行性报告（2026-07-25）

> 后续更新：2026-07-27 已完成更精确的
> [vendor 音频只读盘点](p0-vendor-audio-inventory-20260727.md)。本文中的“Mode1 四通道”
> 是当时的组合假设；当前 DTB 的 TRCM Mode1 只表示 TX/RX 共享 TX 时钟，不能证明
> capture 四槽或数字 reference。本文保留为历史证据，不据此宣称板端能力。

## 结论

P0 当前为“部分通过”，不能标记完成。匹配 BSP 的 C++ 交叉构建、Rockchip 3A
二进制 ABI、Snowboy 加 OpenBLAS 的干净链接和 TLS 链接已经验证；真实板端执行、
48 kHz 全双工、Mode1 四通道、3A/Snowboy 实时处理、Wi-Fi AP/STA 能力和 UI
刷新路径仍需真机补证。

本轮只读取 SDK、sysroot、既有第三方依赖和主机网络状态。没有打开 PCM、录音、
播放、扫描 Wi-Fi、修改 mixer、刷镜像、改设备树、重启或写入板卡。

## 状态矩阵

| 闸门 | 状态 | 证据或缺口 |
| --- | --- | --- |
| C++ 工具链与 sysroot | 通过 | GCC 8.3.0 Buildroot wrapper 成功构建两个 RV1106 ELF |
| 目标 ELF ABI | 通过 | ELF32 ARM EABI5、hard-float、uClibc loader、无 RPATH/RUNPATH |
| 最小程序真机执行 | 阻塞 | 直连物理链路存在，但本轮 SSH 管理通道无响应 |
| Rockchip 3A ABI | 候选通过 | 匹配 ARMv7/uClibc；API、资源和哈希已核对 |
| Rockchip 3A 功能/实时率 | 未验证 | 需要真实双麦、参考通道和 16 kHz 连续输入 |
| Snowboy ABI/链接 | 候选通过 | 原始静态库加交叉编译 OpenBLAS 可生成干净 RV1106 ELF |
| Snowboy 模型加载/实时率 | 未验证 | 需要板端模型加载、连续 16 kHz 和 CPU/RSS 数据 |
| ALSA/Mode1 | 未验证 | 历史单项结果不能替代当前镜像的全双工四通道测试 |
| TLS ABI | 候选通过 | OpenSSL 3.5.7 LTS 已在目标工具链静态构建并链接干净 ELF |
| WSS 握手/SPKI | 未验证 | 仍需板端证书校验、SPKI 固定、重连和 half-open 测试 |
| Wi-Fi 配网 | 候选 | 镜像含 `hostapd`、`wpa_supplicant` 和 `iw`；驱动模式仍待 `iw list` |
| UI/触摸 | 候选 | 历史 framebuffer/GT911 单项通过；像素格式、旋转和刷新路径待重测 |

“候选”只表示可以进入下一项 HIL，不表示功能或性能已经通过。

## 工具链与目标 ABI

SDK 基线 commit 为 `994243753789`。SDK 工作树存在用户已有改动，本轮没有修改。
构建使用 Buildroot `output/host` 下的 wrapper，配置契约为：

```text
BOOMPI_RV1106_TOOLCHAIN_PREFIX=arm-rockchip830-linux-uclibcgnueabihf
BOOMPI_RV1106_TOOLCHAIN_ROOT=<BUILDROOT_OUTPUT>/host
BOOMPI_RV1106_SYSROOT=<BUILDROOT_OUTPUT>/staging
```

已确认环境：

- GCC 8.3.0、crosstool-NG 1.24.0。
- ARMv7-A/Cortex-A7、32-bit little-endian、EABI5、hard-float、VFPv4/NEON。
- uClibc-ng 1.0.31，解释器 `/lib/ld-uClibc.so.0`。
- libstdc++ 6.0.25，最高导出 `GLIBCXX_3.4.25`。
- sysroot 含 ALSA 1.2.8、OpenSSL 1.1.1v、pthread、fbdev、input 和 spidev
  的开发文件。

`cmake --preset rv1106-release` 和对应 build preset 已成功生成
`boompi-client` 与 `boompi-supervisor`。两者均为 PIE、ARM EABI5 hard-float、
动态依赖 uClibc/libstdc++，没有开发机 RPATH。未 strip 的链接产物符号信息中仍有
sysroot 启动对象的绝对路径；使用目标工具链 `strip --strip-unneeded` 生成部署副本后，
ELF 校验器确认路径已清除，且最高 `GLIBCXX_3.4.19` 低于目标上限。后续发布流程
必须校验部署副本，不能把未 strip 的构建树文件直接打包。由于板卡 SSH 不可达，
本轮没有把产物复制到板卡，也没有执行。

## Rockchip 3A

应接入 `media/common_algorithm/out` 中与 uClibc 匹配的版本，不使用 SDK 内另一个
依赖 `libc.so.6` 且含开发机 RPATH 的旧 `libRKAP_3A.so`。

核心 API 基线：

```c
void *rkaudio_preprocess_init(
    int rate, int bits, int src_chan, int ref_chan, RKAUDIOParam *param);
int rkaudio_preprocess_short(
    void *handle, short *input, short *output, int input_size,
    int *wakeup_status);
void rkaudio_preprocess_destory(void *handle);
```

已确认只按 16-bit PCM 接入；完整 3A 的文档范围为 8/16 kHz。头文件默认
`NUM_REF_CHANNEL=1`，但这不能证明板上 Mode1 的四通道顺序。`libaec_bf_process.so`
是 ARMv7 EABI5、NEON/VFPv4、hard-float、uClibc，动态依赖为
`librkaudio_common.so`、`libgcc_s.so.1` 和 `libc.so.0`。

固定 SHA-256：

| 文件 | SHA-256 |
| --- | --- |
| `rkaudio_preprocess.h` | `b9bbf723d8e5bfdc421cf45fdf5853fec1584737d0e20682cd0db6bae5a7b54d` |
| `libaec_bf_process.so` | `5abbcf518ffa39900dd78352547ebf5feab83d2f9b30a82c1e2dc1dc44b25e07` |
| `librkaudio_common.so` | `4f4c9d78028a592174c3e959e35d231317d0ed2a864a1ed230f8adab42960246` |
| `librkaudio_detect.so` | `f84b66a2d1d561fbb3c36e288a57f1e9ef50990974d9be9accbb0aaebcbae396` |
| `config_aivqe.json` | `1d160fde184935cf43a49feae7be0dfd24efdc82ff9de2ea8b35aba6318074f9` |

当前 assembled OEM/rootfs 能看到 3A 库，但没有看到 `config_aivqe.json` 和相关
模型资源。若后续选择文件配置模式，打包规则必须显式安装并校验这些资源。

## Snowboy

基线使用 [Kitt-AI/snowboy commit
`c9ff036e2ef3`](https://github.com/Kitt-AI/snowboy/commit/c9ff036e2ef3f9c422a3b8c9a01361dbad7a9bd4)。
仓库许可证文本将源码、运行库、资源和默认 `snowboy.umdl` 置于 Apache-2.0；其他
模型有各自许可，不能由默认模型的结论外推。

固定 SHA-256：

| 文件 | SHA-256 |
| --- | --- |
| `snowboy-detect.h` | `f203e88bccd3782b9fdfaa5f02ea2fab402671f415c9eae3609b67c1e622a363` |
| `libsnowboy-detect.a` | `346db1193490a9cc404d49fcfb22ca612cd3a0e649c4863f411553eb1c4f9f1f` |
| `common.res` | `5dd5258678182f2e055fa7a6167eba50ded3bf8b41f70faab11fd9b221de488b` |
| `snowboy.umdl` | `7ccc61effbe05c27d8fd3428bf27e71578d2eddcc97ac9c1437fa0f9cacc64f1` |
| OpenBLAS `libopenblas.a` | `fabfc588e0e0d94f3655d4ad5515e0c90fd161f016be5261e2f11d3df77a3e9d` |

RPi 静态库为 ARMv6/VFPv2 hard-float，可在 Cortex-A7 上作为 ABI 候选。它使用旧
libstdc++ 字符串 ABI，接入 target 必须隔离
`_GLIBCXX_USE_CXX11_ABI=0`，不能把该定义扩散到整个应用。库还需要
`cblas_saxpy`、`cblas_sdot`、`cblas_sgemm`、`cblas_sgemv`、`cblas_sger`、
`cblas_snrm2` 和 `cblas_sscal`。

使用 OpenBLAS commit `1bd74ad3d1e8d21f86d1a6be35abfcdf27c0208a` 的 ARMv7
静态库完成了最小链接。strip 后产物为 uClibc hard-float PIE，没有
RPATH/RUNPATH 或开发机绝对路径，最高需要 `GLIBCXX_3.4.20`，低于目标 rootfs
的 `GLIBCXX_3.4.25`。这只证明链接可行，不证明 `common.res`/模型能在板端加载，
也不证明唤醒准确率或实时率。上述 OpenBLAS archive 哈希只固定本次链接候选；
它仍需按单线程配置重建、重新固定哈希并做板端最坏耗时验证后才能成为发布输入。
当前 CMake 闸门因此只允许显式 opt-in 的 Debug feasibility probe，并拒绝 Release 配置。

Snowboy、OpenBLAS 和模型二进制仍放在仓库外，通过显式 CMake cache 路径接入；
在依赖来源、哈希和许可检查完成前不提交二进制。

## TLS 与网络安全

使用目标 sysroot 的 OpenSSL 1.1.1v 已成功链接最小 TLS client ELF，依赖
`libssl.so.1.1` 和 `libcrypto.so.1.1`。但
[OpenSSL 官方已在 2023-09-11 结束 1.1.1 的公开支持](https://openssl-library.org/post/2023-06-15-1.1.1-eol-reminder/)，
因此它只用于证明 BSP ABI 可链接，不能作为 boomPI 发布版的 WSS 安全基线。

进一步使用官方 OpenSSL 3.5.7 LTS 源码完成了仓库外的 RV1106 静态交叉构建：

```text
source: openssl-3.5.7.tar.gz
source SHA-256: a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8
target: linux-armv4
options: no-shared no-tests no-docs no-module no-comp no-weak-ssl-ciphers
```

生成的 `libssl.a` 约 1.4 MiB、`libcrypto.a` 约 7.4 MiB；使用稳定的 `/usr`
安装前缀、仅在构建环境 `PATH` 中提供交叉编译器，并 strip 后，最小 TLS client
约 4.0 MiB。该 ELF 为 ARM EABI5 hard-float/uClibc，无
RPATH/RUNPATH，不再依赖系统 `libssl.so.1.1`，动态依赖仅剩目标 rootfs 已有的
`libatomic` 和 uClibc，也没有嵌入开发机绝对路径。
[OpenSSL 官方发布页](https://www.openssl-library.org/source/)将 3.5 标为 LTS，
支持期到 2030-04-08。

P0 推荐以锁定的 3.5.x 外部构建作为 WSS 候选，不回退到 1.1.1v。板端 TLS
初始化、证书链、主机名校验、SPKI 固定、WebSocket 握手和内存占用仍未验证，
不能通过关闭证书校验绕过。

## 板卡连接状态

本轮检查时，Windows 直连网口为 `Up/100 Mbps`，DHCP 日志在十余分钟前仍有板卡
租约活动，但 SSH 和调试端口均超时。结论只能是“物理链路及近期 DHCP 有证据，
当前管理通道不可用”，不能据此断言板卡断电或当前镜像正常。

为避免泄露环境标识，本报告不保存 IP、MAC、SSID、主机名、私钥路径或完整本地
目录。历史板端输出仅作为定位线索，不用于勾选当前镜像的 P0 闸门。

## 只读探针

恢复 SSH 后，从仓库根目录执行：

```powershell
Get-Content -Raw scripts/probes/rv1106_p0_probe.sh |
  ssh <board-host> "sh -s -- --rockchip-3a-lib /oem/usr/lib/libaec_bf_process.so"
```

输出是 JSON，只包含白名单枚举、版本和计数，不输出网络标识或文件路径。探针不会
打开 PCM、修改 mixer、扫描 Wi-Fi 或写入系统。JSON 中的 `candidate` 仍需后续
功能测试确认。

交叉构建后应对 strip 的部署副本运行 ABI 校验器：

```text
python3 scripts/probes/verify_rv1106_elf.py \
  --readelf <TARGET_READELF> \
  --max-glibcxx GLIBCXX_3.4.25 \
  <DEPLOY_ELF>
```

校验器拒绝错误架构/float ABI/loader、`libc.so.6`、超上限 GLIBCXX、
RPATH/RUNPATH 和开发机绝对路径，并且不会在 JSON 中回显输入路径。本轮的
boomPI 客户端、Snowboy 最小链接产物和 OpenSSL 3.5.7 TLS 最小产物的 strip
部署副本均已通过。

## 解除 P0 阻塞的顺序

1. 恢复板端 SSH，运行只读探针并保存脱敏 JSON。
2. 将当前交叉编译的 ABI smoke 只复制到板端 `/tmp`，核对 loader 后执行。
3. 记录 ALSA 实际能力，再验证真正同时运行的 48 kHz capture/playback。
4. 用可辨识信号确认四通道顺序、双麦极性和数字参考采样位置。
5. 分别验证 Rockchip 3A 与 Snowboy 的初始化、错误路径和短时实时率；功能通过前
   不做长时间压力测试。
6. 核对 Wi-Fi 驱动 AP/STA 模式、UI backend 和受支持 TLS 方案。
