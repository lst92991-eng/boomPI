# P0 Rockchip MPI 音频交叉链接验证记录（2026-07-27）

- 采集时间：2026-07-27 21:12:34 +08:00（Asia/Shanghai，以 host 时钟为准）。
- 结果：**交叉链接与动态符号级通过；板端功能仍未验证**。
- 范围：匹配 BSP 的 GCC 8.3/uClibc 工具链、固定 SHA-256 的八个 MPI/类型头文件、
  `librockit.so`、`librockchip_mpp.so.0`（SONAME 为 `librockchip_mpp.so.1`）与
  `librga.so`，以及 tests-off 默认 ALL 的 `boompi_rockchip_mpi_audio_link_check`。
- 证据边界：没有运行或安装该 ELF，没有打开 PCM、调用 vendor API、修改 mixer、录音或
  播放。本记录不证明 AI/AO 能同时运行、真实通道布局、数字 reference、VQE/3A 效果或
  实时率。

## 固定输入与路径边界

CMake 只接受显式绝对路径并逐项核对固定 SHA-256，不会搜索相邻 SDK 或下载依赖。八个
ABI 相关头文件来自匹配 BSP 的 MPI SDK include 目录：

```text
rk_mpi_ai.h
rk_mpi_ao.h
rk_comm_aio.h
rk_mpi_sys.h
rk_mpi_mb.h
rk_comm_mb.h
rk_common.h
rk_type.h
```

各头文件的固定哈希见 [P0 vendor 音频证据基线的固定输入摘要](p0-vendor-audio-inventory-20260727.md#固定输入摘要)。

库 pin 为：

| 输入 | SHA-256 | 说明 |
| --- | --- | --- |
| `librockit.so` | `3f92f8c41ffe9ad72e407b68750906fcff89ea06758f14a3fc2a3d87061e3d0f` | `SONAME=librockit.so` |
| `media/out/lib/librockchip_mpp.so.0` | `e8183339fff1dd466adc9567be5c4c98239c567157eaebefe4a2fe50f793fec8` | 未 strip 链接候选；`SONAME=librockchip_mpp.so.1` |
| `media/out/lib/librga.so` | `13cf7d10210cdf43a998a07a9bf0033821dfec61b31d9c50195848c0480010c7` | 未 strip 链接候选；`SONAME=librga.so` |

MPP/RGA 的 CMake pin 精确对应 `media/out/lib` 链接候选。当前 OEM 中经过 strip 的
`librockchip_mpp.so.0` 和 `librga.so` 哈希分别为
`b0d0b5256dc5643d68a07fb18df324fd68161cdfb8c15fd1e4847d730de280cb` 与
`a8456f37c8cfb679e7c1f60f5f7e888d54f0a6d83fbbef0a7ca6d4c06c720227`；即使其动态 ABI
与 SONAME 相同，也不能替代上述 CMake 输入，否则固定哈希会按设计拒绝。板端部署还必须
保留运行时名称 `librockchip_mpp.so.1`；链接通过不负责安装或创建该软链接。

## 配置与命令

私有 BSP、工具链和构建目录的绝对路径不写入仓库。等价的脱敏命令如下：

```text
cmake --preset rv1106-debug \
  -DBOOMPI_BUILD_TESTS=OFF \
  -DBOOMPI_ENABLE_ROCKCHIP_MPI_AUDIO=ON \
  -DBOOMPI_ALLOW_FEASIBILITY_AUDIO_VENDOR_INPUTS=ON \
  -DBOOMPI_ROCKCHIP_MPI_INCLUDE_DIR=<matching-bsp>/media/rockit/rockit/mpi/sdk/include \
  -DBOOMPI_ROCKCHIP_MPI_ROCKIT_LIBRARY=<matching-bsp>/media/out/lib/librockit.so \
  -DBOOMPI_ROCKCHIP_MPI_MPP_LIBRARY=<matching-bsp>/media/out/lib/librockchip_mpp.so.0 \
  -DBOOMPI_ROCKCHIP_MPI_RGA_LIBRARY=<matching-bsp>/media/out/lib/librga.so
cmake --build --preset rv1106-debug --parallel

<matching-cross-readelf> -h <build>/boompi_rockchip_mpi_audio_link_check
<matching-cross-readelf> -l <build>/boompi_rockchip_mpi_audio_link_check
<matching-cross-readelf> -d <build>/boompi_rockchip_mpi_audio_link_check
<matching-cross-readelf> -Ws <build>/boompi_rockchip_mpi_audio_link_check
```

Feasibility 闸门先核对 Linux/ARM 交叉编译、固定 RV1106 GNU compiler、uClibc sysroot、
Debug-only 配置和全部固定输入。Release 配置已确认会被拒绝。同一个 clean
`rv1106-debug`/tests-off 构建同时启用 MPI 与 Rockchip 3A 两套 pinned inputs 时，默认
ALL 构建也成功，并同时生成两个 link-check executable；这只证明两套 CMake 候选可共存。

## 链接与 ELF 结果

- `BOOMPI_BUILD_TESTS=OFF` 时默认 ALL 构建成功，证明该检查不依赖 CTest。
- 最终文件为 ELF32 PIE、ARM EABI5、hard-float，解释器为 `/lib/ld-uClibc.so.0`。
- 动态段没有 `RPATH` 或 `RUNPATH`，并保留以下 `NEEDED`：

```text
librockit.so
librockchip_mpp.so.1
librga.so
libstdc++.so.6
libgcc_s.so.1
libc.so.0
```

- 动态符号表保留 21 个 raw full-duplex 生命周期 `UND`：

```text
RK_MPI_SYS_Init
RK_MPI_SYS_Exit
RK_MPI_SYS_CreateMB
RK_MPI_MB_Handle2VirAddr
RK_MPI_MB_ReleaseMB

RK_MPI_AI_SetPubAttr
RK_MPI_AI_Enable
RK_MPI_AI_SetChnParam
RK_MPI_AI_EnableChn
RK_MPI_AI_GetFrame
RK_MPI_AI_ReleaseFrame
RK_MPI_AI_DisableChn
RK_MPI_AI_Disable

RK_MPI_AO_SetPubAttr
RK_MPI_AO_Enable
RK_MPI_AO_SetChnParams
RK_MPI_AO_EnableChn
RK_MPI_AO_SendFrame
RK_MPI_AO_WaitEos
RK_MPI_AO_DisableChn
RK_MPI_AO_Disable
```

该集合只覆盖 CPU 侧 raw AI/AO 有限探针需要的 SYS/MB、capture 和 playback 生命周期。
`SYS_Bind/UnBind`、VQE、resample、AMIX/mixer、volume/mute/track、AENC/ADEC 和检测接口均
未纳入。`AO_WaitEos` 仅用于有限播放探针的有界 drain，不表示持续全双工依赖 EOS。

函数地址通过匹配头文件的精确类型检查并保留为真实未定义引用，避免 `--as-needed` 把
vendor 库变成空链接。target 使用私有 include，设置 `SKIP_BUILD_RPATH=TRUE`，没有
`add_test`、自定义运行命令或安装规则。对临时 `--strip-debug` 副本执行完整 RV1106 ELF
验证后，ABI、uClibc loader、依赖、无开发路径及无 RPATH/RUNPATH 检查全部通过；原始
Debug link-check 仍不作为发布产物。

## 回归结果

同一最终树在 clean 临时目录中通过：

- 16 个 CTest；
- 31 个 Python/script 测试；
- Go 1.26.5 的 `go test ./...` 与 `go vet ./...`。

MPI 专用 host fixture 还确认：默认关闭时不访问 vendor 路径；host 启用会在读取私有输入前
失败关闭；tests-off 时 target 属于默认 ALL 且无 RPATH/RUNPATH；合成库分别缺少代表性的
SYS、MB、AI 或 AO 符号时链接失败。合成库只用于 CMake 回归，不是 RV1106 ABI 或板端证据。

## 同轮板端只读探测

以 host 时间 2026-07-27 21:12:34 +08:00 记录的 schema v2 只读探针显示：

- 当前镜像可见一个 capture PCM 和一个 playback PCM，是后续全双工 HIL 的候选；
- `librockit.so`、预构建 AI/AO test 和直接 3A 库存在；
- VQE JSON 资源缺失。

探针没有打开 PCM、执行预构建 test 或调用任何 vendor 函数，也没有修改 mixer。板端系统
时钟错误地停留在 2021 年，因此本轮及后续记录以 host 时间为准。探测后开发板物理链路
断开，未继续 raw AI/AO 或直接 ALSA HIL。

## 本次没有证明

- 没有证明 `librockit.so` 能在当前板端进程中成功加载，或 OEM 运行时依赖闭包完整可用。
- 没有证明 capture/playback 参数协商、48 kHz 真全双工、有限 timeout、停止顺序或 xrun。
- 没有证明双麦/reference 的 slot、packing、极性、延迟、漂移或 reference tap。
- 没有证明 VQE、直接 3A、AEC/NS/BF/AGC、声音质量、CPU/RSS 或持续实时率。

因此这里只关闭“匹配头文件、`librockit` 及其 MPP/RGA 依赖能由目标工具链解析最小 raw
生命周期符号”这一项。恢复物理链路后，下一步仍是保存/恢复 mixer 的直接 ALSA 与 raw
MPI 48 kHz 有限全双工 HIL，而不是继续扩展生产 adapter 或开始压力测试。
