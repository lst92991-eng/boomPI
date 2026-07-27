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

Snowboy 基线来源是 Kitt-AI/snowboy commit `c9ff036e2ef3`。上游仓库许可证文本覆盖
源码、运行库、资源和默认 `snowboy.umdl`；其他个人或通用模型许可必须单独核对，
不能从默认模型外推。本次 OpenBLAS 仍是多线程 feasibility archive，只能用于 P0
探针；发布输入必须按单线程配置重建、重新固定 SHA-256 并重新验证许可和性能。

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

## 下一步决策

在保持“第一版使用 Snowboy”和“默认单进程”的现有产品约束下，推荐优先获取或重建
一个与 RV1106 匹配、错误可返回/可捕获且使用单线程 OpenBLAS 的 Snowboy runtime，
然后重复缺失、损坏、权限、reset、连续 PCM 和真实麦克风测试。

如果无法得到安全的同进程 runtime，需要用户明确选择：

1. 批准 Snowboy 进程隔离并更新单进程架构；或
2. 批准更换唤醒引擎。

在作出决定前，当前代码和证据只能作为 P0 feasibility adapter/probe，产品 capability
必须保持 unavailable，不能静默降级、假装唤醒已接通或进入稳定性压力测试。
