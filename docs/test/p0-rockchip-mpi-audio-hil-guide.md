# P0 Rockchip MPI 原始音频 HIL 指南

本指南对应显式构建的 `boompi_rockchip_mpi_audio_hil`。它只用于关闭当前 RV1106
`rk_mpi_ai`/`rk_mpi_ao` 的原始传输事实，不是生产音频 backend，也不经过现有
playback/control/committer/worker。当前只完成实现、离线回归和交叉链接验证，尚未在板端运行
ARM ELF；**本地探针结果也不等于完整 HIL，更没有证明 raw MPI 全双工、双麦布局或可听播放**。

## 构建和自动执行边界

`BOOMPI_BUILD_ROCKCHIP_MPI_AUDIO_HIL` 默认 `OFF`。只有同时显式开启 pinned MPI feasibility
依赖、Debug RV1106 交叉环境和该开关时，CMake 才创建 `EXCLUDE_FROM_ALL` target：

```text
BOOMPI_TARGET_RV1106=ON
BOOMPI_ALLOW_FEASIBILITY_AUDIO_VENDOR_INPUTS=ON
BOOMPI_ENABLE_ROCKCHIP_MPI_AUDIO=ON
BOOMPI_BUILD_ROCKCHIP_MPI_AUDIO_HIL=ON
```

配置仍必须提供现有文档列出的匹配工具链、uClibc sysroot、八个 pinned header 和
Rockit/MPP/RGA 库。默认 `cmake --build`、CTest、install、post-build 和 supervisor 都不会构建
或运行 HIL；只能显式执行：

```sh
cmake --build <rv1106-debug-build> --target boompi_rockchip_mpi_audio_hil
```

target 不写 RPATH/RUNPATH，不安装，也不进入发布包。构建成功只证明 pinned ABI/链接闭包，
不证明当前镜像中的设备、权限、时钟或 Codec 工作。

## 首轮固定传输契约

- AI/AO 都请求 `48 kHz / vendor AUDIO_BIT_WIDTH_16 / stereo`；这时还不能把 vendor enum
  写成已经验证的 `S16_LE`。
- sound-card channel、`u32ChnCnt` 均为 2；`u32FrmNum=4`，`u32PtNumPerFrm=1024`。
- AI frame depth 为 4；AI/AO loopback 都固定 `AUDIO_LOOPBACK_NONE`。
- 不启用 resample、VQE、AEC frame、track mode、volume、AMIX、`SYS_Bind`、AENC 或 ADEC。
- AO 只发送 1024-byte 全零块，共 750 个成功 payload，合计 768000 bytes、名义 4 秒。
  1024 bytes 沿用当前 vendor 样例的保守 chunk，不等于已经证明一个 hardware period 的大小。
- AI 使用 100 ms 正 timeout 连续取帧 6 秒；AO SendFrame 也只用 100 ms 正 timeout。
- 两个 worker 通过同一个 start gate 启动。共同起点后的活动按固定 100 ms bucket 记账；只有
  **至少 30 个连续 clean common bucket**，即连续至少 3 秒每个 bucket 同时有 AI、AO 完整成功且
  两侧都没有错误，transport 的时间重叠条件才成立。
- 正常 payload 结束后只尝试一次零长 EOS MB/Send，随后用 500 ms 正 timeout WaitEos；
  当前 runtime 如果干净地拒绝 `CreateMB(0)`，EOS status 记为 `unsupported`，整体 probe status
  只能是 `inconclusive`，不能伪造成功。

AI bucket 只有在 GetFrame 成功、handle/虚拟地址/`RK_MPI_MB_GetSize`/frame metadata 全部有效，且
ReleaseFrame 也成功后才标为成功；代码不解引用、复制、hash 或保存 PCM。每个成功 GetFrame
无论后续判断如何都恰好 ReleaseFrame 一次，AI 的 MB 绝不调用 `MB_ReleaseMB`。AO bucket 只有在
CreateMB → SendFrame → ReleaseMB 三步全部成功后才标为成功；Send 失败也必须释放有效的
caller-owned MB。AI/AO payload 路径的任一 vendor 调用错误、无效 metadata/handle，或成功/错误
事件落到预定义 bucket 范围之外，都会让 transport fail closed，不能用其他 bucket 的成功抵消。
初始化、EOS 和 cleanup 错误分别由自身 facet fail closed，并阻止整体 probe 通过。

线程全部结束后，主线程才按 AO channel/device、AI channel/device、SYS 的顺序 best-effort 清理。
signal handler 只设置 `sig_atomic_t` 标记；探针在 cleanup 完成后再次把 signal 状态合并进报告，
再计算 facet，因此清理期间到达的 signal 也不能被误记成通过。

## Dry-run 与显式执行

card、MPI device/channel 和 artifact 目录没有默认猜测值，必须来自当前板只读探测。以下命令
只校验参数和打印固定计划；不创建目录、不扫描 PCM 占用，也不调用任何 MPI API：

```sh
boompi_rockchip_mpi_audio_hil \
  --ai-card hw:<card>,<capture-device> \
  --ao-card hw:<card>,<playback-device> \
  --ai-device <mpi-device> --ai-channel <mpi-channel> \
  --ao-device <mpi-device> --ao-channel <mpi-channel> \
  --artifact-dir /tmp/boompi-rkmpi-hil-<host-timestamp>
```

真实执行还必须在同一命令同时追加：

```text
--execute --allow-ai-capture --allow-ao-playback
```

artifact 必须是尚不存在的绝对目录，并使用 owner-only 权限。实现先打开已经规范化且非符号链接
的 parent dirfd，再用 `mkdirat`/`openat` 创建并验证目录；新目录 fd 由 `ScopedFd` 持有直至结果
落盘和显式关闭。`result.json` 的临时文件创建、失败清理和原子发布都相对这个 dirfd 完成：命名空间
操作只使用 `openat`/`unlinkat`/`renameat`，并分别 `fsync` 文件和目录，避免重新按可变路径查找。

执行模式在首次 MPI 调用前对 `/proc/<pid>/fd` 连做两次只读扫描。它们只是
**snapshot-only、non-exclusive** 的快照，既不持锁，也不能排除扫描后另一个进程打开 PCM 的竞态；
扫描只报告冲突，绝不停止服务或杀进程。因此完整板端 HIL 的外层执行器必须先停止或锁定所有相关
音频服务，并在探针整个生命周期内保持排他。任一扫描不完整或发现占用都 fail closed。全零 payload
也不保证模拟链路绝对无 pop，执行前仍要有人值守并确认功放状态安全。

## 有界性的真实边界

GetFrame/SendFrame/WaitEos 都使用有限正 timeout，普通错误也没有无限 retry。这个边界不能约束
没有 timeout 参数的 SYS、SetPubAttr、Enable、Disable，以及驱动进入内核 D-state 的情况。
板端实际运行必须由主机/SSH 对**本探针 PID**设置约 20 秒的外层 watchdog；超时后不得并发拆除
活线程下面的 MPI 资源，也不得使用 `killall`。应停止本轮、检查残留进程和设备状态，必要时重启
板卡，再继续任何音频测试。

外层 watchdog、音频服务排他和连续 dmesg delta 都不是这个 ELF 自己能够提供的能力。即使
`result.json` 的 `probe_status` 和 `transport.status` 为 `pass`，它仍只是一次本地 raw MPI 探针
结果；缺少上述任一外层证据时，完整 HIL 必须保持 `inconclusive`，不得登记为板端通过。

## JSON facet 与禁止推导的结论

最终 `result.json` 在所有 worker join 和 cleanup 完成后，通过同目录临时文件原子改名发布。
它分别记录：

- pinned 输入集合和本 TU 的 SHA-256；
- 所有初始化/清理调用的 attempted、signed rc 和 hex rc；
- AI Get/Release 数量、非零/EOS frame、`u32Len`/MB capacity 范围与关系分类；
- AO Create/Send/Release、成功 payload bytes、EOS 和 drain；
- 两线程 ready/start/done/join、首末成功时间、100 ms 成功/错误 bucket 和最长连续 clean common
  bucket；
- `transport`、`eos`、`cleanup` facet，以及明确为 false 的能力声明。

HIL 本身不采集 dmesg。完整板端 gate 必须在外层 20 秒 watchdog 和音频服务排他约束下运行，
取得可连续比较的 dmesg 前后快照，并确认没有新增 xrun/underrun/overrun；dmesg 不可读、发生
ring wrap 或无法证明快照连续时只能记为 `inconclusive`。完整 dmesg 可能含 MAC、序列号和内核
命令行，不得提交 Git 或公开上传。

即使 transport facet 通过，也只能说明当前 runtime 在有限时间内同时接受 raw AI Get 和 AO
Send。以下结论仍全部为未验证：精确 S16_LE 字节序、`u32Len` 是每通道还是总字节、MB padding、
左右交织、两个 slot 是否都是麦克风、数字 reference、极性、AI/AO 时钟漂移、真实 presentation
和可听扬声器输出。它们必须由后续单变量、可辨识信号和通道相关性 HIL 分别关闭。
