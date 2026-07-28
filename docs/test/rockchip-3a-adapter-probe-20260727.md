# Rockchip 3A 可行性平台适配器离线探针记录（2026-07-27）

## 结论

`Rockchip3aAudioDspAdapter` 已在不修改冻结 `AudioDspEngine` 的前提下接入
`AudioDspFrameBridge16k`。Host Debug/Release 测试证明 20 ms 输入与 vendor 固定
16 ms 块之间不存在补零、丢样、重复样本或 metadata 错配；匹配 BSP 的真实
`rkaudio_preprocess` session 已完成交叉链接，并在目标 RV1106 的 `/tmp` 用全零内存
输入执行两代初始化、处理、销毁和重新初始化，退出码为 0。

本记录只证明适配器调用节奏、固定布局、长度检查、generation 清理和 vendor ABI 可用。
它不证明 Mode1 物理通道顺序、双麦极性、reference 采样点、AEC/ANR/BF/AGC 声学效果、
算法内部顺序或持续实时率。当前 vendor pins 与算法参数仍是 Debug-only feasibility
候选，不能进入 Release 发布物。

## 测试条件与安全边界

- 主机采集时间：`2026-07-27T19:37:16+08:00`。
- 目标板：boomPI 自研 RV1106，`armv7l`。
- 目标镜像：Buildroot `2023.02.6`，版本 `-g712a500de-dirty`，内核 `5.10.160`。
- BSP 基线：commit `994243753789` 的 pinned header、AEC/BF 与 common 库。
- 交叉编译器：`arm-rockchip830-linux-uclibcgnueabihf-g++` 8.3.0。
- 探针只复制到 `/tmp`，输入由进程内固定数组生成并保持全零。
- 没有打开 ALSA PCM、采集或播放音频，没有修改 mixer、DTS、镜像、分区、启动项或
  持久配置。
- 没有提交 vendor header、共享库、配置、音频或构建产物。

## 已实现边界

- adapter 固定接收四个 16 kHz 逻辑平面：`MIC-L`、`MIC-R`、`REF-L`、`REF-R`。
- 每个 16 ms vendor 块按 sample frame 交织为上述固定顺序，共 `256 * 4 = 1024`
  个 `int16`；实际 ALSA slot 到逻辑平面的映射仍由上游 `ChannelMap` 和 HIL 负责。
- vendor 只有精确返回 512 bytes 时才映射为 256 mono samples；0、短返回、长返回或
  其他异常结果全部映射为 backend failure。
- `Arm` 先让 bridge 接受严格递增 epoch，再执行 `destory`、参数释放和重新初始化。
  初始化失败会 Disarm，恢复必须使用更新 generation。
- backend failure、discontinuity 或 bridge overflow 会清空 PCM/metadata 并销毁 vendor
  历史；旧 epoch/stream 输入不会调用 vendor，也不会破坏当前 generation。
- boomPI adapter 和 bridge 自身的每帧热路径只使用定长成员数组，不执行动态分配、
  日志、文件或网络 I/O；vendor 初始化参数的 boomPI 侧分配只发生在冷控制路径。
  预编译 vendor `Process` 内部是否分配、加锁或执行 I/O 尚无证据。
- adapter 不推测或复述 vendor 内部算法顺序；当前固定参数只复用已通过 P0 零输入 ABI
  探针的结构体路径。

## 验证结果

| 检查 | 结果 | 边界 |
| --- | --- | --- |
| Host Debug | `18/18` 通过 | MSVC 严格告警；不加载 vendor 库 |
| Host Release | `18/18` 通过 | 默认 vendor 开关关闭 |
| adapter fake 长序列 | 通过 | 1,000 个 20 ms 输入 → 1,250 次 16 ms 调用 → 1,000 个输出 |
| vendor CMake fixture | `10/10` 通过 | 默认关闭、Debug-only、pin/path fail-closed |
| ELF verifier fixture | `7/7` 通过 | verifier 自身回归 |
| RV1106 Debug 全构建 | 通过 | 包含具体 preprocess session 编译与链接 |
| adapter probe target | 通过 | 显式 `EXCLUDE_FROM_ALL`、不安装 |
| 部署副本 ELF | 通过 | ELF32 ARM EABI5 hard-float、uClibc、无 RPATH/RUNPATH/开发机路径 |
| 板端 `/tmp` 零输入 | exit `0` | 两代各 4 个 20 ms 输入、5 次 vendor 调用、4 个输出 |

部署副本 SHA-256：

```text
18bcdd010ab6e4fef0a9bf23a4195cdb37ae6b6724396b5788a4a42860aef8d9
```

板端脱敏结果：

```json
{"schema_version":1,"probe":"rockchip_3a_adapter","input_frames_per_run":4,"first_processed_blocks":5,"first_output_frames":4,"second_processed_blocks":5,"second_output_frames":4,"metadata_valid":true,"reinitialize_ok":true,"status":"passed"}
```

部署副本直接依赖 `libaec_bf_process.so`、`librkaudio_common.so`、`libstdc++.so.6`、
`libgcc_s.so.1` 和 `libc.so.0`。由于 probe 复用现有 `boompi_platform_rv1106` 静态 target，
链接行也保留 `libasound.so.2`，但本探针没有创建 ALSA 对象或打开 PCM。ELF 不依赖
`librkaudio_detect.so`。

## 仍未验证

- 真实 Mode1 四通道物理顺序、双麦极性和 DAC/software reference 的实际位置与延迟。
- vendor 完整错误域；当前头文件没有错误 enum，实测只关闭了精确成功长度与 0 返回。
- 预编译 vendor `Process` 内部是否分配内存、加锁或执行 I/O，以及它的最坏调用耗时。
- 真实非零双麦/reference 连续输入、mono 内容、单块最坏耗时、CPU、RSS 和持续实时率。
- 最终声学参数、波束方向、ERLE、残余回声、double-talk 与 AGC/ANR 主观效果。
- 产品 DSP worker、reference producer/reset ACK、VAD/Snowboy 和 TTS 打断组合闭环。
- vendor 二进制、配置与参数的许可/再分发和 Release 发布批准。
