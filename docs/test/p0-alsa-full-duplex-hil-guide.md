# P0 直接 ALSA 全双工 HIL 操作指南

本指南对应 `scripts/hil/rv1106_alsa_full_duplex.sh`。它是一个显式 opt-in 的板端测试工具，
不是生产音频后端，也不是只读探针。2026-07-28 已在真实板卡完成两轮 direct ALSA
transport 全双工，精确结果、当前旧镜像差异和未关闭边界见
[真板验证记录](p0-alsa-full-duplex-validation-20260728.md)。本指南继续定义操作契约，不能用
一次通过替代目标自定义 BSP、物理通道、reference 或 AEC 验证。

## 第一轮固定契约

- direct `hw:C,D` capture 与 playback，设备号必须由当前板只读探测后显式传入；
- 请求 `48 kHz / S16_LE / 2ch / RW_INTERLEAVED`；
- 请求 480-frame period 和 1920-frame buffer，实际接受值以 `arecord -v`、`aplay -v`
  产物为准，不能把请求值写成协商结果；
- capture 6 秒、playback 4 秒，播放内容固定为 768000 bytes 全零 raw PCM；
- 只有两个 PCM 都成功、capture 为 1152000 bytes、单调时钟重叠不少于 3000 ms、且
  新增 dmesg 后缀没有 xrun/underrun/overrun 时才通过；
- 如果 dmesg 不可读或 ring buffer 前缀无法连续比对，结果为 `inconclusive`，不能降级成通过。

480 点只用于首先验证 vendor/硬件的 10 ms 候选；产品固定的 20 ms frame 可以在能力关闭后
聚合两个 period。首轮不能把 960 点静默写成 Rockit 或 Codec 已支持的硬件前提。

## 安全与隐私边界

脚本默认只做 dry-run。真正打开 PCM 前必须同时提供 `--execute`、`--allow-pcm-io` 和
`--allow-mixer-write`。执行还要求一个不存在的安全绝对 artifact 目录；已有目录、普通文件、
符号链接和相对路径都会被拒绝，目录及其中录音使用 owner-only 权限。

脚本只修改调用方指定的单个 DAC enum 控件，并要求它公开精确的 `Off` item。修改前保存
`numid` 与原始 numeric index，写入后回读；正常退出、错误、HUP、INT、QUIT 和 TERM 的退出处理
只处置本脚本创建的 capture/playback PID，并尝试恢复 mixer 后再次回读。恢复失败使用独立退出码
6，并优先于原始测试错误。脚本不会运行 `killall`、停止服务、调用 `alsactl store`、改 ADC gain/MICBIAS、
切换 loopback、安装库或写启动配置。

capture/playback 的主等待循环共用 12 秒单调时钟 deadline；清理对每个子 PID 的 TERM 和 KILL
轮询也都有界。这个承诺只覆盖脚本自己的 PCM 子进程等待与清理，不是整个 shell 进程的绝对
watchdog：同步执行的 `fuser`、`amixer`、`readlink`、`dmesg` 或底层内核 ioctl 仍可能卡在内核
不可中断状态。执行端必须另设 SSH/主机 watchdog；一旦外层超时，应停止本轮测试，人工核对并
恢复 DAC enum，检查残留进程，必要时重启板卡，绝不能继续下一项音频测试。

脚本在自己的失败路径中先尝试恢复 mixer，随后停止等待并把本次 HIL 记为失败。mixer 恢复不
依赖 artifact 目录仍可写；结果 JSON 先写临时文件并检查成功，再在同目录原子改名，任何写入
失败都不能输出 `pass`。脚本依赖调用环境中的外部命令，因此只能在受信任、root-owned 的板端
rootfs 与 `PATH` 下执行，不能从可由普通用户写入的目录解析 `amixer`、`fuser` 等命令。

artifact 中的 `capture.raw` 可能包含环境语音；完整 dmesg 快照也可能暴露 MAC、序列号、内核
命令行或其他设备身份。只有用户明确同意本次短录音时才执行；目录应位于板端临时存储，权限
保持 `0700/0600`，分析后删除。原始录音与完整 dmesg 都不得提交 Git、公开上传或直接放进诊断包；
需要共享时只摘录完成脱敏且与故障直接相关的最小片段。

## Dry-run

以下命令只校验参数，不访问 `amixer`、不创建目录，也不打开 PCM：

```sh
scripts/hil/rv1106_alsa_full_duplex.sh \
  --capture-pcm hw:<card>,<capture-device> \
  --playback-pcm hw:<card>,<playback-device> \
  --mixer-card <card> \
  --dac-control 'DAC Control Manually' \
  --artifact-dir /tmp/boompi-alsa-hil-<host-timestamp>
```

## 显式执行

只有完成运行库、PCM 占用和 mixer 名称只读核对后，才在同一命令追加：

```text
--execute --allow-pcm-io --allow-mixer-write
```

脚本在 mixer 写入前和 PCM 启动前各运行一次 `fuser`。发现占用时退出 3，只报告冲突，不杀
进程。完整走到汇总阶段时，输出目录包含 mixer 前后读回、arecord/aplay 输出、dmesg 前后与连续
delta、`capture.raw`、全零 playback 输入和 `result.json`；早期配置失败、deadline 或 artifact
故障可能只留下部分产物，此时必须同时依据退出码和已有产物判断。JSON 使用单调时间，只记录
设备参数、PID、退出码、字节数、重叠、恢复状态和 dmesg 结论，不记录用户名、IP、命令行或网络身份。

## 当前禁止直接运行的 vendor 程序

- `sample_ai_aenc_adec_ao_stresstest` 及其 stress wrapper；wrapper 可能执行 `killall rkipc`。
- 预构建 `rk_mpi_ai_test`；它可能修改 capture volume/mute 和 Mode2 loopback，且使用无限
  `GetFrame` timeout，退出时还会无条件写 `Disabled`。
- 预构建 `rk_mpi_ao_test`；它可能把 output volume 设为 100，并用无限 SendFrame/WaitEos
  timeout 和无限 retry。

这些程序只作为 SDK 源码证据，不作为 boomPI 的 HIL 执行入口。

## 离线验证边界

`scripts/tests/test_rv1106_alsa_full_duplex_hil.py` 使用临时 artifact 和 fake
amixer/arecord/aplay/fuser/dmesg，覆盖 dry-run 零写入、缺少 opt-in、占用、成功、子进程
失败、字节数、xrun 和 mixer 恢复失败。fixture 只在临时脚本副本中把唯一的 `/proc/uptime`
读取替换为等比例测试时钟；生产脚本没有时钟覆盖入口。fake 测试不会打开真实声卡，也不能
替代板端执行。
