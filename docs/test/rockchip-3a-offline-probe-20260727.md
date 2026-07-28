# Rockchip 3A 离线 ABI 探针记录（2026-07-27）

## 结论

匹配 BSP commit `994243753789` 的 Rockchip 3A 头文件、共享库、交叉工具链与
板端库已经对齐。Debug-only 离线探针在 RV1106 上完成了两轮纯内存全零输入的
初始化、错误长度、正确长度、销毁和重新初始化检查，结果通过。

本次结果同时暴露了一个不可绕过的契约阻断：vendor 每次固定处理 16 ms，现有
`AudioDspEngine` 每次固定消费并同步产出 20 ms。未修改公共契约前，不能通过补零、
丢样、重复样本或错误 metadata 把两者伪装成一对一，因此本轮没有实现生产 adapter。
默认 host 和 RV1106 Release 继续使用失败关闭实现。

## 测试条件与安全边界

- 主机记录时间：`2026-07-27T17:01:48+08:00`。
- 目标板：boomPI 自研 RV1106 板，`armv7l`。
- 板卡硬件修订：未记录。
- 目标镜像：Buildroot `2023.02.6`，镜像版本
  `-g712a500de-dirty`，内核 `5.10.160`。
- 板端时钟仍显示 `2021-01-03`，未完成校时，因此本记录使用主机时间戳。
- 可执行文件只复制到板端 `/tmp`，输入是进程内生成的全零 PCM。
- 没有打开 ALSA PCM、录音或播放，没有读取用户录音，没有修改 mixer、DTS、镜像、
  分区、启动项或持久配置。
- 探针不安装到 rootfs，不进入默认构建，不进入 Release，也不提交任何 vendor
  头文件、库、配置、模型或构建产物。

## 固定输入与板端一致性

本次继续使用 P0 已记录的外部候选。pinned header 来自匹配 BSP；板端
`/oem/usr/lib` 三个共享库的 SHA-256 与匹配 BSP 输出完全一致：

| 文件 | 来源/核对位置 | SHA-256 |
| --- | --- | --- |
| `rkaudio_preprocess.h` | 匹配 BSP 输出 | `b9bbf723d8e5bfdc421cf45fdf5853fec1584737d0e20682cd0db6bae5a7b54d` |
| `libaec_bf_process.so` | 匹配 BSP 与板端 `/oem/usr/lib` | `5abbcf518ffa39900dd78352547ebf5feab83d2f9b30a82c1e2dc1dc44b25e07` |
| `librkaudio_common.so` | 匹配 BSP 与板端 `/oem/usr/lib` | `4f4c9d78028a592174c3e959e35d231317d0ed2a864a1ed230f8adab42960246` |
| `librkaudio_detect.so` | 匹配 BSP 与板端 `/oem/usr/lib` | `f84b66a2d1d561fbb3c36e288a57f1e9ef50990974d9be9accbb0aaebcbae396` |

板端目标路径没有找到 `config_aivqe.json`。当前 probe 使用头文件定义的
`RKAUDIOParam` 及子结构体配置，不向 preprocess API 传 JSON 路径；这只证明本次
struct-param 初始化路径不依赖板端 JSON，不能外推其他 Rockchip 集成模式也不需要该
文件。`librkaudio_detect.so` 和 JSON 仍是现有 feasibility pin，但本次 probe 的
`NEEDED` 不包含 detect，运行时也没有加载 detect 或 JSON。

## 实际验证结果

| 检查 | 结果 | 边界 |
| --- | --- | --- |
| Host CMake configure/build | 通过 | vendor 默认关闭，probe 不进入 host target |
| Host CTest | `11/11` 通过 | 不加载 Rockchip 库，不替代 HIL |
| audio vendor CMake fixture | `10/10` 通过 | 覆盖默认关闭、目标/Debug 闸门、pin 与路径脱敏 |
| `boompi_rockchip_3a_probe` 交叉 target | 通过 | matching BSP GCC 8.3.0/uClibc，显式 Debug target |
| probe 严格警告直接编译 | 通过 | `-Wall -Wextra -Wpedantic -Werror` |
| ELF header/dependency/path 检查 | 通过 | ARM EABI5 hard-float、uClibc、无 `RPATH/RUNPATH` |
| 板端 `/tmp` 运行 | exit `0` | 两轮全零内存输入，未打开 PCM |
| `git diff --check` | 通过 | 无空白错误 |

本轮没有运行 sanitizer、真实录音 fixture、连续压力、CPU/RSS、最坏帧耗时或声学
验收；这些项目均为未验证。

## 已关闭的 API 与 ABI 事实

匹配头文件、同 BSP 的《Rockchip 麦克风阵列音频算法调试说明文档》、导出符号、
反汇编和板端返回值给出一致结论：

- 核心入口为 `rkaudio_preprocess_init`、`rkaudio_preprocess_short` 和拼写保持原样的
  `rkaudio_preprocess_destory`；初始化成功返回非空 handle，失败返回 `NULL`。
- PCM 位深只按 16-bit 接入。vendor 指南列出多个采样率，但本次只实际执行 16 kHz；
  不能据此宣称 48 kHz 功能或实时率已经通过。
- 一次调用固定为 16 ms。16 kHz 时每通道 256 samples。
- 输入是按 sample frame 交织的 signed 16-bit PCM。mic/reference 的逻辑位置由
  `REF_POSITION`/`pos` 表达，0 表示 reference 在 mic 前，1 表示在 mic 后；可选
  `Array_list` 用于重排。实际 ALSA Mode1 物理 slot、极性和 reference 采样点仍未验证。
- `input_size` 的单位是所有 source 与 reference 通道合计的 `int16` sample 数，不是
  bytes，也不是单通道 samples。本次 `2 mic + 2 ref` 的正确值是
  `256 * 4 = 1024`。
- `rkaudio_preprocess_short` 的成功返回值是 mono 输出 byte 数。16 kHz/16 ms 的
  期望值为 `256 * 2 = 512`。错误输入 1023 返回 0；未来 bridge 必须只接受精确的
  512，不能把任意非负值都当成功。其他负错误值语义仍没有 enum 或完整文档。
- 头文件和导出符号没有独立 reset API。vendor 指南要求结束时依次调用
  `rkaudio_preprocess_destory` 与 `rkaudio_param_deinit`；本次销毁后重新初始化已通过。
  `rkaudio_param_set` 只证明存在参数更新入口，不能把它解释为历史状态 reset。
- probe 使用 `RKAUDIO_EN_AEC | RKAUDIO_EN_BF`，BF 子模块选择固定波束、ANR 和 AGC；
  全零输入只能验证调用、尺寸和内存边界，不能验证这些算法的实际效果。

探针 ELF 为 ELF32 ARM、EABI5 hard-float，解释器为 `/lib/ld-uClibc.so.0`，无
`RPATH/RUNPATH`。直接依赖包含 `libaec_bf_process.so` 和
`librkaudio_common.so`，不包含 `librkaudio_detect.so`。

板端脱敏结果为：

```json
{"schema_version":1,"probe":"rockchip_3a_offline","sample_rate_hz":16000,"bits_per_sample":16,"source_channels":2,"reference_channels":2,"frame_samples_per_channel":256,"input_samples_total":1024,"invalid_input_result":0,"valid_result_bytes":512,"wakeup_status":0,"guards_intact":true,"reinitialize_ok":true,"status":"passed"}
```

## 16 ms 与 20 ms 阻断

80 ms 内 vendor 需要处理 5 个 16 ms 块，而核心层表示为 4 个 20 ms 帧。现有接口要求
每次 `Process` 成功时同步输出一个 20 ms frame，并复制当前输入 metadata；它没有
“已消费但暂时无完整输出”或“输出属于更早缓存帧”的表达能力。

因此以下做法均禁止：

- 每 20 ms 尾部补 64 个零再调用两次 vendor。
- 为凑长度丢弃、重复或跨 generation 复用样本。
- 输出缓存中的旧 PCM，却复制当前调用的 sequence/timestamp/turn metadata。
- 把原始麦克风一路复制成处理后 mono 作为临时 fallback。

后续公共契约评审至少需要表达非错误的“需要更多输入”、缓存中最早 20 ms 输入的
metadata 归属，以及 Arm、Disarm、continuity fault 和 backend fault 时清空输入、输出
与 metadata 队列。方案获批并有确定性 host 测试前，不实现真实 adapter。

## 验证入口

Debug feasibility 构建必须继续提供全部显式外部路径和固定 SHA-256：

```text
cmake --preset rv1106-debug \
  -DBOOMPI_ENABLE_ROCKCHIP_3A=ON \
  -DBOOMPI_ALLOW_FEASIBILITY_AUDIO_VENDOR_INPUTS=ON \
  -DBOOMPI_ROCKCHIP_3A_INCLUDE_DIR=<PINNED_INCLUDE_DIR> \
  -DBOOMPI_ROCKCHIP_3A_AEC_LIBRARY=<PINNED_AEC_LIBRARY> \
  -DBOOMPI_ROCKCHIP_3A_COMMON_LIBRARY=<PINNED_COMMON_LIBRARY> \
  -DBOOMPI_ROCKCHIP_3A_DETECT_LIBRARY=<PINNED_DETECT_LIBRARY> \
  -DBOOMPI_ROCKCHIP_3A_CONFIG_FILE=<PINNED_CONFIG_FILE>
cmake --build --preset rv1106-debug \
  --target boompi_rockchip_3a_probe --parallel
```

部署副本只用于受控 `/tmp` 探测：

```text
LD_LIBRARY_PATH=/oem/usr/lib /tmp/boompi-rockchip-3a-probe
```

probe 的 JSON 不输出 SDK 路径、网络标识、设备标识或音频。vendor 自身会输出固定参数
诊断，采集报告前仍应检查并脱敏。

## 仍未验证

- 真实双麦与 reference 的板端 packing、物理 slot、极性、时钟和延迟。
- 最终声学参数、波束方向、AEC/ANR/AGC 实际效果、ERLE、残余回声和 double-talk。
- 连续非零音频、最坏帧耗时、CPU、RSS、持续实时率和故障后的恢复策略。
- 20 ms/16 ms 缓冲契约、metadata 对齐、generation 清理和完整 adapter。
- `config_aivqe.json` 的其他运行模式、打包位置和再分发范围。
