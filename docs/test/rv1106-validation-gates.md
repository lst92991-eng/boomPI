# RV1106 验证闸门

本文列出进入真实功能开发前必须完成的板端检查。除非测试记录明确给出板卡、镜像、日期和命令输出，否则状态均为“未验证”。

当前分项证据和阻断见 [2026-07-25 P0 可行性报告](p0-feasibility-report-20260725.md)
、[2026-07-27 Rockchip 3A 离线 ABI 探针报告](rockchip-3a-offline-probe-20260727.md)
、[2026-07-27 RV1106 ALSA 全双工 smoke 记录](rv1106-alsa-smoke-20260727.md)
及 [2026-07-27 Snowboy P0 板端探针记录](snowboy-p0-probe-20260727.md)。

## P0 环境与 ABI

- [ ] 记录 CPU ISA、float ABI、动态加载器、libc、libstdc++ 和内核版本。
- [x] 确认交叉编译器与目标 sysroot 匹配，并运行最小 C++ smoke executable。2026-07-27 的 Debug-only 3A 离线探针已在目标板退出 0；生产客户端 smoke 仍待补。
- [ ] 确认 ALSA、TLS、pthread 和所需系统调用在目标镜像可用。ALSA 1.2.8 已完成板端短时 capture/playback；TLS、pthread 与其余生产路径仍待板端运行验证，因此组合闸门保持未完成。
- [x] 对 Rockchip/Snowboy 运行库执行 `file`、`readelf` 和依赖/符号检查。功能与性能仍分别保留在下方闸门。

## 音频

- [x] 记录真实 ALSA card/device、支持格式、period/buffer 和 mixer 控件。2026-07-27 已记录 card 0 `rv1106-acodec`、显式 `hw:0,0`、本次格式和两端实际 buffer；只读 mixer 值未被修改。
- [x] 验证 48 kHz capture/playback 全双工，不用串行播放与录音冒充。2026-07-27 的静音 smoke 同时完成 4 通道 capture 与 2 通道 playback 50 个周期，失败 0；不证明可听播放或采集内容正确，迁移后的主线 committer 组合尚未板端重跑。
- [ ] 用真实 ALSA adapter 验证 partial write、would-block/interrupted、xrun/suspend、
  device loss、`drop`/`prepare` 和重新建链；不得重复写出已经 accepted 的 prefix。
- [ ] 对 adapter 故障注入验证 control aggregate `{}` 失败关闭；malformed positive count
  只能保守推进请求范围、禁止重放和伪 timing reference，并立即进入取消路径。
- [ ] 验证 accepted sequence 耗尽会在下一次 write 前拒绝；完成 `drop`/`prepare` 后仍
  保持 terminal restart-required，不能通过换 generation 或重建 committer 假恢复。
- [ ] 验证 cancel fence 到停止旧代际 write、本地 playback ACK、DSP/reference reset ACK
  join 和最终扬声器静音的时序；结果必须区分 retired/prepared PCM incarnation，且本地
  `drop`/`prepare` 返回不能单独充当声学静音证据。
- [ ] 用 ALSA status/delay 与可观测硬件证据定义 normal-EOS presentation completion；
  sink accepted 或预计 presentation timestamp 不能直接报告为 played/audible。
- [ ] 用可辨识信号确认 Mode1 四通道顺序、双麦极性和数字参考采样位置。
- [ ] Mode1 不成立时停止，不自动启用未经评审的软件 reference。本轮 Mode1 保持 Disabled，未自动启用软件 reference；最终 reference 方案仍待评审。
- [ ] 若评审后启用软件 reference，验证 accepted-prefix ledger 到 AEC 输入的组装、换代、
  partial/cancel 边界、延迟对齐和旧 reference 清除；不得用原始 TTS 或补零前缀代替。
- [ ] 验证 Rockchip 3A 16 kHz 输入布局、错误码、单帧最坏耗时和持续实时率。离线 ABI 已确认固定 16 ms、2 mic + 2 ref 时 `input_size=1024` 个样本、成功返回 512 字节单声道输出、长度不匹配返回 0；实际通道排列、声学效果和实时率仍未验证。
- [x] 评审 16 ms 厂商块与 20 ms 核心帧之间的缓冲、输出可用状态和元数据归属。独立 `AudioDspFrameBridge16k` 已实现 `kNeedMoreInput`/`kOutputAvailable`、最早输入 metadata 归属、换代清空和逐样本守恒；它不修改冻结的 `AudioDspEngine`，Rockchip platform adapter 与板端连续输入仍待补。
- [ ] 最终壳体中记录距离、角度、音量、背景噪声、ERLE、残余回声和 double-talk 结果。

## Snowboy

- [x] 确认锁定 feasibility runtime、资源和默认 `snowboy.umdl` 的来源、SHA-256 与
  Apache-2.0 范围；其他模型与未来发布 runtime 仍需逐个核对并完成 packaging review，
  不能从当前候选外推。
- [ ] 验证模型初始化、16 kHz mono 连续输入、目标英文唤醒和安全错误路径。默认模型
  正向离线探针已检测 keyword 1，但缺失模型会直接 `terminate`/`Aborted`，因此该闸门
  保持失败。
- [ ] 记录 CPU、RSS、最坏帧耗时、漏唤醒和误唤醒；当前短测已记录最大 7.197 ms 与
  3,180 KiB，真实麦克风统计及至少 30 分钟稳定性仍未执行。
- [ ] fatal destructor/缺模型 `terminate` 证据已保存，架构选择和独立 helper 候选仍
  待用户批准；尚未静默换引擎或自行改为多进程，当前代码保持 Debug-only feasibility
  且未接产品 runtime。

## 网络、UI 与系统

- [ ] 以太网和 Wi-Fi 分别验证 DHCP、路由、断线和恢复。
- [ ] 验证 Wi-Fi 芯片/驱动支持目标 AP/STA 配网流程。
- [ ] 验证 UDP 发现、WSS 首配、SPKI 固定、重连和 half-open timeout。
- [ ] 验证 ST7789P3 横屏像素格式/旋转/刷新、GT911 坐标和背光休眠。
- [ ] 验证 BusyBox init 的实际 `inittab` 字段和 supervisor 有序停止/重启。
- [ ] 仅在 `/dev/watchdog`、DTS 和驱动均确认后测试硬件 watchdog。
- [ ] A/B 更新最后验证：签名失败拒绝、掉电安全、候选三次失败回滚和 mark-good。

## 安全边界

只读探测可以直接进行；刷镜像、改设备树、分区、启动项或持久删除数据必须针对该次操作取得用户明确授权。测试日志不得包含 API Key、Wi-Fi 密码、device token、证书私钥或用户录音。
