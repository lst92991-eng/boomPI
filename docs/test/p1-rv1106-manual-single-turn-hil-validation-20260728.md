# P1 RV1106 手动单轮音频闭环 HIL 验证记录（2026-07-28）

- 最后采集时间：2026-07-28 20:18:59 +08:00（Asia/Shanghai，以 Windows host
  时钟为准）
- 结果：**通过**
- 板卡：RV1106 自研板；板卡版本未记录
- 镜像：Buildroot 2023.02.6，`-g712a500de-dirty`
- 内核：Linux 5.10.160，ARMv7
- 用户态：BusyBox 1.36.1、ALSA 1.2.8、uClibc hard-float
- 代码基线：`4c637bf304daefd220a7590abb826080695a27e9`

## 结论和证据边界

当前 `boompi-client --manual-single-turn` 已在真实 RV1106 板上完成以下闭环：

```text
hw:0,0 48 kHz / S16_LE / 2ch 采集
  -> 选择 mic slot 0、极性 +1
  -> FIR 48 kHz -> 16 kHz mono
  -> TLS/WSS + SPKI pin + device token
  -> Go deterministic fake backend（不访问 Qwen）
  -> 24 kHz mono 安全提示音
  -> 48 kHz mono 渲染并复制到 2ch
  -> hw:0,0 ALSA playback 写入和有界 drain
```

客户端和服务端均返回成功，协议计数、PCM 字节数及测试后资源清理一致。该结果关闭
“slot 0 选择、手动触发、固定三秒、单轮串行传输与 ALSA playback 写入”的 P1 闸门。

本轮没有让 capture 与 playback 同时运行，因此**不能**证明 48 kHz 采播全双工；也不能
证明第二只麦克风、双麦 slot/极性、数字 playback reference、AEC/NS/BF/AGC、VAD、
Snowboy、TTS 打断或正常局域网发现/配对已经可用。

## 测试拓扑与安全后端

板卡只有 link-local Ethernet，Ubuntu VM 位于主机 NAT 网段。本轮拓扑为：

```text
RV1106 127.0.0.1:27806
  -> 一次性 SSH reverse tunnel
  -> Windows host
  -> Ubuntu VM 192.168.137.128:动态端口
  -> Go TLS/WSS test server
```

Go server 使用测试包内的 deterministic fake `ConversationBackend`。配置加载阶段只注入
离线占位凭据，运行阶段不会构造 Qwen backend，也不会访问 DashScope 或产生付费请求。
该 fake backend 不持久化麦克风 PCM，只以原子计数器累计上行字节数；协议栈在处理期间
仍会瞬时缓冲或复制 PCM。

下行信号为 24 kHz、S16_LE、mono 的 440 Hz 正弦提示音：500 ms、峰值 1500、首尾
各 20 ms 淡入淡出。板端使用音量 60、扬声器增益 100%，并受 95% limiter ceiling
保护。ALSA 写入和 drain 已成功；现场是否清晰可听没有人工确认，因此声学可听性记为
**未验证**。

## 构建与部署产物

构建环境：

```text
Ubuntu kernel: 6.8.0-124-generic
CMake:         3.22.1
Go:            1.26.5 linux/amd64
cross GCC:     arm-rockchip830-linux-uclibcgnueabihf-g++ 8.3.0
ALSA headers:  1.2.8
OpenSSL:       3.5.7（固定 package，静态链接）
```

最终剥离后的板端产物：

```text
SHA-256:       ea46a27dd1b1a13966d00db301860d2ae737e0927782328951b6ce152b17ced1
file bytes:    4,347,140
ELF:           ELF32 little-endian ARM EABI5 hard-float PIE
interpreter:   /lib/ld-uClibc.so.0
ARM attributes: ARMv7-A, VFPv4, NEON, VFP register arguments
RPATH/RUNPATH: absent
host path:     absent
```

动态依赖为 `libatomic.so.1`、`libasound.so.2`、`libstdc++.so.6`、
`libgcc_s.so.1`、`libc.so.0` 和 `ld-uClibc.so.1`；没有动态依赖
`libssl.so` 或 `libcrypto.so`。VM、本机中转副本和板端副本的 SHA-256 一致；最终精确
制品保留在 VM 的
`/home/st/boompi-final-validation.yGCrAY/artifacts/boompi-client-4c637bf`。
产物只部署到 `/tmp/boompi-client-final-ea46a27d`，没有覆盖
`/userdata/boompi/v1-alpha/boompi-client`，最终板端临时副本已删除。

## 音频和协议契约

板端 ALSA 打开时精确设置 access、format、rate、channels、period 和 buffer；其中 rate、
channels、period 和 buffer 由程序显式回读并比对：

- capture/playback：`hw:0,0`
- 格式：48,000 Hz、S16_LE、RW_INTERLEAVED、2 channels
- period：960 frames（20 ms）
- buffer：3,840 frames（4 periods）
- capture mic slot：0
- capture polarity：+1
- 固定录音时长：3,000 ms
- 音量：60；扬声器增益：100

客户端不再“先录满三秒再突发上传”。它在单线程中每取得一个 20 ms 硬件 period，立即
完成单通道选择、FIR 降采样和一个 640 字节 WSS PCM 帧发送；媒体时钟还会限制测试
`null` PCM，防止假设备瞬时灌满服务端有界接收队列。最后一个上行帧发送后，客户端先
调用 `FinishCapture()`，由 `snd_pcm_drop()` 停止本轮 capture；只有成功后才发送
`turn.commit`、接收响应和开始 playback。最终真板客户端返回 0，因此该 capture stop
也已在匹配硬件上成功执行。

最终真板计数：

```text
client uplink_frames:    150
server uplink_bytes:     96,000
server commits:          1
client downlink_frames:  25
client playback_chunks:  26（含 renderer 最终 drain chunk）
client text_delta_bytes: 0
client exit:             0
Go test result:          PASS
```

上行是 16 kHz、S16_LE、mono，150 个 20 ms 帧；首帧带 START，末帧带 END，sequence
从 0 连续递增。下行是 24 kHz、S16_LE、mono，共 24,000 字节，由服务端严格重分块为
25 个 960 字节帧，END 在最后一帧，`response.done` 在 END 之后发送。客户端将其变换为
48 kHz 后写入双声道 ALSA。服务端按 turn 记录上行 START、END 和 commit 状态，拒绝
缺失/重复 START、DISCONTINUITY、END 后继续 PCM、END 前 commit 和重复 commit。

## 实测发现和修复

### 三秒录音突发上传

第一次真实长度运行中，服务端只收到 1,920 字节、0 次 commit，随后关闭连接。原因是旧
实现先把三秒音频留在 RAM，再无节流地发送 150 帧；transport 的 32 帧有界接收队列被
瞬时填满。40 ms 的旧主机 smoke 只有两帧，未覆盖该问题。

修复后改为 capture/decimate/send 的单线程 20 ms 流式循环，并把跨语言回归改为真实
3 秒、96,000 字节。没有新增 playback/control/committer/worker 抽象或额外产品线程。

### 非阻塞 ALSA drain 的完成状态

第二次板测已完成上行和下行写入，但 `snd_pcm_drain()` 首次返回 `-EAGAIN` 后，
`snd_pcm_wait()` 返回 `-EIO`。对当前 sysroot 中 alsa-lib 1.2.8 源码核对后确认：硬件
PCM 从 `DRAINING` 完成到 `SETUP` 时 poll 会报告 `POLLERR`；`pcm_state_to_error(SETUP)`
没有负错误，因此 `snd_pcm_wait_nocheck()` 返回 `-EIO`。这不是 xrun。

修复逻辑只在 wait 失败且 `snd_pcm_state()` 已精确为 `SND_PCM_STATE_SETUP` 时认定 drain
完成；XRUN、SUSPENDED、DISCONNECTED 和其他错误仍原样失败。最终真板客户端返回 0。

### capture 到 playback 的交接

收尾审查发现，早期通过版本发送最后一个采集帧后没有 stop/drop capture；虽然业务逻辑
不再读取，ALSA capture handle 仍可能保持 RUNNING，并在回答足够快时与 playback 短暂
重叠。修复只在本轮具体 ALSA 对象上增加 `FinishCapture()`，不增加通用 worker 或抽象层；
它在 commit 前执行 `snd_pcm_drop()`，失败即终止该轮。修复后的代码提交和新剥离产物已
重新完成全量 host、交叉构建与真板 HIL，上述计数再次全部通过。

同一轮审查还补齐了服务端上行 START/END/commit 的严格顺序验证，以及 hello watchdog
在“超时与收包同刻”的二次检查。对应边界均有离线回归；真板 150 帧流符合新契约。

## 旧进程保护、掉电和恢复

掉电前的初步运行中，脚本核对 BusyBox supervisor 的 PPID、start time、命令行、旧
client 路径和 SHA-256 后，执行 `SIGSTOP supervisor -> TERM 精确 child -> 两次 PCM
空闲快照 -> 测试 -> SIGCONT`。即使业务测试失败，旧 supervisor 仍恢复并拉起新的旧
client child；没有调用 OEM stop、`killall` 或停止 `rkipc`。收尾审查进一步让脚本记录
测试 client 的 PID/starttime/PPID/exe/cmdline/SHA；HUP/INT/TERM 即使落在 fork 窗口也先
延迟到身份采集完成，再按 TERM、有界等待、身份复核、必要时 KILL 的顺序清理。测试
client 未确认退出时，脚本不会恢复 supervisor。

随后用户为板卡断电。首次重启后持久化 pidfile 仍写着 PID 707，但 PID 707 已被
`/usr/bin/rkwifi_server` 复用。HIL 安全检查因 executable identity 不匹配而拒绝发信号，
该次检查没有向复用 PID 发信号，也没有影响 Wi-Fi；最终复测后的清理快照中
`/proc/707/exe` 已不存在，pidfile 仍未被测试修改。
旧 `--voice-loop` 没有随重启启动，两个 PCM 均为 `closed`，所以最终闭环按冷启动空闲板
路径直接执行，不创建常驻进程。

测试后确认：

- 没有残留 `/tmp/boompi-client-final-ea46a27d` 进程或文件；
- capture/playback PCM 均为 `closed`；
- reverse tunnel 的板端 27806 端口已释放；
- VM 的临时 test server、PID 文件和 HIL test binary 已删除；上述哈希锁定的最终 client
  证据制品单独保留；
- `rkipc` PID 521 始终保留，没有运行 MPI HIL；
- 板端临时 client 文件已删除，可从 VM 和本机构建产物恢复；
- 没有修改镜像、设备树、启动项、mixer 或 `/userdata` 程序。

板卡掉电后墙钟回到 2021 年，因此本记录不使用板端 wall clock 作为证据时间；时间同步
仍是后续 CA/hostname TLS 测试的阻断项。SPKI pin 模式本轮不依赖证书有效期，但仍验证
serverAuth、精确公钥 pin 和服务器私钥持有。

## 最终回归结果

```text
Debug CTest:        21/21 passed
ASan/UBSan CTest:   21/21 passed
Go test ./...:      passed
Go vet ./...:       passed
Go race:            app/protocol/session/transport passed
offline scripts:    63/63 passed
RV1106 Release:     strict cross build passed
RV1106 manual HIL:  client PASS + server PASS
```

跨语言 3 秒测试和真板外部 HIL 均使用 fake backend。保存的业务摘要只包含帧数、字节数、
commit 数和状态；没有保存 device token、完整 UUID、证书私钥、用户语音、PCM 内容或
完整对话文本。manual-single-turn 层自有的 capture、decimator、uplink、pending、输入帧和
输出对象会被显式清零；renderer 内部 scratch 与 transport 临时分配尚未证明 secure-zero。

## 尚未覆盖

- 48 kHz capture/playback 同时运行的真全双工与重叠时长
- slot 0 非静音与信号质量，以及第二只麦克风、双麦 slot/极性、通道间幅相和最终壳体声学
- rk_mpi AI/AO、Rockchip VQE、`librkaudio` 3A 与 AEC reference
- VAD、Snowboy、3 秒 follow-up 和任意语音打断 TTS
- 正常 LAN discovery/pairing、Wi-Fi、断线重连和 half-open
- 长时压力、CPU/RSS、xrun/device-loss 故障注入和量产声学指标

下一阶段仍应遵守 vendor backend 优先：先关闭 48 kHz 真全双工、实际通道布局和
Rockchip 3A 输入契约，再按实测结果拆分生产线程或模块。
