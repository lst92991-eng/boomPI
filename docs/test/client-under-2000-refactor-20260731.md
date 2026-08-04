# 板端客户端 2000 ELOC 收敛与启动记录（2026-07-31）

- 采集时间：2026-07-31 19:24:52 +08:00
- 板卡：第三块 RV1106，SSH 别名 `rv1106-board-3`
- 目标：保持现有语音闭环能力，把自写板端生产 C++ 压到 2000 ELOC 以下
- 结论：生产目标共 15 个 C++/头文件，1904 ELOC；严格交叉构建和板端启动通过，真人语音回归待执行

## 计数边界

ELOC 定义为非空且非纯 `//` 注释的 C++ 源码行，包含预处理行和结构行。计入
`boompi-client` 的全部自写 C++ 及其 Snowboy/Rockchip 薄适配；不计 CMake、文档、
vendor HIL 探针、Go 服务端、WebSocketpp/Boost/OpenSSL/Snowboy/Rockchip/WebRTC/FFmpeg/ALSA
第三方实现。没有把自写业务逻辑移动到 `third_party/`。

按同一口径，重构前基线 `62d85b7` 的 `client/apps`、`client/include`、`client/src` 为
62 个文件、10655 ELOC；当前减少 8751 ELOC（82.1%）。

| 模块 | ELOC |
| --- | ---: |
| `main` + `Client` 状态机 | 350 |
| `AudioEngine` | 524 |
| WSS + protocol `Transport` | 396 |
| 环境配置 | 141 |
| `Status` | 41 |
| Rockchip 3A 薄适配 | 304 |
| Snowboy 旧 ABI 桥 | 148 |
| **合计** | **1904** |

旧 `manual_single_turn`、自写 WSS/parser、重复协议对象、独立 ALSA/renderer/resampler/gain
框架、未接入的 runtime/UI/update/supervisor 及其失效测试已删除。现役路径只保留一个业务
状态机、一个音频对象和一个持久传输对象。

## 保留的行为

- ALSA 48 kHz/S16_LE/双声道真全双工，20 ms period、80 ms buffer。
- 48→16 kHz 双麦输入、Rockchip 3A/AEC、Snowboy、WebRTC VAD。
- 24→48 kHz TTS、固定 1.5 s 播放队列、最终增益/限幅及播放 reference。
- 500 ms pre-roll、唤醒后 6 s 等待、700 ms 结束静音、60 s 单轮上限。
- 持久 WSS、SPKI pin、16 kHz 上行和 24 kHz 下行协议。
- 80 ms duck、160 ms cancel、3 s 免唤醒追问和 1/2/4/8/16/30 s 异步重连。
- 采集线程在连接建立/重连期间继续读取 ALSA；上下行 PCM 使用固定容量缓冲。

## 构建证据

- 构建机：`ubuntu-codex`
- 工具链：GCC 8.3 `arm-rockchip830-linux-uclibcgnueabihf`
- libc/loader：匹配 BSP 的 uClibc，`/lib/ld-uClibc.so.0`
- 模式：Debug + `-O2`、严格 `-Wall -Wextra -Wpedantic -Werror`
- 外部头文件：Boost 1.74；仓库固定 WebSocketpp 0.8.2
- 未剥离 ELF SHA-256：`620edf90c6c39200162bcc99d9967ce3f5f2ec0c8a29b6b64439c4e6d5abd4bf`
- 板端剥离 ELF SHA-256：`d83091fa99a9e2d69d1b7757c4db7d42d4f6fe579c76a5e1f2287aa5d6e2fa9d`
- ELF：32-bit ARM EABI5、uClibc；无 `RPATH`/`RUNPATH`
- 直接依赖：Rockchip AEC/common、ALSA、swresample 3、avutil 56、atomic、C++ runtime/uClibc
- 干净快照只含上述 15 个生产文件；从零配置和完整链接通过，结果与增量构建一致
- Ubuntu 协议 fixture 通过，Python vendor/HIL 确定性测试 `69/69` 通过

## 板端启动证据

- 当前候选/活动文件：`/userdata/boompi/task2-diagnostic/boompi-client`
- 可恢复上一版：`/userdata/boompi/task2-diagnostic/boompi-client.under2k-15da78d2`
- 可恢复更早版：`/userdata/boompi/task2-diagnostic/boompi-client.under2k-735e0063`
- 旧版 SHA-256：`2f826852107f847fc57a598445788b94ebe9bab6a383c1ed1bee52c0269d596f`
- 启动日志：Rockchip AEC/BF 初始化成功，随后出现 `voice loop ready` 和 `secure session ready`
- 空闲观测：约 10 MiB RSS、约 35% CPU、进程 4 线程；源码自有主/播放/WSS 三个执行上下文，
  另一个为链接依赖内部线程；观测窗口内无 `EPIPE`、TTS queue full 或进程退出

首次真人回归已完成唤醒、VAD、提交、AI 文本/TTS 和打断确认，随后暴露出主动
`DropPlayback` 被误标为 reference 故障、导致新一轮 `capture discontinuity` 重连的问题。
当前候选已区分“受控丢弃”和“真实播放失败”：受控打断清空 reference 但不建立
discontinuity；xrun、重采样或写入失败仍保持失败语义。修复后的打断回归待复测。

本次提交时仍保留三项真人验收问题：扬声器回放会触发近端语音判定，说明 AEC/reference
闭环尚未有效；用户感知 VAD 开头缺字，虽然代码已有 500 ms pre-roll，仍需检查发送时序与
实际上行证据；服务端把模型 Markdown 原文直接送入 TTS，`*` 等格式字符需要在语音文本中
净化。板端 Snowboy 灵敏度已从 `0.5` 调到 `0.7`，该参数只存在测试启动配置中。

部署时 `/userdata` 被历史二进制占满；仅删除了
`boompi-client.bak-pre-o1-cb37-20260731`，其 SHA-256 与仍保留的
`boompi-client.bak-pre-echo-gate-cb37-20260731` 完全相同，可从后者无损恢复。

本记录只关闭“源码体量、交叉链接、板端加载和空闲持久会话”四项。必须由人在场继续说
唤醒词并完成首轮问答、三秒追问、长回答、播放中打断、环境噪声误打断及断网恢复；通过前
不得把该候选标成发布版。
