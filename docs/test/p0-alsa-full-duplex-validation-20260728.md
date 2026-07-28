# P0 RV1106 直接 ALSA 全双工验证记录（2026-07-28）

- 采集时间：2026-07-28 20:48:49 +08:00（Asia/Shanghai，以 Windows host 时钟为准）
- 代码基线：`aa5ec229e52f72d59061473242bf66341eaa397e`
- 结果：**直接 ALSA transport 全双工通过；目标自定义 BSP 与双麦布局闸门未关闭**
- 测试后端：仓库内有界 direct ALSA HIL；没有访问 Qwen 或任何公网 API

## 结论

真实 RV1106 上的 `hw:0,0` 已同时完成 48 kHz、S16_LE、RW_INTERLEAVED、2ch capture
和 playback：两轮测试均得到约 3.94 秒重叠，录放进程退出码均为 0，capture 输出长度
精确，playback 输入文件长度精确且 `aplay` 返回 0；新增 dmesg 没有
xrun/underrun/overrun，测试后两个 PCM 均回到 `closed`。

这关闭的是当前板上 direct ALSA 的 48 kHz 双向 PCM 同时传输能力，不是最终
双麦/AEC 结论。只读复核发现当前板运行的 DTB model 是 `RV1106-Atguigu`，启动脚本把
`ADC Mode` 设置为 `SingadcL`；它不是 BSP 工作区中待烧录的 `LST RV1106 Custom Board`
配置。默认模式下第二个 PCM slot 全程固定为 `-32768`。把 ADC mode 临时切到
`DiffadcLR` 后，两个 slot 均出现非恒定且无满幅饱和值的样本，随后已精确恢复原来的
`SingadcL`。

因此当前 A/B 观察表明：保持其他已记录条件不变时，切到 `DiffadcLR` 后两个 slot 均出现
非恒定、无满幅饱和值的样本；它没有证明唯一根因或两只物理麦克风已经恢复。没有受控
敲击/声源，仍不能证明 U9/U12 到 PCM 左右的物理映射、极性、幅相一致性、capture/playback
时钟关系与漂移、数字播放 reference、reference tap 或 AEC 可用。

## 板端环境与镜像不一致

```text
kernel:        Linux 5.10.160, armv7l
rootfs:        Buildroot 2023.02.6, -g712a500de-dirty
BusyBox:       1.36.1
ALSA tools:    1.2.8
DTB model:     RV1106-Atguigu
board revision:未记录
sound card:    card 0, rv1106-acodec
capture PCM:   hw:0,0
playback PCM:  hw:0,0
```

板端 `/etc/init.d/S60micinit` 的 SHA-256 为：

```text
62c72b9b028654966719242da794dc2d09a1d24c1f5e8b196bb72f152d203fcc
```

该脚本实际设置 `ADC Mode=SingadcL`。BSP 工作区的自定义 overlay 脚本 SHA-256 为
`a0986b59bef5b7d3694f6cdbbaca6d6e1d609ae685a1c7a7188f56cdb5cb1316`，目标设置为
`DiffadcLR`，并显式启用左右 ADC、MICBIAS 与匹配增益。该候选尚未在本轮烧入板卡，不能
把工作区文件冒充当前运行镜像。

该 BSP 工作树仍有用户已有 dirty/untracked 内容，本轮也没有生成新的不可变 release
manifest。现有历史 image 哈希来自更早网表基线，不能自动升级为最新 `netlist.json` 的
烧录候选。烧录前仍须单独钉住 BSP commit 与 dirty patch manifest、BoardConfig、DTB/model、
`update.img`/分区镜像 SHA-256、rootfs 及 `S60micinit` 哈希；候选脚本中仍引用历史
`netlist (5).enet` 的注释也必须先改成当前硬件基线。

最新 `netlist.json`（SHA-256
`f668a52ac19debdeb1eb257e8c4601fec14a57e1662424d8969db060e8da5bcc`）只读复核确认：
U9 连接 `MIC0_P/MIC0_N`，U12 连接
`MIC1_P/MIC1_N`；四路分别经 1 µF 耦合电容连接 RV1106 的 `CODEC_MIC0P/N` 和
`CODEC_MIC1P/N`。这支持双输入拓扑，但不能替代通道相关性 HIL。

## 安全前置条件

执行前确认：

- capture/playback PCM 状态均为 `closed`，`fuser` 未报告 owner；
- `amixer`、`arecord`、`aplay`、`fuser`、`dmesg`、`usleep` 等脚本依赖均存在；
- `DAC Control Manually` 是单值 enum，原值 `None(0)`，精确 `Off(1)` 可用；
- dmesg 可读，`/tmp` 是规范路径且空间充足；
- `rkipc` PID 521 保留，未停止、未发信号；它未占用 `/dev/snd`，但仍占用
  `/dev/mpi/*`，所以本轮没有执行 raw MPI HIL；
- 仓库脚本 SHA-256 为
  `afcdbbe444ec647b22a3bc704f26194d52e059320d1ba88071447dfdd522a6ef`；板端副本匹配；
- Ubuntu 离线 fake 回归 17/17 通过，板端 dry-run 返回 `mutated=false`。

脚本使用三重 opt-in，播放内容是 4 秒全零数字 PCM，并在打开 PCM 前把模拟 DAC 切到
`Off`。PCM 子进程主等待 deadline 为 12 秒，子进程清理也有界；`fuser`、`amixer`、
`dmesg` 或内核 ioctl 本身不受该内部 deadline 保证。本次 SSH 编排另设 45,000 ms 外层
上限，两轮均在约 8 秒内正常结束，未触发外层超时。没有运行 vendor stress test、
`killall`、OEM stop、持久 mixer 保存或启动项修改。

核心执行命令固定为：

```text
rv1106_alsa_full_duplex.sh \
  --capture-pcm hw:0,0 --playback-pcm hw:0,0 \
  --mixer-card 0 --dac-control 'DAC Control Manually' \
  --artifact-dir <new-/tmp-path> \
  --execute --allow-pcm-io --allow-mixer-write
```

## 第一轮：当前 `SingadcL`

实际 ALSA 协商值：

```text
capture/playback:  hw:0,0
access:            RW_INTERLEAVED
format:            S16_LE
channels:          2
rate:              48000 exact
period:            480 frames / 10 ms
buffer:            1920 frames / 4 periods
capture duration:  6 s
playback duration: 4 s digital silence
```

结果：

```text
capture exit/bytes:  0 / 1,152,000
playback exit/input bytes: 0 / 768,000
overlap:             3,950 ms
dmesg delta:         clean, 0 new lines
mixer restored:      true
overall:             pass
```

仅统计、不输出样本内容的 PCM 分析显示：

```text
slot 0: mean +6492.814, AC RMS 383.096, range 4093..11984
slot 1: constant -32768, AC RMS 0, clipped 100%
```

所以该轮的 transport 可以通过，但 capture layout/信号质量失败，不能把 slot 0 直接当成
可用产品麦克风，更不能把 slot 1 当成第二只麦克风或 reference。

## 第二轮：临时 `DiffadcLR` 对照

外层事务先保存 `ADC Mode` 的 numid 19 和 numeric index，再把索引从 `SingadcL(1)` 临时
切到 `DiffadcLR(5)` 并回读。该临时 wrapper 先在 Ubuntu 执行 `sh -n`，SHA-256 为
`eb820787e31015df7cc6caf53a368eb0d9e446f4060e79de9e28857d2cfd9bfe`。它被设计为在可达的
正常/错误及已捕获 HUP/INT/QUIT/TERM 清理路径尝试恢复原 index 和回读，但没有对其错误/
信号路径做独立故障注入；SIGKILL、掉电或不可中断 D-state 也不在保证范围。本次正常路径
实际恢复已验证。本轮没有改 MICBIAS、ADC gain、ALC、loopback 或任何持久配置。

结果：

```text
capture exit/bytes:  0 / 1,152,000
playback exit/input bytes: 0 / 768,000
overlap:             3,940 ms
dmesg delta:         clean
DAC restored:        None(0)
ADC Mode restored:   SingadcL(1)
overall:             pass
```

板端只生成 count/sum/sumsq/min/max/cross-product 等聚合值，主机仅对聚合值计算：

```text
slot 0: mean +242.110, AC RMS 318.979 (-40.234 dBFS), range -3534..4602,
        half peak-to-peak -18.121 dBFS, full-scale saturation count 0
slot 1: mean -139.847, AC RMS 397.031 (-38.333 dBFS), range -6392..5537,
        half peak-to-peak -14.798 dBFS, full-scale saturation count 0
cross:  correlation 0.483046578, delta RMS 531.904, exact-equal 0.057986%
```

可重算这些数值的隐私安全聚合量为：

```text
slot 0: count=288000, sum=69727693, sumsq=46185136217.000002,
        min=-3534, max=4602, full-scale-saturated=0
slot 1: count=288000, sum=-40275812, sumsq=51030863233.999997,
        min=-6392, max=5537, full-scale-saturated=0
cross:  pairs=288000, sum_product=7867289352.9999995,
        sum_delta_sq=81481420745.000005, exact_equal=167
```

两路都不再是常量或削顶，且不是简单复制；这只关闭“双 ADC mode 下两个 PCM slot 均有
动态”的初步事实。没有受控单侧敲击、已知声学信号或电注入，因此正相关不能作为极性通过，
也不能据此命名物理左右。

## 恢复、隐私与残留

测试后确认：

- capture/playback PCM 均为 `closed`；
- `ADC Mode` 恢复为原始 `SingadcL(1)`；
- `DAC Control Manually` 恢复为原始 `None(0)`；
- `rkipc` PID 521 始终保留；没有操作 MPI、网络、设备树或启动项；
- 板端两轮 `capture.raw`、dmesg artifact 和全部临时脚本已删除；
- 原始 PCM、完整 dmesg 和临时脚本均未加入 Git。

首轮分析时有一份 PCM 临时复制到 Windows 编排主机；两次经过规范路径校验的自动删除均被
主机执行策略拒绝。该文件位于用户本地临时目录、仓库之外且未上传，但仍须由用户手工删除，
因此本轮隐私清理尚有这一项外部待办。后续分析已改为板端聚合统计，没有再复制录音。

## 未关闭项与下一步

1. 先从最新硬件基线生成不可变 BSP 候选 manifest，再取得本次明确烧录授权；烧入后只读
   验收 model、DTB、`S60micinit` 哈希、`DiffadcLR`、MICBIAS 和左右 ADC 状态。
2. 在正确镜像上重复本记录的 direct ALSA 全双工，防止临时 mixer 对照冒充启动配置。
3. 由用户在 U9/U12 附近分别施加受控单侧敲击/近场声源，关闭物理 slot、极性、幅相和
   通道一致性；没有人工刺激时不勾选该闸门。
4. 逐个保存/恢复 loopback mixer 后，用低幅可辨识数字序列验证 reference slot、tap、延迟
   和漂移；不能把 TRCM 或 enum 名称当作数据证据。
5. raw MPI 仍需专用 maintenance boot，在 `rkipc` 首次启动前跳过它并保留网络；禁止使用
   会 `killall rkipc/udhcpc` 的 OEM stop 链。
6. 通道/reference 布局关闭后，再运行固定 16 kHz/256-sample 的直接 Rockchip 3A 探针。
