# P0 vendor 音频证据基线（2026-07-27）

采集时间：2026-07-27 20:27:14 +08:00。

> 后续更新：同日已完成匹配 GCC 8.3/uClibc 的 Rockchip 3A tests-off 默认 ALL
> [交叉链接与符号检查](p0-rockchip-3a-link-validation-20260727.md)。该结果只证明
> `libaec_bf_process.so`/common 与三个公开入口可由目标 linker 解析；没有运行板端 ELF，
> 不改变本文对 PCM、物理 slot、3A 效果和实时率“未验证”的结论。
>
> 2026-07-27 21:12:34 +08:00 又完成 Rockchip MPI 音频的 tests-off 默认 ALL
> [交叉链接与 21 个 raw 生命周期符号检查](p0-rockchip-mpi-link-validation-20260727.md)。
> 同轮 schema v2 板端只读探针仅确认一个 capture PCM、一个 playback PCM、Rockit、AI/AO
> test 与直接 3A 库存在，且 VQE JSON 缺失；没有打开 PCM 或执行 vendor。板端时钟错误地
> 停留在 2021 年，故以 host 时间为准。随后物理链路断开，未进行 HIL。
>
> 2026-07-28 后续：direct ALSA 已在当前旧 `RV1106-Atguigu` 镜像完成 48 kHz/S16_LE/2ch
> transport 真全双工；该结果没有关闭目标自定义 BSP、物理通道/reference、raw MPI 或 3A。
> 见[真板验证记录](p0-alsa-full-duplex-validation-20260728.md)。

## 结论

匹配 BSP 已提供完成最小音频闭环所需的三类候选入口：Rockit
`rk_mpi_ai`/`rk_mpi_ao`、直接 ALSA PCM、Rockchip VQE/`libaec_bf_process.so`。截至
2026-07-27 的原始盘点，证据足够开始制作板端探针，但不够宣称 48 kHz 全双工、四通道
回采或 AEC 已经可用；上方 2026-07-28 follow-up 只补充关闭 direct ALSA transport。

20:27 的原始盘点只读取 SDK、已装配 rootfs/OEM、DTB、头文件、样例和 ELF，没有连接
开发板。21:12 的后续只读探针连接了当前板/镜像，但仍没有打开 PCM、修改 mixer、录音、
播放、执行 vendor test 或写入镜像。SDK 基线 commit 为 `994243753789`，工作树有用户已有
改动，盘点过程没有修改。下文用 `<BSP_ROOT>` 代替开发机绝对路径。

| 项目 | 当前结论 | 不能外推的事项 |
| --- | --- | --- |
| Rockit raw AI/AO | API、样例、目标库和预构建测试程序齐全 | 真实 card、参数协商、全双工、MB 总长度与 packing |
| 直接 ALSA | 2026-07-28 当前旧镜像的 48 kHz/2ch transport 全双工通过 | 目标 BSP、物理通道、reference 和声学质量 |
| DTB `TRCM clk-trcm=1` | TX/RX 共享 TX LRCK/BCLK | 不表示四通道，也不表示存在 DAC reference slot |
| AI VQE 样例 loopback Mode2 | vendor test 启用 VQE 时会设置该 mixer 控件 | 不表示本板为双麦+reference，也不是 DTB 的 TRCM 设置 |
| Rockchip 3A | ARM/uClibc ABI、固定帧、input/output 长度已核对 | 物理 packing/slot、错误恢复、声学参数和实时率仍需 HIL |
| OEM 3A/VQE 资源 | 直接 3A 的 AEC/common/detect 库已装配 | RockAA wrapper、VQE JSON 未装进当前 OEM/rootfs |

## Rockit raw PCM

权威头文件：

- `<BSP_ROOT>/media/rockit/rockit/mpi/sdk/include/rk_mpi_ai.h`
- `<BSP_ROOT>/media/rockit/rockit/mpi/sdk/include/rk_mpi_ao.h`
- `<BSP_ROOT>/media/rockit/rockit/mpi/sdk/include/rk_comm_aio.h`
- 同目录的 `rk_mpi_sys.h`、`rk_mpi_mb.h` 和 `rk_comm_mb.h`

主要参考样例是
`media/rockit/rockit/mpi/example/mod/test_mpi_ai.cpp` 与 `test_mpi_ao.cpp`。产品只应抽取
raw AI-only/AO-only 路径；`sample_ai_aenc_adec_ao_stresstest` 会构造编码/解码媒体图，
其 wrapper 还可能执行 `killall rkipc`，不能作为 boomPI 的最小 48 kHz 探针。

raw PCM 不需要 `RK_MPI_SYS_Bind`。最小生命周期如下：

1. 一次 `RK_MPI_SYS_Init()`；全部 AI/AO 使用者停止并 join、设备逆序释放后，只调用一次
   `RK_MPI_SYS_Exit()`。
2. AI：`SetPubAttr -> Enable -> [SetChnParam] -> EnableChn`；仅当设备率与输出率不同时
   `EnableReSmp`。用有限 timeout `GetFrame`，每个成功取得的普通/EOS frame 都恰好
   `ReleaseFrame` 一次。
3. AO：`SetPubAttr -> Enable -> [SetChnParams] -> EnableChn`；仅当输入率与设备率不同时
   `EnableReSmp`。`bBypassMbBlk=false` 时，`SendFrame` 返回即可释放调用方 MB，所有失败
   路径也必须释放。EOS 是 `u32Len=0` frame，之后有限 timeout `RK_MPI_AO_WaitEos`。
4. 停止时先停/join 取送帧执行者，再按成功状态逆序 Disable VQE/resample、channel、device。
   `SetChnParam(s)` 是产品建议而不是 raw API 必需。官方 test 中的无限 timeout、goto
   retry 和未检查返回值不得照搬。

头文件枚举包含 48 kHz、16-bit、mono/stereo/4/6/8 声道模式，但枚举存在不等于当前
硬件支持。`AI_MAX_CHN_NUM=1` 只证明最多一个逻辑 AI channel；若当前板双麦由 Rockit
同时采集，则应位于该 channel 的多声道 frame，真实物理 packing 仍需 HIL。
`AUDIO_FRAME_S::u32Len` 的头文件语义已明确为每通道长度；MB 总长度、
interleaved/planar、左右 slot 和可用 frame points 仍需板端验证。

产品候选库是 `media/rockit/rockit/lib/lib32/librockit.so`，不是较大的
`librockit_full.so`，也不是不导出 AI/AO 的 `librockit_tiny.so`。其动态依赖为：

```text
librockchip_mpp.so.1
librga.so
libstdc++.so.6
libgcc_s.so.1
libc.so.0
```

CMake link-check 使用 `media/out/lib/librockit.so` 的同哈希副本，并显式固定
`media/out/lib/librockchip_mpp.so.0` 与 `media/out/lib/librga.so`。后两者是未 strip 链接
候选；OEM 中经过 strip、哈希不同的 MPP/RGA 副本不能替代这些 CMake 输入。MPP 的真实
文件名虽为 `.so.0`，其 `SONAME` 和运行时所需名称为 `librockchip_mpp.so.1`。

当前 OEM 已装配 `librockit.so` 及依赖，并包含 `rk_mpi_ai_test`、`rk_mpi_ao_test` 和
stress sample。raw AI/AO 不要求 VQE JSON，但需要驱动、设备节点和板级 mixer 初始化。
当时 BSP 工作区的目标 rootfs overlay `S60micinit` 会设置 `DiffadcLR`、MICBIAS、左右 MIC
Work、gain/ALC；2026-07-28 真板复核发现当前实际烧录镜像仍是 `RV1106-Atguigu`，其
`S60micinit` 设置 `SingadcL`。因此工作区 overlay 不能冒充板端启动事实。这些
是本板前置条件，不是通用 Rockit API 默认行为。

官方 test 构建还使用 pthread 和 test-only helper；产品 CMake 应以 imported
`librockit.so` target 加 `Threads::Threads` 为边界，不链接 `rt_test_comm`/`libsample_comm`，
也不携带 SDK 构建目录 RPATH。

## ALSA、Codec 与两个“Mode”

BSP 工作区候选定制 DTB 的 `acodec`、`i2s0_8ch` 和 simple-audio-card 均为 enabled，I2S
`mclk-fs=256`，`rockchip,clk-trcm=<1>`。Rockchip binding 将这个值定义为收发共用 TX
LRCK/BCLK；驱动在 TRCM 下对 TX/RX 共启停，并要求 symmetric rates。Codec/DAI 的声明
范围覆盖 48 kHz 和 capture 1..4ch，因此它们只是全双工候选。

`rk_mpi_ai_test` 在启用 AI VQE 时调用 mixer：

```text
I2STDM Digital Loopback Mode = Mode2
```

这与 DTB 的 `TRCM clk-trcm=1` 不是同一个设置。vendor 样例的某些配置、BCD mask 或 codec 的
四通道上限，都不能证明真实 frame 是 `MIC-L, MIC-R, REF-L, REF-R`。只有同步播放可辨识
左右序列并做多通道相关性分析，才能确认 slot 和数字 reference 位置。

预构建 `rk_mpi_ai_test` 的退出路径会把该 loopback mixer 无条件写成 `Disabled`，初始化
失败也可能留下 `Mode2`。任何 HIL 都必须逐项 `amixer cget` 保存将改的枚举/开关值，设置后
回读，并用 trap/失败清理恢复原值；没有 `alsactl` 时不能假定原值就是 `Disabled`。

## Rockchip VQE 与直接 3A

Rockit AI VQE 通过 `RK_MPI_AI_SetVqeAttr`/`EnableVqe` 使用 `AI_VQE_CONFIG_S`。官方 test
只接受 10 ms 或 16 ms VQE gap，并在启用时请求上述 Mode2 loopback。当前
`media_out/share/vqefiles` 有 AI/AO JSON 和部分模型，但装配后的 OEM/rootfs 没有
`librockaa.so` 或 `vqefiles`；因此 RockAA/Rockit 文件配置模式在打包规则补齐并于板端
验证前不可用。

直接 3A 的公开入口是：

```c
void *rkaudio_preprocess_init(
    int rate, int bits, int src_chan, int ref_chan, RKAUDIOParam *param);
int rkaudio_preprocess_short(
    void *handle, short *input, short *output, int input_size,
    int *wakeup_status);
void rkaudio_preprocess_destory(void *handle);
```

匹配 header/PDF/wrapper 和目标 binary 已关闭基本 ABI：16 kHz、16-bit、固定 256 samples
（16 ms）；`src_chan=2, ref_chan=1` 时 `input_size` 是总共 768 个 `short`（1536 bytes），
direct 输入为交织逻辑通道，默认未 array-reset 的顺序为 ref-last `[mic0,mic1,ref]`；
BF mono 成功返回 512 输出 bytes，尺寸不符返回 0，init 失败返回 null。上游物理 slot 如何映射到这三个逻辑通道、
错误后的恢复和持续实时率仍需板端验证。公开能力包含 AEC、BF、ANR 和 AGC；
`wakeup_status` 不是产品 VAD，boomPI 仍需独立 VAD。

直接 `rkaudio_preprocess_*` 不读取 JSON，调用方构造 `RKAUDIOParam`；
`config_aivqe.json` 由 RockAA/Rockit file mode 解析。该基线 JSON 启用 AEC、BF、fast AEC、
AES、AGC、ANR、dereverb 和 howling，关闭 delay、GSC、NLP、CNG、DTD、EQ、DOA、wind、
AINR 和 wakeup，因此不需要 AINR/wakeup `.rknn` 才能表达这份配置。header helper 默认值
又与 JSON 不一致，不得盲用。OEM 中的 `librkaudio.so` 是无关播放器库，不能当成麦克风
3A；直接路径必须精确识别 `libaec_bf_process.so` 与 `librkaudio_common.so`。Rockit VQE
和直接 3A 是二选一候选，不得叠加两次 AEC/NS/BF/AGC。

## 固定输入摘要

这些哈希固定本次盘点输入，不是发布许可或功能验收：

| 文件 | SHA-256 |
| --- | --- |
| `rk_mpi_ai.h` | `5eb52c01056bdf6cdb4948a2a39d58172460dbcf7700e279774942f507b011cd` |
| `rk_mpi_ao.h` | `e297104409a67f5d794bc111f900faae91b453f7255a0ed858f163a21201d618` |
| `rk_comm_aio.h` | `95a76ae4d8dbd29563094c2e33ed5e200aeeef8ef6bc4426ff0ab34239d91867` |
| `rk_mpi_sys.h` | `0b7d08b59d437acfb2bbbdabfbb39b77631b34cd904b2ebd041ba34c98fcbac9` |
| `rk_mpi_mb.h` | `0c54ef75e4904096165e6229469e75bb981ebb535ee8cd1699b6bb27857375cf` |
| `rk_comm_mb.h` | `7ba6b839615f7c62340562d93db2d01e2cf5d3e47f90d6f7b68e0b685a4ddd39` |
| `rk_common.h` | `ef3da84bf65e727de587be7665f29dbdb135326549736dbed0c1366d53e2b418` |
| `rk_type.h` | `1ca5eabff89c39034a5be31185a13709da0f697f3f9cac7637e41ea59bed924f` |
| `librockit.so` | `3f92f8c41ffe9ad72e407b68750906fcff89ea06758f14a3fc2a3d87061e3d0f` |
| `media/out/lib/librockchip_mpp.so.0` | `e8183339fff1dd466adc9567be5c4c98239c567157eaebefe4a2fe50f793fec8` |
| `media/out/lib/librga.so` | `13cf7d10210cdf43a998a07a9bf0033821dfec61b31d9c50195848c0480010c7` |
| `rk_mpi_ai_test` | `633dede4ac9dda4d17d3d7d185067fa89c09f88e64af3d3b4aab5468b4b6265e` |
| `rk_mpi_ao_test` | `2a77b02a6371c15124909c3bf84b7922614bac899aaffab98ae6da3ede6738a6` |
| `config_aivqe.json` | `1d160fde184935cf43a49feae7be0dfd24efdc82ff9de2ea8b35aba6318074f9` |
| `config_aovqe.json` | `fef45ef54a843245b030388e67eb8360b76f4c610df2d9113ef46958decd9eb7` |
| `libaec_bf_process.so` | `5abbcf518ffa39900dd78352547ebf5feab83d2f9b30a82c1e2dc1dc44b25e07` |
| `librkaudio_common.so` | `4f4c9d78028a592174c3e959e35d231317d0ed2a864a1ed230f8adab42960246` |

## 板端闭环进度与下一项

严格按以下顺序推进，每一步单独记录当前板卡、镜像、命令、返回值和 dmesg：

1. **只读资源盘点已完成**：schema v2 记录了 ALSA 节点以及 MPI、Rockit、3A 库和 VQE
   资源存在性；这一步没有打开 PCM 或执行 vendor，不能记为功能通过。
2. 记录 direct-hw 参数和测试前 dmesg，确认无其他进程占用 PCM，逐控件保存 mixer；
   先保持模拟 DAC Off，以有限时长数字静音让 48 kHz S16_LE 2ch capture/playback 明确
   重叠，验收所有 PID/exit code、录音字节数和测试后新增 xrun，并恢复/回读 mixer。
3. 分别用直接 ALSA 和 raw rk_mpi 关闭同一事实问题；不使用会 kill 业务进程的 stress wrapper。
4. 能力查询确认支持后才尝试 4ch。固定 TRCM 和其他变量，逐次保存/设置/恢复
   loopback `Disabled/Mode1/Mode2/Mode2 Swap`；保持模拟 DAC Off，播放正交低幅 L/R
   序列并同步采集，以相关性识别双麦/reference/极性/延迟。再分别改变数字幅度、AO volume、
   codec mixer/mute，确定 reference tap 在音量/mixer/DAC 前后的位置。
5. 将确认后的 48 kHz 输入降采样为 16 kHz，先做 3A init/link/固定帧短测；只有错误恢复、
   CPU/RSS、单帧最坏耗时和实时率通过后，才进入 AEC 声学评估。

在这些结果出来前，不继续增加 playback/control/committer/worker 抽象，也不把
loopback Mode1/Mode2、四声道或 3A 库存在写成“已接通”。
