# P0 可行性报告（2026-07-25）

> 后续更新：2026-07-27 已完成更精确的
> [vendor 音频只读盘点](p0-vendor-audio-inventory-20260727.md)。本文中的“Mode1 四通道”
> 是当时的组合假设；当前 DTB 的 TRCM Mode1 只表示 TX/RX 共享 TX 时钟，不能证明
> capture 四槽或数字 reference。本文保留为历史证据，不据此宣称板端能力。

## 结论

P0 当前为“部分通过”，不能标记完成。匹配 BSP 的 C++ 交叉构建、Rockchip 3A
二进制 ABI 与离线零输入板端调用、Snowboy 加 OpenBLAS 的干净链接和 TLS 链接已经
验证；当前镜像的 48 kHz 短时静音全双工也已通过，Rockchip 3A 平台适配器的
跨帧节拍、故障清空、重新初始化和板端零输入路径也已通过。生产客户端 DSP worker
接线、可辨识播放/采集内容、Mode1 四通道、3A 声学效果与实时处理、可安全失败的
Snowboy 产品 runtime、Wi-Fi AP/STA 能力和 UI 刷新路径仍需真机补证。

2026-07-25 的初始检查只读取 SDK、sysroot、既有第三方依赖和主机网络状态，没有
打开 PCM。2026-07-27 的补充验证打开了 PCM，但只播放静音并丢弃采集样本；没有保存
录音、播放可辨识信号、扫描 Wi-Fi、修改 mixer、刷镜像、改设备树、重启或持久写入板卡。

2026-07-27 补充验证在同一 BSP 基线的 RV1106 板卡上执行了 Debug-only 离线 3A
探针，只向 `/tmp` 复制程序并以全零内存缓冲区调用库，没有打开 PCM 或修改板卡配置。
ABI 可行性记录见
[Rockchip 3A 离线 ABI 探针报告](rockchip-3a-offline-probe-20260727.md)，平台适配器
记录见 [Rockchip 3A 平台适配器板端探针报告](rockchip-3a-adapter-probe-20260727.md)。
同日的 ALSA 补充验证见
[RV1106 ALSA 全双工 smoke 记录](rv1106-alsa-smoke-20260727.md)。
Snowboy 默认模型的正向离线检测与致命错误路径见
[Snowboy P0 板端探针记录](snowboy-p0-probe-20260727.md)。

## 状态矩阵

| 闸门 | 状态 | 证据或缺口 |
| --- | --- | --- |
| C++ 工具链与 sysroot | 通过 | GCC 8.3.0 Buildroot wrapper 成功构建两个 RV1106 ELF |
| 目标 ELF ABI | 通过 | ELF32 ARM EABI5、hard-float、uClibc loader、无 RPATH/RUNPATH |
| 最小程序真机执行 | 部分通过 | 2026-07-27 离线 3A 探针和 ALSA 平台 smoke 执行成功；生产客户端 smoke 尚未执行 |
| Rockchip 3A ABI/平台适配器 | 通过（离线） | 固定头文件/库哈希、跨帧节拍单测、严格错误映射、交叉链接、ELF 和板端两代零输入调用均通过；尚未接入 DSP worker |
| Rockchip 3A 功能/实时率 | 未验证 | 需要真实双麦、参考通道、通道顺序和 16 kHz 连续输入 |
| Snowboy ABI/链接 | 候选通过 | 私有旧 ABI bridge、固定静态库与 OpenBLAS 可生成干净 RV1106 ELF，旧 ABI 未扩散 |
| Snowboy 模型/产品安全 | 阻塞 | 默认模型板端加载成功，47 帧 fixture 检测 1 次、最大 7.197 ms；缺失模型会直接 `terminate`/`Aborted`，不能接产品 runtime |
| ALSA 48 kHz 全双工 | 通过（短时静音） | 当前镜像上同时推进 4 通道 capture 与 2 通道 playback 50 个周期，失败 0；不证明可听播放或采集内容正确 |
| Mode1/通道与参考 | 未验证 | Mode1 保持 Disabled；四通道顺序、双麦极性、DAC reference 采样位置和 AEC 均未验证 |
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

头文件与反汇编共同确认只接受 16-bit PCM。官方资料列出多个采样率，但本项目只在
16 kHz 验证：每通道固定 16 ms，即 256 个样本；`input_size` 是所有源通道与参考
通道的 `int16_t` 样本总数，2 mic + 2 ref 时必须为 1024。处理成功返回单声道输出
字节数，16 kHz 时为 512；长度不匹配返回 0。导出的 ABI 没有 reset，当前唯一有
证据的复位方式是 destroy 后重新 init。头文件默认 `NUM_REF_CHANNEL=1`，官方资料的
`REF_POSITION`/可选重排说明也不能证明板上 Mode1 的四通道顺序。

`libaec_bf_process.so` 是 ARMv7 EABI5、NEON/VFPv4、hard-float、uClibc，动态依赖为
`librkaudio_common.so`、`libgcc_s.so.1` 和 `libc.so.0`。离线探针的 NEEDED 不含
`librkaudio_detect.so`。目标镜像缺少预期的 `config_aivqe.json`，但结构体参数路径
仍能以零输入完成 init/process/destroy/reinit；这不证明文件配置模式、实际通道排列、
AEC/BF 效果或实时性。

Rockchip 库固定 16 ms，而 `AudioDspEngine` 公共契约固定 20 ms。平台适配器通过
既有 `AudioDspFrameBridge16k` 将 80 ms 内的 4 个公共输入帧转换为 5 个厂商块，再还原
为 4 个公共输出帧；元数据绑定到最早缓冲输入，并在 Arm/Disarm/不连续/后端故障时
清空。适配器不修改冻结的 `AudioDspEngine`，厂商库只在显式 opt-in 的构建闸门内接入，
默认 Host/Release 构建不会加载它。主机单测与板端两代零输入探针已经验证节拍、样本守恒、
错误映射和重新初始化；生产 DSP worker 接线、物理通道顺序、AEC/BF 声学效果与实时率
仍未验证。

固定 SHA-256：

| 文件 | SHA-256 |
| --- | --- |
| `rkaudio_preprocess.h` | `b9bbf723d8e5bfdc421cf45fdf5853fec1584737d0e20682cd0db6bae5a7b54d` |
| `libaec_bf_process.so` | `5abbcf518ffa39900dd78352547ebf5feab83d2f9b30a82c1e2dc1dc44b25e07` |
| `librkaudio_common.so` | `4f4c9d78028a592174c3e959e35d231317d0ed2a864a1ed230f8adab42960246` |
| `librkaudio_detect.so` | `f84b66a2d1d561fbb3c36e288a57f1e9ef50990974d9be9accbb0aaebcbae396` |
| `config_aivqe.json` | `1d160fde184935cf43a49feae7be0dfd24efdc82ff9de2ea8b35aba6318074f9` |

当前目标镜像能看到 3A 库，但没有看到 `config_aivqe.json` 和相关模型资源。若后续
选择文件配置模式，打包规则必须显式安装并校验这些资源。

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
的 `GLIBCXX_3.4.25`。后续 Debug-only probe 已在板端加载固定 `common.res` 与默认模型：
50 个全零 20 ms 帧均映射为 diagnostic silence；47 个上游 `snowboy.wav` fixture 帧中
产生一次 keyword 1，最大单帧处理耗时 7.197 ms，最大 RSS 3,180 KiB，reset 成功。
这只证明短时正向离线路径，不证明真实麦克风准确率、误唤醒率或持续实时率。

缺失模型的受控错误路径会在旧静态库内部直接 `terminate`/`Aborted`，私有 C bridge
外层的 `catch (...)` 无法获得控制权。因此当前 feasibility adapter/probe 不接
`boompi-client`，Release capability 保持 unavailable。上述 OpenBLAS archive 哈希只
固定本次链接候选；
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

2026-07-25 检查时，Windows 直连网口为 `Up/100 Mbps`，DHCP 日志在十余分钟前仍有板卡
租约活动，但 SSH 和调试端口均超时。结论只能是“物理链路及近期 DHCP 有证据，
当前管理通道不可用”，不能据此断言板卡断电或当前镜像正常。

2026-07-27 管理通道已恢复，离线 3A 探针在 Buildroot 2023.02.6、内核 5.10.160、
ARMv7l 的目标板上退出 0。该结果只解除最小 3A ABI 调用的板端执行阻塞，不替代生产
客户端、实时率或声学验证。板卡时钟未同步，报告以主机时间为准。

同日的 ALSA 平台 smoke 在相同镜像上以显式 `hw:0,0` 参数同时推进 48 kHz、S16_LE、
4 通道 capture 与 2 通道 playback 50 个周期，失败 0。测试全程使用静音且不保存 PCM；
Mode1 保持 Disabled，因此该结果不能证明四通道内容、极性、DAC reference、可听播放
或 AEC。该 HIL 对应报告中的固定 smoke 二进制；当前主线 committer 迁移后尚未板端重跑。

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

1. 在已通过 Host 守恒测试的 16↔20 ms bridge 上补齐 Rockchip platform adapter；继续
   禁止用填充、丢弃或重复规避。
2. 在允许出声/采音并保存、恢复 mixer 原值的前提下，用可辨识信号确认 Mode1
   四通道顺序、双麦极性和数字参考采样位置。
3. 获取或重建错误可返回、单线程 OpenBLAS 的 Snowboy runtime；当前 archive 的缺失
   模型路径会终止进程，不能接入产品或进入长时间压力测试。
4. 执行生产客户端板端 smoke，并核对 loader、依赖和故障关闭行为。
5. 核对 Wi-Fi 驱动 AP/STA 模式、UI backend 和受支持 TLS 方案。
