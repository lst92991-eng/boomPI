# P0 Mode1 硬件播放参考验证记录

- 记录时间：2026-08-01 14:44:08 +08:00
- 目标：确认第三块 RV1106 上的全双工通道布局、直接 Rockchip 3A 输入契约和生产客户端启动/退出边界
- 结论：Mode1 硬件参考链路及 direct 3A 固定帧调用已关闭；最终壳体 ERLE、真人 double-talk 和最大音量仍未验收

## 48 kHz 全双工与通道布局

临时把 `I2STDM Digital Loopback Mode` 设为 `Mode1` 后，直接 ALSA 在同一时段以
`48 kHz / S16_LE` 完成四通道采集和双声道播放，测试窗口没有 xrun。生产参数按实测固定为：

| 方向 | 通道 | period / buffer |
| --- | --- | --- |
| capture | 4，`[mic0,mic1,refL,refR]` | `960 / 1920` frames |
| playback | 2，`[left,right]` | `960 / 3840` frames |

正交低幅信号给出的数字相关性如下：

- 左播放 997 Hz 对 `refL` 的相关系数为 `0.9983`，没有把右参考误判为左参考。
- 右播放 1499 Hz 对 `refR` 的相关系数为 `0.9983`，没有把左参考误判为右参考。
- 当前物理扬声器链路主要使用 DAC-L；正常产品播放仍复制 mono 到左右两路，但 `refL` 是当前声学路径中主要有效的参考。
- 从数字参考到麦克风的声学到达时间约为 `14–17 ms`。这是阈值法近似，不是最终壳体的精密系统辨识结果。
- 原始麦克风存在较高 DC/底噪，因此禁止用 raw mic RMS 作为近讲或 double-talk 判据。

测试完成和进程退出后均把 loopback mixer 恢复为 `Disabled`。Mode1 是运行时临时配置；本记录
没有修改设备树、镜像、分区或持久启动项。

## Direct Rockchip 3A 契约

板端实际运行库接受 `rkaudio_preprocess_init(16000, 16, 2, 2, ...)`。每个 vendor block 是
256 samples/channel，输入按 `[mic0,mic1,refL,refR]` 交织：

- 输入：`1024 short`，即 `2048 bytes`。
- 输出：`512 bytes`，即 256 个 16-bit mono samples。
- init 耗时：`11262 us`；单次 process 耗时：`1561 us`。
- 本次历史固定帧 HIL 使用 mask `1141`，即 FastAEC、STDT、AES、ANR、Dereverberation 和
  vendor AGC；这记录的是当时已执行参数，不代表当前生产 profile。
- STDT 阈值：high `0.70`、low `0.50`。
- 硬件参考 profile 使用 `model_aec_en=0`，不启用软件 reference delay。
- 输入/输出 guard 完整，返回长度符合 ABI。

供应商公开 ABI 没有提供可直接读取的 DTD 事件或 getter。`wakeup_status` 属于唤醒状态 ABI，
不是 DTD 输出，禁止把它解释成 double-talk 结论。因此生产 `near_voice` 只取 3A 后 WebRTC VAD；
首次收到有效硬件参考后保留 600 ms 自适应 warm-up。自然播放结束后另保留 300 ms 尾音窗并在
末端重置 VAD；主动打断不复位已确认近讲的 VAD 生命周期，避免短打断词丢失结束事件。

## 生产实现边界

生产 capture 直接使用 Mode1 的四通道硬件参考，不再维护软件 reference ring，也不再使用
“60 ms software lead”。48→16 kHz 重采样保持四个通道同相位，随后只调用一次 Rockchip 3A；
Snowboy、VAD 和上行网络共同消费 3A 后 mono PCM。硬件 Codec reference 与软件 reference
禁止同时启用。

当前生产 DSP profile 已收敛为 mask `1109`：FastAEC、AES、ANR、Dereverberation 和 STDT
启用，vendor AGC 关闭。当前 `ALC31/ref2/delay0` 仍是 A/B 后的候选参数，不是最终壳体定案。
因此历史 mask `1141` 的固定帧 ABI 证据继续有效，但它的 AGC 行为不能外推为当前生产行为。

## AGC A/B 与主动硬参考探针

同类无人声/嘈杂环境回归的累计结果如下；`confirmed` 表示主动探针二次确认，`follow` 表示随后
仍进入 follow-up/新轮风险，`attempts` 是探针候选总数：

| Profile | 样本 | confirmed | follow | attempts |
| --- | ---: | ---: | ---: | ---: |
| AGC ON（mask `1141`） | 5 | 4/5 | 5/5 | 119 |
| AGC OFF（生产 mask `1109`） | 10 | 2/10 | 3/10 | 43 |

AGC OFF 对误触发有明显改善，但仍有 `2/10` confirmed 和 `3/10` follow，不能写成问题已解决。
本轮环境仍嘈杂，噪声是未受控混杂因素；这组数据只支持保留 AGC OFF 作为当前生产选择，不能
替代安静可控环境、最终壳体或真人 double-talk 验收。

播放期主动硬参考探针在首个近端候选时硬静音，等待 reference 连续 3 帧降低（最多 15 帧），
再清除 10 帧（200 ms）声学尾音、重置 listener，并以连续 6 帧（120 ms）重新确认近端语音；
确认后才 drop/cancel，失败则恢复播放。自然播放结束走独立路径：backend 抑制 15 帧
（300 ms）尾音后，follow-up 以连续 20 帧（400 ms）确认新一轮近讲。这是产品侧
防自激 containment 候选，不是 AEC 算法结论。探针区间因扬声器被故意静音，
相关样本不得计入 AEC 效果评分或用来宣称 ERLE/残余回声通过。

## 启动与有限无人工冒烟

候选客户端启动日志到达 `voice loop ready` 和 `secure session ready`。发送 `SIGTERM` 后正常
退出，loopback mixer 自动恢复为 `Disabled`，没有依赖强制 kill 清理。

另以低幅 997 Hz 做过有限的无人工收敛观察：至少一轮安静样本在自适应后得到 processed RMS
`0.000435` 且 wake count 为 `0`。其他轮次受到环境声影响较大，因此该结果只能证明硬参考确实
进入算法并呈现收敛行为，不能据此声明 ERLE、残余回声、真人 double-talk 或量产声学已经通过。

## 尚未关闭

- 最终壳体、实际工作音量和目标 0.5 m 距离下的 ERLE/残余回声测量。
- AI 播放期间真人任意语音打断，尤其是噪声环境中的 double-talk 灵敏度与误打断。
- 长时间 CPU/RSS、单帧最坏耗时、xrun 恢复和 30 分钟以上稳定性。
- 两个 mic slot 的最终物理左右命名和量产极性复核。
