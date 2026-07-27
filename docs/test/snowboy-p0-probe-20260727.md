# Snowboy P0 板端探针记录（2026-07-27）

## 结论

Snowboy 候选库的正常路径在 RV1106 板端通过了短时离线探针：

- 默认 `snowboy.umdl` 与 `common.res` 可以加载。
- 运行时报告输入格式为 16 kHz、mono、S16，模型包含一个 keyword。
- 50 个 20 ms 全零帧均返回 `-2`；这里只记录为 Snowboy diagnostic
  silence，没有把它当成产品 VAD。
- 上游 `snowboy.wav` 离线 fixture 被分成 47 个 20 ms 帧后，返回一次
  keyword 1 检测。
- `Reset()` 正常返回成功。

但是 P0 仍然是**阻塞**，不能把 Snowboy 标记为可进入同进程产品运行时。故意提供
不存在的模型文件时，旧静态库打印 `std::runtime_error` 后直接
`terminate`/`Aborted`；私有 C bridge 的 `catch (...)` 没有获得控制权。缺失、损坏或
不可读资源因此仍可能终止整个 `boompi-client`，不满足失败关闭要求。

二次审计进一步确认：这不是 bridge 少写了一个 `catch`，也不是 OpenBLAS 线程配置
导致的。该公开上游仓库的可达 Git 历史没有提供能够生成 detector archive 的 detector、
pipeline 和 fatal-log 核心实现，因此无法从这份公开源码为 RV1106 重建一个错误可返回
的同进程 Snowboy runtime。当前可审计的最小决策是保持产品 capability unavailable，
或者由用户批准把**同一个 Snowboy 引擎**放入受监管的独立 helper 进程；不能通过全局
`terminate_handler`、吞异常、二进制
打补丁或只做文件预检查来伪装同进程安全。

本记录不宣称真实麦克风唤醒、准确率、误唤醒率、实时调度或稳定性已经通过。

## 时间、板卡与测试边界

- 开发机采集时间：`2026-07-27 17:41:38 +08:00`。
- 板端 RTC 显示 `2021-01-03`，未校时，因此本记录以开发机时间戳为准。
- 板卡：用户自研 RV1106 板。
- 内核：Linux `5.10.160`，ARMv7 Cortex-A7。
- 测试目录：板端 `/tmp/boompi-snowboy.*` 临时目录；测试结束后已删除。
- 未打开 ALSA、未修改 mixer、未录音、未播放、未修改设备树、启动项、分区或镜像。
- 仅做模型加载、离线 PCM、reset 和一个受控错误路径；没有进行压力测试。

## ABI 与依赖

目标环境实测：

- ARMv7-A/Cortex-A7，VFPv4/NEON，EABI5 hard-float。
- uClibc-ng `1.0.31`，动态加载器 `/lib/ld-uClibc.so.0`。
- libstdc++ `6.0.25`，目标上限 `GLIBCXX_3.4.25`。
- 交叉编译器：与 BSP 匹配的 GCC `8.3.0` Buildroot wrapper。

外部候选材料均未提交到 Git：

| 文件 | SHA-256 |
| --- | --- |
| `snowboy-detect.h` | `f203e88bccd3782b9fdfaa5f02ea2fab402671f415c9eae3609b67c1e622a363` |
| `libsnowboy-detect.a` | `346db1193490a9cc404d49fcfb22ca612cd3a0e649c4863f411553eb1c4f9f1f` |
| `common.res` | `5dd5258678182f2e055fa7a6167eba50ded3bf8b41f70faab11fd9b221de488b` |
| 默认 `snowboy.umdl` | `7ccc61effbe05c27d8fd3428bf27e71578d2eddcc97ac9c1437fa0f9cacc64f1` |
| OpenBLAS `libopenblas.a` | `fabfc588e0e0d94f3655d4ad5515e0c90fd161f016be5261e2f11d3df77a3e9d` |

Snowboy 基线来源是 Kitt-AI/snowboy commit `c9ff036e2ef3`。上游仓库许可证文本明确
覆盖源码、运行库、资源和默认 `snowboy.umdl`，许可证为 Apache-2.0；其他个人或通用
模型许可必须单独核对，不能从默认模型外推。Apache-2.0 允许使用和修改已提供的对象
形式，但不代表仓库实际提供了构建这些对象的首选源码。若未来分发 archive、资源或
默认模型，仍必须履行许可证和适用的 NOTICE/归属要求，并完成本项目自己的 packaging
review。

原记录把既有 OpenBLAS feasibility archive 描述成“多线程”，二次符号审计没有找到
未解析的 `pthread_*` 或 `blas_thread_server`，因此该描述没有充分证据。为了不依赖这项
历史假设，本次从锁定的 OpenBLAS commit
`1bd74ad3d1e8d21f86d1a6be35abfcdf27c0208a` 独立执行了显式单线程交叉构建：

```sh
git -C <CLEAN_OPENBLAS_WORK_COPY> rev-parse HEAD
git -C <CLEAN_OPENBLAS_WORK_COPY> status --short
make -C <CLEAN_OPENBLAS_WORK_COPY> -j<N> \
  TARGET=ARMV7 BINARY=32 CROSS=1 CROSS_SUFFIX=<RV1106_PREFIX> \
  CC=<RV1106_PREFIX>gcc AR=<RV1106_PREFIX>ar \
  RANLIB=<RV1106_PREFIX>ranlib HOSTCC=gcc \
  NOFORTRAN=1 NO_LAPACK=1 ONLY_CBLAS=1 \
  USE_THREAD=0 NUM_THREADS=1 NO_SHARED=1 DYNAMIC_ARCH=0
sha256sum <OPENBLAS_ARCHIVE>
<TARGET_NM> -u <OPENBLAS_ARCHIVE>
<TARGET_NM> -C <OPENBLAS_ARCHIVE>
<TARGET_AR> p <OPENBLAS_ARCHIVE> openblas_get_parallel.o \
  > <TEMP_DIR>/openblas_get_parallel.o
<TARGET_OBJDUMP> -dr <TEMP_DIR>/openblas_get_parallel.o
```

执行前 `git status --short` 无输出。生成 archive 为 3,003,736 bytes，SHA-256 为
`efaab6185a413a57307a588e269758afe970f4be8c7ead78c603c7d73172a01a`；没有未解析的
`pthread_*`，没有 `blas_thread_server`，`openblas_get_parallel()` 的 ARM 指令直接返回
`0`。该临时 archive 未提交，也没有自动替换仓库中的 feasibility pin；它只证明
RV1106 单线程 OpenBLAS 可以重建，不能解决 Snowboy 自身 fatal destructor 的终止行为。

候选 Snowboy archive 使用旧 libstdc++ 字符串 ABI。构建结果确认
`_GLIBCXX_USE_CXX11_ABI=0` 只作用于私有 legacy bridge translation unit，没有扩散到
adapter、`boompi_audio_core` 或应用 target。

## 构建与 Host 验证

默认 Host 构建不读取 Snowboy 路径，也不链接 Snowboy。adapter 的结果映射使用 fake
C bridge 做确定性测试，覆盖：

- 配置、格式和 keyword count 校验。
- `-2` silence、`0` no event、合法 keyword index。
- `-1`、未知 keyword、bridge exception 和 reset failure。
- backend error fault 锁存以及成功 reset 后恢复。
- 无 score 时固定 `score_available=false`、`score_milli=0`。
- 非 16 kHz/20 ms/mono/S16/320-sample 帧在调用 backend 前拒绝。

Host CTest：`12/12` 通过。

交叉链接后的 strip 部署副本通过 ELF 校验：

- ELF32、ARM、little-endian、EABI5、hard-float。
- interpreter 为 `/lib/ld-uClibc.so.0`。
- 最高需要 `GLIBCXX_3.4.20`，不超过板端 `GLIBCXX_3.4.25`。
- 不依赖 `libc.so.6`，没有 RPATH/RUNPATH，没有开发机绝对路径。

使用上述显式单线程 OpenBLAS 重新交叉链接的 probe 也通过相同 ELF verifier：ELF32、
ARM EABI5 hard-float、uClibc loader、最高 `GLIBCXX_3.4.20`，且无 RPATH/RUNPATH 和
开发机绝对路径。尝试进行新一轮板端验证时，目标板 SSH 超时，因此本记录没有把这个
新链接产物写成新的板端通过结果；前文正常模型和缺模型结果仍来自已经完成的真实板端
测试。

独立探针是 Debug-only、`EXCLUDE_FROM_ALL` target，不安装到产品路径。Release 配置和
未显式 opt-in 的构建继续由 vendor CMake gate 拒绝。

## 板端正常路径证据

### 全零 PCM

输入为 50 个 20 ms、16 kHz、mono、S16 全零帧：

| 指标 | 结果 |
| --- | ---: |
| create | 成功 |
| create elapsed | 16,431 us |
| process total | 12,221 us |
| process max | 1,473 us |
| silence (`-2`) | 50 |
| no event | 0 |
| detection | 0 |
| reset | 成功 |
| 最大 RSS | 3,068 KiB |

### 上游英文唤醒 fixture

上游 `resources/snowboy.wav` 本身是 16 kHz、mono、S16。测试时只在临时目录转为
little-endian raw PCM，并在末尾补齐到 47 个固定 20 ms 帧；临时 raw fixture
SHA-256 为
`f1b92472d01bd5fd01d28ecfe71e39249d81201d527c3baaada9524cf8687051`。
该音频和转换产物均未提交到 Git。

| 指标 | 结果 |
| --- | ---: |
| create | 成功 |
| create elapsed | 19,083 us |
| frames | 47 |
| process total | 148,286 us |
| process max | 7,197 us |
| silence (`-2`) | 20 |
| no event | 26 |
| keyword 1 detection | 1 |
| reset | 成功 |
| 最大 RSS | 3,180 KiB |

上述 `process max` 来自不做 20 ms pacing 的离线 burst，只证明单次调用在本次短测中
低于 20 ms，不是调度后的最坏帧耗时，也不能替代 30 分钟稳定性测试。

## 真实错误路径阻断

受控测试把 model 参数指向不存在的文件。结果：

```text
Snowboy reports input-file failure
terminate called after throwing std::runtime_error
Aborted
```

进程没有返回 adapter 定义的 create error。即使 bridge 用
`_GLIBCXX_USE_CXX11_ABI=0` 编译并在构造调用外包围 `catch (...)`，异常仍未被捕获。
这表明当前预编译 archive 的错误展开行为不能作为安全的同进程边界。

启动前检查存在性或 SHA-256 可以减少资源错误，但不能证明损坏模型、权限变化或内部
运行时错误不会走同类终止路径，因此不能用预检查把该闸门标为通过。

### `terminate` 根因复核

本次对 commit `c9ff036e2ef3` 做了 shallow clone 之外的可达历史审计：共 387 个提交，
标签覆盖 `1.0.0` 到 `v1.3.0`。历史中出现过同名的
`swig/Node/snowboy-detect.cc`，但它是 SWIG/Node wrapper/generated glue，不是生成
`libsnowboy-detect.a` 的 detector 核心。当前树和可达历史都没有公开
`snowboy-debug.cc`、`pipeline-detect.cc` 以及能够重建 archive 的等价核心实现；仓库只
提供公共头文件、语言 wrapper、示例和各平台预编译 archive。因此不存在可以应用异常
补丁并从该公开仓库重新编译 detector 的源码输入；这个结论不排除原作者或供应方另有
未公开源码。

可复现审计使用以下只读命令；所有路径均由执行者显式提供：

```sh
git -C <SNOWBOY_REPO> rev-parse HEAD
git -C <SNOWBOY_REPO> rev-list --all --count
git -C <SNOWBOY_REPO> tag --list
git -C <SNOWBOY_REPO> log --all --name-only --format=
sha256sum <SNOWBOY_ARCHIVE>
<TARGET_AR> t <SNOWBOY_ARCHIVE>
<TARGET_AR> p <SNOWBOY_ARCHIVE> snowboy-debug.o \
  > <TEMP_DIR>/snowboy-debug.o
<TARGET_NM> -C <TEMP_DIR>/snowboy-debug.o
<TARGET_READELF> -SW <TEMP_DIR>/snowboy-debug.o
<TARGET_OBJDUMP> -drC <TEMP_DIR>/snowboy-debug.o
```

对锁定的 RPi archive 解包后确认：

- 对象由 GCC 4.9.3 生成，包含 ARM `.ARM.extab`/`.ARM.exidx` 展开信息和
  `__gxx_personality_v0`，所以根因不是“整个库关闭了异常展开”。
- `snowboy-debug.o` 中的 `snowboy::SnowboyLogMsg::~SnowboyLogMsg()` 同时引用
  `std::runtime_error`、`__cxa_throw` 和 `std::terminate()`。
- 反汇编显示该析构函数构造 `std::runtime_error` 后调用 `__cxa_throw`，其析构异常
  落地路径直接调用 `std::terminate()`；这与 C++11 隐式 `noexcept` 析构语义一致。
  缺失模型触发 fatal log 后正好进入这条路径。

因此异常在 Snowboy 内部析构边界已经终止，无法展开到
`boompi_snowboy_legacy_create()` 外层的 `catch (...)`。旧 C++ 字符串 ABI bridge 是链接
Snowboy 的必要边界，但调整 bridge ABI、重新编译 wrapper 或换成单线程 OpenBLAS 都不能
改变这个已固化在 archive 内的行为。替换单个对象或修改 ARM 指令还需要重建未公开的
私有类型布局、异常说明和调用点假设，无法形成可审计、可维护的产品 runtime，本项目
明确拒绝这种二进制补丁方案。

### 独立 helper 的最小成本（仅供决策，尚未实施）

如果用户批准进程隔离，第一版不需要改换引擎，也不需要把整个音频管线拆成多进程。
最小边界是只有 helper 拥有 `SnowboyDetect`；主客户端仍拥有 ALSA、3A、VAD、pre-roll、
对话状态机和网络：

- 控制面：版本握手、模型配置、`reset(epoch)`、shutdown 和结构化错误。
- 数据面：发送 AEC 后的 16 kHz/mono/S16 固定 20 ms 帧；每帧 640 bytes，实时带宽约
  32 KiB/s。若用户批准该架构，可优先评估有界 Unix-domain `SOCK_SEQPACKET`；后续
  只有实测表明复制成本不合格时才评估共享内存 SPSC。
- 事件面：只返回 keyword index、对应 frame sequence/epoch、错误和 heartbeat；不传
  `std::string`、C++ 对象、异常或指针。
- 故障语义：helper 退出时关闭通道、丢弃所有旧 epoch PCM、将唤醒 capability 标成
  unavailable；监管者限速重启，禁止 crash loop，也不得重放陈旧音频。
- 建议监管参数：500 ms heartbeat、1.5 s 无响应判死、60 s 内最多重启 3 次；这些值
  必须在板端实测后固化。
- 安全边界：helper 不访问网络、不执行 shell，只读打开经过 pin/权限校验的资源和模型，
  以最小权限运行。

在尚未批准、也没有完成 ADR 的前提下，粗略规划估算是新增一个很小的 C++ helper、IPC
协议、监管逻辑和测试，约 200–400 行产品代码加测试、0.5–1.5 个工作日；这个估算**不
包含** ADR 评审、板端 HIL 和 fault-injection 收敛时间。额外内存至少包括一个进程的
页表、栈和 Snowboy/OpenBLAS 私有页；现有短测 RSS 约 3 MiB，只能作为量级参考。IPC
新增一次调度/复制，若获批可把小于 1 ms 作为待测目标，但不能提前当作 SLA。

## 下一步决策

对该公开上游仓库可达历史的审计表明，该仓库不能重建错误可返回的 Snowboy 核心；
显式单线程 OpenBLAS 也不能改变 archive 内部的 fatal destructor。若能从原作者或可信
供应方取得完整、可重建且许可清晰的核心源码，才重新开启同进程方案，并重复缺失、
损坏、权限、reset、连续 PCM 和真实麦克风测试。

如果无法得到安全的同进程 runtime，需要用户明确选择：

1. 批准最小 Snowboy helper 进程隔离并更新单进程架构；或
2. 批准更换唤醒引擎。

在作出决定前，当前代码和证据只能作为 P0 feasibility adapter/probe，产品 capability
必须保持 unavailable，不能静默降级、假装唤醒已接通或进入稳定性压力测试。
