# P0 Rockchip 3A 固定帧 HIL 指南

本指南对应 `boompi_rockchip_3a_hil`。它只验证构建期 pinned Rockchip direct 3A 契约能否用板端
另行核验哈希的运行库完成一次固定帧调用，不是生产 DSP backend，也不打开 ALSA/MPI、读取录音
或写出处理后 PCM。
当前仓库没有该探针的板端执行结果；交叉构建成功也不得写成 3A 功能通过。

## 构建边界

`BOOMPI_BUILD_ROCKCHIP_3A_HIL` 默认 `OFF`。配置时必须同时显式开启 3A 依赖、Debug-only
feasibility 输入和 HIL target，并提供同一套匹配 BSP 的 pinned 文件：

```sh
cmake --preset rv1106-debug \
  -DBOOMPI_ENABLE_ROCKCHIP_3A=ON \
  -DBOOMPI_BUILD_ROCKCHIP_3A_HIL=ON \
  -DBOOMPI_ALLOW_FEASIBILITY_AUDIO_VENDOR_INPUTS=ON \
  -DBOOMPI_ROCKCHIP_3A_INCLUDE_DIR=<matching-bsp-3a-include> \
  -DBOOMPI_ROCKCHIP_3A_AEC_LIBRARY=<matching-bsp>/libaec_bf_process.so \
  -DBOOMPI_ROCKCHIP_3A_COMMON_LIBRARY=<matching-bsp>/librkaudio_common.so \
  -DBOOMPI_ROCKCHIP_3A_DETECT_LIBRARY=<matching-bsp>/librkaudio_detect.so \
  -DBOOMPI_ROCKCHIP_3A_CONFIG_FILE=<matching-bsp>/config_aivqe.json

cmake --build --preset rv1106-debug \
  --target boompi_rockchip_3a_hil --parallel
```

CMake 会校验 header、三个 shared object 和配置文件的固定 SHA-256，并把 pinset 与本探针源码
SHA-256 写入结果 provenance。该 target 是 `EXCLUDE_FROM_ALL`：默认构建不会构建它；它不安装、
不进入 CTest、没有 post-build 执行，也不进入发布包。只有显式指定 target 才会生成 ELF，只有人
工启动 ELF 才会运行探针。

HIL ELF 不直接链接 vendor `.so`，因此没有 `libaec_bf_process.so` 或
`librkaudio_common.so` 的 `DT_NEEDED`；已有 `boompi_rockchip_3a_link_check` 单独保留 pinned header/
library 的直接链接证据。按下述外层命令清理 loader override 后，无参数 dry-run 不主动加载
vendor binary；execute 只在双 opt-in、安全前置
检查和 core-dump 禁用成功后，从固定 `/oem/usr/lib/libaec_bf_process.so` 执行 `dlopen`，再解析
真实 link-check 已证明导出的 `rkaudio_preprocess_init`、`rkaudio_preprocess_short`、
`rkaudio_preprocess_destory` 三个固定 API 名称。参数初始化与释放 helper 是 pinned header 的
`static inline` 实现，不作为 `.so` 符号解析。探针没有运行时库路径参数，也拒绝
`LD_LIBRARY_PATH`、`LD_PRELOAD` 与
`LD_AUDIT` 残留后继续主动加载；构建 pinset 仍只证明构建输入，不能替代下面的板端文件哈希
核验。注意动态 loader 会在 `main()` 前处理有效的 `LD_PRELOAD`/`LD_AUDIT`，所以进程内检查
无法阻止已经发生的 constructor；下面外层 `env -u` 是唯一的 pre-exec 防线。

`config_aivqe.json` 是 pinned provenance 和参数审计来源；direct
`rkaudio_preprocess_*` 运行时不会读取或解析 JSON。探针把已核对的有限 profile 明文固化在源码
中，不能把“提供了 JSON 路径”误解为运行时加载配置。

## 固定单帧契约

- 输入为内存合成的低幅单帧，不接触 PCM 设备或文件。
- 格式固定为 `16 kHz / 16-bit / 256 samples per channel`，即 16 ms。
- `src_chan=2`、`ref_chan=1`，逻辑交织顺序固定为 `[mic0,mic1,reference]`。
- 输入总长为 `768 short`（1536 bytes）；传给 `rkaudio_preprocess_short` 的
  `input_size` 也是 768。
- BF 输出缓冲区为 256 个 `short`；成功契约是返回 512 output bytes。
- 只执行一次 load、参数准备、init、process、destroy、参数释放和 unload；process 执行后才把
  guard 标记为已评估，输入、输出两侧都有 guard 检查。
- 输出只包含状态、返回值、耗时和 guard；不打印、保存、hash 或上传输入/输出 PCM。
- `wakeup_status_raw` 仅原样记录，明确不是产品 VAD 结论。

单帧 `process_elapsed_us` 只是一次调用耗时，不代表连续帧最坏耗时、调度抖动、CPU/RSS 或实时率。

## Dry-run 与执行闸门

按下述命令清理环境后，无参数运行是默认 dry-run，不主动加载 vendor `.so`，也不调用任何
Rockchip 3A API：

```sh
ulimit -c 0
env \
  -u LD_LIBRARY_PATH -u LD_PRELOAD -u LD_AUDIT \
  -u PATH_TX_IN_MIC -u PATH_TX_IN_REF \
  -u PATH_TX_OUT_MDF -u PATH_TX_OUT_MDFFAST \
  -u PATH_RX_IN -u PATH_RX_OUT \
  timeout 10s <probe-path>/boompi_rockchip_3a_hil
```

这六个 `PATH_*` 名称来自 pinned SDK/library 中可见的调试路径字符串；现有证据没有证明库一定
通过 `getenv` 读取它们。执行前仍保守清理，且关闭 core dump，避免供应商调试机制或异常退出
落下音频/内存产物。三个 `LD_*` 变量必须同时为空，避免替换或插入运行库；不得清理系统的普通
`PATH`。如果目标 BusyBox 的 `env` 不支持 `-u`，应在同一个受控子 shell 中逐项 `unset` 后再
`exec` 探针，不能省略任何一项。探针自身仍检查变量是否残留并以退出码 `3` 拒绝，但该检查只
阻止后续主动 `dlopen`，不能倒退 pre-main loader 行为。

构建 pinset 不证明板端 `/oem` 中的实际字节。execute 前必须在目标镜像上只读核对：

```sh
sha256sum \
  /oem/usr/lib/libaec_bf_process.so \
  /oem/usr/lib/librkaudio_common.so \
  /oem/usr/lib/librkaudio_detect.so
```

预期依次为 `5abbcf518ffa39900dd78352547ebf5feab83d2f9b30a82c1e2dc1dc44b25e07`、
`4f4c9d78028a592174c3e959e35d231317d0ed2a864a1ed230f8adab42960246`、
`f84b66a2d1d561fbb3c36e288a57f1e9ef50990974d9be9accbb0aaebcbae396`。任一文件缺失或哈希
不一致都必须停止，不能仅凭 SONAME、路径或编译期 provenance 继续。

真实 vendor 调用需要同一命令同时提供两个 opt-in：

```sh
ulimit -c 0
env \
  -u LD_LIBRARY_PATH -u LD_PRELOAD -u LD_AUDIT \
  -u PATH_TX_IN_MIC -u PATH_TX_IN_REF \
  -u PATH_TX_OUT_MDF -u PATH_TX_OUT_MDFFAST \
  -u PATH_RX_IN -u PATH_RX_OUT \
  timeout 10s <probe-path>/boompi_rockchip_3a_hil \
    --execute --allow-rockchip-3a-call
```

`timeout 10s` 必须由探针外部提供，因为 vendor init/process/destroy API 自身没有 timeout 参数。
超时后先停止本轮并检查进程、core/debug dump 和内核日志，不得立刻并发启动第二个探针。

以下条件全部关闭前，**禁止在板端使用 execute 模式**：

1. 当前板运行的是目标 BSP/镜像，且 `/oem/usr/lib` 三个文件的实际 SHA-256 与本指南完全一致；
2. 48 kHz 全双工基线已经完成；
3. 当前镜像的两个物理麦克风 slot、唯一 reference slot、交织顺序和 reference 来源已经用可辨识
   信号实测确认，且没有把模拟麦、数字 reference 或两个 reference 错误叠加；
4. 已完成执行前进程/资源排他和连续 dmesg 证据方案。

这些闸门未关闭时仍可审阅源码、交叉构建和运行 dry-run，但不得用合成逻辑通道绕过真实
slot/reference 问题。

## 结果与退出码

探针向标准输出写一个 JSON 对象。dry-run 应报告 `mode=dry_run`、
`vendor_library_load_attempted=false`、`vendor_calls_attempted=false`、
`mutated_external_state=not_evaluated`；execute 会分别记录 load/symbol resolution、参数准备、init、
process、destroy、参数释放、unload、微秒耗时、返回值和 guard 状态。execute 调用 vendor 后的
`mutated_external_state` 固定为 `not_evaluated`，不能据此声称没有库级副作用。源码定义的退出码为：

| 退出码 | 含义 |
| --- | --- |
| `0` | help/dry-run 正常，或 execute 的单帧返回 512 bytes 且 guard 完整 |
| `2` | 未知/重复参数，或只给出一个执行 opt-in |
| `3` | 检测到 vendor 调试路径/loader override 变量，或无法关闭 core dump |
| `4` | vendor load/symbol resolution、参数树准备失败，或 init 返回 null |
| `5` | process 返回值不等于 512、内存 guard 被破坏，或 vendor unload 失败 |

外层 `timeout` 的退出状态由目标系统实现决定，应与探针自身退出码分开记录。即使 execute 返回
`0`，也只关闭“当前 pinned direct API 完成一次合成固定帧调用”这一项；它不证明：

- 物理双麦/参考的 slot 映射、左右顺序、极性、幅度、延迟或时钟关系；
- AEC、BF、ANR、AGC 的实际效果，ERLE、残余回声或 double-talk；
- 连续 16 ms 帧的实时率、最坏耗时、错误恢复、CPU/RSS 或长时间稳定性；
- 48 kHz 到 16 kHz 重采样、ALSA/MPI 全双工或产品 VAD 链路。

板端执行时必须另行记录目标镜像、库哈希、命令、探针退出码、外层 timeout 状态和 dmesg 边界；
没有这些真实证据时只能写“未验证”，不得补写推测结果。
