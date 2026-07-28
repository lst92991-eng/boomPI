# P0 Rockchip MPI 音频 HIL 只读前置验证记录（2026-07-28）

- 最后采集时间：2026-07-28 16:07:16 +08:00（Asia/Shanghai，以 host 时钟为准）。
- 板卡/镜像：RV1106 自研板，Buildroot 2023.02.6，镜像版本
  `-g712a500de-dirty`，Linux 5.10.160 `#8`，BusyBox 1.36.1。
- 板端时钟：错误地停留在 2021 年，不能作为本记录时间来源。
- 结果：只读 preflight 完整采集并正确阻断执行；**raw MPI ARM HIL 没有运行**。
- 副作用边界：没有打开 PCM、访问 mixer、调用 MPI、发送 signal、停止服务、终止进程、
  创建/删除/改名板端文件或采集网络标识；本轮未主动修改 `rkipc` 或板端网络配置。

## 为什么必须增加独立 preflight

原始 C++ HIL 只对配置的 `/dev/snd/pcm*` 做两次 `/proc/*/fd` 快照。当前镜像实测
`rkipc` 没有持有 `/dev/snd/*`，但持有 Rockchip MPI 设备并加载 `librockit.so`、
`librockchip_mpp.so.0` 和 `librkaudio.so`。只看 PCM 会错误地把这台正在使用 MPI 的板卡
判断为空闲，然后进入 `RK_MPI_SYS_Init()`。

本轮因此同时完成两项最小修正：

1. 新增固定用途、只读且永远不会给出执行许可的
   `scripts/probes/rv1106_rockchip_mpi_audio_preflight.sh`；
2. C++ HIL 的两次快照从“配置的 capture/playback PCM”扩展为“配置的 PCM 加全部
   `/dev/mpi/*`”。它发现任一占用都会在首次 MPI 调用前失败，仍不终止任何进程。

这两层都只是 fail-closed 快照，不是服务锁，也不能消除扫描后的竞态。

## 当前板端实测结果

preflight 经 SSH stdin 直接运行，没有复制到板端文件系统。schema v1 的关键结果是：

```text
probe_status=complete
proc_fd_scan_complete=true
snd_fd_count=0
snd_owner_count=0
mpi_fd_count=22
mpi_owner_count=1
rkipc_process_count=1
rkipc_mpi_owner_count=1
safe_to_execute=false
exclusivity=unproven
kernel_log_continuity=unproven
```

主要阻断 reason code 为：

```text
rkipc_process_present
rkipc_mpi_device_owner_present
mpi_device_owner_present
dmesg_follow_option_not_listed
target_timeout_tool_missing
service_stop_lexical_risk_detected
```

ALSA 只读清单仍是 card 0 `rv1106-acodec`，同一个 00-00 endpoint 声明 playback 1 和
capture 1，设备节点为一个 control、一个 capture 和一个 playback。该声明没有证明
48 kHz 全双工、S16_LE packing、双麦 slot、数字 reference 或可听播放。

## init、工具和内核日志能力

PID 1 是 BusyBox init，`/etc/inittab` 通过 `/etc/init.d/rcS` 启动系统；文件中唯一明确的
`respawn` 项是 console getty。这个静态事实不等于已经建立了 rkipc 的防拉起维护模式。

板端有 `dmesg/readlink/awk/sleep/usleep/kill/mkdir/mv/cmp/sed/wc/sha256sum/flock/ps/fuser/scp`，
缺少 `timeout` 和 `stat`。`dmesg` 当前 root 可读，但 BusyBox help 只有
`[-cr] [-n LEVEL] [-s SIZE]`，没有 `-w/--follow`。`/dev/kmsg` 存在且权限可读，但本轮没有
打开它；stream 起点、sequence/gap、ring wrap、阻塞和精确终止语义仍为 `unverified`，因此
不能登记 continuous kernel-log evidence。`flock` 也只会串行化遵守同一锁的进程；当前
`rkipc` 不遵守 boomPI 锁，不能据此宣布排他。

目标没有 `timeout`，未来 watchdog 必须由固定的远端父 shell 和 host 双层约束，并只跟踪
它直接启动的探针 PID/starttime/exe。断开 SSH 不能证明 D-state 进程已经退出。

## 当前 OEM start/stop 链

只读脚本审计得到：

```text
/etc/inittab
  -> /etc/init.d/rcS
  -> /etc/init.d/S21appinit start
  -> /oem/usr/bin/RkLunch.sh
  -> post_chk
  -> vendor rkipc application（启动参数不写入仓库）
```

只读快照确认当前实例由启动链拉起，但这仍不足以证明随意结束后可无损恢复。

`S21appinit stop` **禁止用于自动 HIL**，因为它调用 `RkLunch-stop.sh`，而后者包含：

- `killall rkipc`，不是 PID/starttime/exe 精确身份控制；
- `killall udhcpc`，可能破坏 SSH/watchdog 控制面；
- 无固定 deadline 的 rkipc 等待循环；
- 对全部 OEM init 脚本执行 `rcK`。

`S21appinit start` 也不是无副作用恢复命令：`RkLunch.sh` 会处理网络、链接、配置、kernel
参数和整组 OEM service，并在存在测试 WAV 时运行 vendor AO sample。因此 runner 不能把
`S21appinit stop/start` 包装成一个“安全服务生命周期”。

当前镜像关键文件/进程哈希为：

| 对象 | SHA-256 |
| --- | --- |
| `/etc/init.d/S21appinit` | `61dcd3bdb6740771b296611ab83e3238f9bc45b2ca0ec5ae78b542085be592e5` |
| `/oem/usr/bin/RkLunch.sh` | `d8805427307e57c45bcfc6a9057384e24eb1be4fcc1a1826cc4f72a0e5360e04` |
| `/oem/usr/bin/RkLunch-stop.sh` | `836984930a929a73eb75fced23a3978415e60b4295b8d7f16cc4b70301fb71c0` |
| `/etc/init.d/S60micinit` | `62c72b9b028654966719242da794dc2d09a1d24c1f5e8b196bb72f152d203fcc` |
| 当前 `/oem/usr/bin/rkipc` executable | `d39f05006407ab45df3cdd93f656da71580694385e16ae6d5dfddf361bfe6d8c` |

这些哈希只固定本次镜像证据，不授权修改或停止对应对象。

## 仓库产物与离线验证

| 对象 | SHA-256 |
| --- | --- |
| `scripts/probes/rv1106_rockchip_mpi_audio_preflight.sh` | `87aab61be470430d59a27fb180e3d6f0dc0f674cec55aaa3589b0b84466ff185` |
| `scripts/tests/test_rv1106_rockchip_mpi_audio_preflight.py` | `ba8f314f949d1dc3ef9a0a0cd883686308581f92811fd649a7c892e78cc7d9d3` |
| 更新后的 `client/tests/hil/rockchip_mpi_audio_hil.cpp` | `2898b80c5c2d5ea4ed87c621da81dadfd091b04edaf2764712d653eb6ec0735e` |

Linux fixture 回归 9/9 通过，覆盖 help/非法参数零探测、无 owner 仍保持 blocked、仅持
`/dev/mpi/vsys` 的 rkipc、PCM owner、`/proc` 扫描失败、无 dmesg follow、危险 OEM stop
词法、隐私和零写入。完整 Python/script discovery 为 62/62；MPI CMake 专项 10/10、vendor
CMake 专项 12/12、Host CTest 16/16、Go test/vet/build 全部通过。真实交叉构建哈希和 ELF
审计见 [MPI HIL 构建验证记录](p0-rockchip-mpi-hil-build-validation-20260728.md)。

POSIX host 可直接使用：

```sh
ssh <board-host> sh -s < scripts/probes/rv1106_rockchip_mpi_audio_preflight.sh
```

Windows PowerShell 必须避免 `Get-Content | ssh` 的文本重编码/BOM；使用 `cmd` 的二进制
stdin 重定向：

```powershell
cmd /d /s /c "ssh <board-host> sh -s < scripts\probes\rv1106_rockchip_mpi_audio_preflight.sh"
```

## 下一项板端闸门

当前不实现或运行通用 runner，也不调用 OEM stop 链。下一步必须先设计当前镜像专用的
maintenance boot：在 rkipc 首次启动前跳过它，同时保留 Ethernet/DHCP/SSH，并以固定镜像/
脚本哈希证明不会自动拉起。这个动作涉及启动流程或镜像修改，必须取得用户对该次操作的明确
授权。维护模式验证通过后，才能补精确 watchdog、内核日志证据并运行 raw MPI HIL。

即使未来 raw transport 通过，也仍不能推出双麦布局、reference slot、极性、时钟同步、
S16_LE packing、真实 presentation 或可听扬声器输出。
