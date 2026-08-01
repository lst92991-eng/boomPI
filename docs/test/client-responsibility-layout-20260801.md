# 语音客户端职责重排与验证记录

- 记录时间：2026-08-01 11:27:04 +08:00
- 最新更新：2026-08-01 18:25:00 +08:00
- 分支：`codex/p1-fast-vertical-integration`
- 可回滚基线：`6204442 fix(voice): stabilize echo, pre-roll, and spoken text`
- 当前状态：严格交叉构建和第三块板端无人工冒烟已通过；真人语音验收待完成

## 本次范围

本次只收口 RV1106 语音客户端闭环，不接入屏幕 UI。目录仍按 boomPI 已有职责组织，未引入
`App/Driver/Inf` 平行分层，也未恢复通用 worker、工厂、插件或占位模块。

| 边界 | 唯一职责 |
| --- | --- |
| `application/voice_client` | 会话 actor、唤醒/追问、pre-roll、turn/epoch、超时与打断 |
| `audio/audio_engine` | 唯一播放线程、固定 TTS 环、drop/duck 与播放结果 |
| `network/voice_transport` | 持久 WSS/TLS、SPKI 固定、协议身份和序号校验 |
| 私有 `platform/rv1106/audio_backend` | ALSA、重采样、Rockchip 3A、Snowboy、WebRTC VAD 和近讲门控 |

长期执行上下文只有主控制/采集 actor、播放线程和 WSS/ASIO 线程。网络回调只向 64 项固定
事件环提交结果；播放队列固定为 75 帧，pre-roll 固定为 25 帧。播放参考由 Mode1 capture 的
`refL/refR` 提供，不经过 application/audio 软件环。

## 代码量口径

统计范围是进入 `boompi-client` 的 17 个 `.cpp/.h` 生产文件。ELOC 定义为非空且不是纯
`//` 注释的行；测试、文档、第三方源码和生成文件不计入。

| 口径 | ELOC | 约束 |
| --- | ---: | ---: |
| Rockchip/Snowboy vendor 集成 | 439 | 单独公开，不计入产品核心预算 |
| 产品核心 | 1846 | `< 2000` |
| 含 vendor 集成总量 | 2285 | `<= 2300` |

这 439 行不是第三方库源码：其中 Snowboy C ABI bridge 为 148 ELOC；Rockchip adapter 为
291 ELOC，包含固定 2 mic + 2 ref profile、320↔256 帧 FIFO、生命周期和错误恢复。将二者单列是为了回答
“业务逻辑有多少”，不是把它们伪装成外部库。CI 通过
`scripts/tests/test_client_source_contract.py` 固定文件集合、两个预算和当前文档本地链接，避免
把业务代码迁入其他目录规避计数。

## 与 Demo4Echo 的公平比较

参考仓库固定到
[`No-Chicken/Demo4Echo@97973f7`](https://github.com/No-Chicken/Demo4Echo/tree/97973f751df531bf3e8a008bd65e40530275669b/AIChat_demo)。
按排除 tests、c_interface、vendor 和资源的同类口径，其运行客户端约 1484 ELOC。它值得借鉴
的是薄入口、按职责落目录和直接调用成熟库；但它是半双工 PortAudio 示例，不包含 boomPI 的
双麦 Rockchip AEC/reference、持久 WSS/TLS、协议 identity、流式 TTS、打断与三秒追问，不能
直接用其总行数作为 boomPI 的等价上限，也没有复制其 GPL 实现。

## 本次修正

- 保留持久 WSS；普通当前轮故障发送 `turn.cancel`/`response.cancel`，不重启健康会话。
- 新 turn 清理旧 response 身份；迟到 epoch/response 帧丢弃，cancel/done 竞态幂等。
- TTS 首播固定预缓冲 180 ms，短 EOS 例外；队列仍有 1.5 s 硬上限。
- 首次有效硬参考后的 600 ms 只用于 3A 收敛；播放期首个近讲候选立即硬静音，等待硬参考
  连续 3 帧降低（最多 15 帧），再清尾 10 帧（200 ms）、重置 listener，并以连续 6 帧
  （120 ms）二次确认，再 drop/cancel。失败则恢复播放。
- 自然播放结束由 backend 抑制 15 帧（300 ms）尾音；follow-up 随后以连续 20 帧
  （400 ms）确认新一轮近讲。
- capture discontinuity 重建 3A/Snowboy/VAD 历史；播放故障与主动 drop 分开上报。
- pre-roll 发送前校验 capture sequence，最多保留当前连续 500 ms AEC 后 PCM。
- 删除软件 reference ring/60 ms lead；生产使用 Mode1 同帧硬件 `refL/refR`，不叠加两种参考。
- `near_voice` 只使用 3A 后 WebRTC VAD；不把 `wakeup_status` 当 DTD，也不使用 raw mic RMS。
- 首次有效硬参考后保留 600 ms 3A warm-up，并在边界结束时重置 VAD 历史。
- 自然播放结束抑制 300 ms 尾音并在末端复位 VAD；主动打断不清除已确认近讲的 VAD 生命周期。
- 生产 DSP profile 改为 mask `1109`（FastAEC、AES、ANR、Dereverberation、STDT），vendor
  AGC 关闭；`ALC31/ref2/delay0` 是当前候选，尚未作为最终壳体参数验收。
- 同类无人声/嘈杂环境 A/B：AGC ON `n=5`，`confirmed=4/5`、`follow=5/5`、
  `attempts=119`；AGC OFF 累计 `n=10`，`confirmed=2/10`、`follow=3/10`、
  `attempts=43`。AGC OFF 明显改善但不充分。
- 主动硬参考探针是应用层防自激 containment 候选，不是 AEC 结论；探针故意静音的样本不计入 AEC 效果
  评分，真人 double-talk 仍待受控环境验收。

## 验证清单

| 项目 | 当前结果 |
| --- | --- |
| 源码/文档契约与坏链接 | 通过，2/2 |
| Linux 全量 Python 回归 | 通过，72/72 |
| Go `test` / `vet` / `build` | 本次未改 Go；此前基线通过，本次复验因下载 `gorilla/websocket` 超时未闭合 |
| GCC 8.3/uClibc 严格 RV1106 Debug 交叉构建 | 通过 |
| 第三块板 `/tmp` 候选启动、线程/RSS、无重启冒烟 | 通过；4 threads，VmRSS 10856 kB |
| Mode1 48 kHz 四通道全双工相关性 | 通过；`[mic0,mic1,refL,refR]`，ref 相关系数均为 0.9983 |
| direct 3A 2 mic + 2 ref 固定帧调用 | 通过；1024 shorts 输入、512 bytes 输出、guard 完整 |
| 生产 DSP profile | mask `1109`；FastAEC/AES/ANR/Dereverberation/STDT，vendor AGC 关闭 |
| 无人声/嘈杂环境 AGC A/B | AGC OFF 显著改善但未通过：ON 4/5 confirmed、5/5 follow；OFF 2/10、3/10 |
| 主动硬参考探针 | 业务防自激路径已启用；不计作 AEC 通过，真人双讲待验收 |
| Snowboy、首轮问答、3 s 追问、长回复打断、无自激 | 待真人验收 |

候选 ELF 是 32-bit little-endian ARM EABI5 hard-float/uClibc，strip 后 5726412 bytes，
SHA-256 为 `f8b7b3103599714cc5bb81ac2db2ecd1917aa3cd9b158841ae93246c324c7b63`。它只部署到
`/tmp/boompi-client-mode1`，没有覆盖 `/userdata` 现役程序。板端配置检查通过，候选
稳定运行并进入 `secure session ready`，Rockchip 3A 与 Snowboy 均完成初始化，未观察到重启。
七秒启动窗口结束时仍存活；向已核验的候选 PID 发送 SIGTERM 后正常退出，没有强制 kill，
loopback mixer 由进程恢复为 `Disabled`。

ABI、endianness、loader、hard-float、RPATH 检查均通过；完整 ELF verifier 仍因既有 WebRTC VAD
静态库包含构建机 `__FILE__` 字符串而报告 `absolute_development_path`。这些字符串不是运行时依赖，
但发布前仍应重编 vendor archive 使用 `-ffile-prefix-map`，不能把本次结果写成完整发布闸门通过。
人工结果未完成前不得把 AEC 声学效果或长期稳定性写成通过。

## 已知限制

- Boost property_tree 无法可靠区分 JSON 数字和字符串，也不能拒绝重复键；当前身份、范围和
  状态校验已经收紧，但严格 JSON parser 仍是后续替换项。
- playback reference 已由 Mode1 正交信号相关性确认；最终壳体、最大音量、double-talk 和
  远场 AEC 仍必须靠 HIL/真人复测。详细证据见
  [P0 Mode1 硬件播放参考验证记录](p0-mode1-hard-reference-validation-20260801.md)。
- capture 当前使用阻塞 `snd_pcm_readi`；本板正常流中 SIGTERM 两秒内退出，但驱动永久卡死时
  尚无独立 poll timeout。发布 supervisor 仍须外层 watchdog，后续用目标 BSP HIL 再收紧。
- 当前范围不包含屏幕、配网、supervisor 和更新；它们不得以占位代码进入本次语音 ELF。
