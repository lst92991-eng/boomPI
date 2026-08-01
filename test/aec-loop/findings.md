# AEC 回归结论

## 2026-08-01 基线事实

- 板卡：第三块 RV1106，SSH 别名 `rv1106-board-3`。
- 服务端：`ubuntu-codex:17816`，现有 Go 服务端保持持久运行。
- 客户端测试前处于停止状态；测试脚本配置为音量 `100`、板级增益 `120%`。
- Mode1 已确认四通道顺序为 `[mic0,mic1,refL,refR]`，左右数字参考相关系数均为 `0.9983`。
- 数字参考到麦克风的声学到达约 `14–17 ms`；该值尚未用于定量 ERLE/延迟扫描。
- 历史 AGC ON 基线的 Rockchip 日志为 `model_aec_en=0`、`delay_len=0`、`look_ahead=0`、
  `filter_len=2`，FastAEC/STDT/AES/ANR/DRVB/AGC 已启用，但公开 ABI 不提供可消费的 DTD 判定。
- 当前生产 DSP profile 为 mask `1109`：FastAEC、AES、ANR、Dereverberation、STDT 启用，
  vendor AGC 关闭；`ALC31/ref2/delay0` 是当前候选，并非最终声学参数。
- 当前应用把 Rockchip 3A 后的 WebRTC VAD 直接作为 `near_voice`；首次参考后仅做 `600 ms`
  预热，自然结束后做 `300 ms` 尾音隔离。
- 最近一次无人值守日志在一次真实唤醒后又产生至少三次无人工新 turn，已确认存在自激现象。

## 最新 AGC A/B 与主动探针边界

| Profile | 样本 | confirmed | follow | attempts | 结论 |
| --- | ---: | ---: | ---: | ---: | --- |
| AGC ON | 5 | 4/5 | 5/5 | 119 | 误触发严重 |
| AGC OFF | 10 | 2/10 | 3/10 | 43 | 明显改善，但仍未通过 |

- `confirmed` 是播放期主动硬参考探针二次确认；`follow` 是随后仍进入 follow-up/新轮风险；
  `attempts` 是各组探针候选累计数。
- AGC OFF 把 `confirmed`、`follow` 和候选总数同时压低，足以支持当前生产禁用 vendor AGC；
  仍有 `2/10` 和 `3/10` 残余，不能写成 AEC 或防自激已完成。
- 当前现场环境仍嘈杂，环境噪声会直接影响 VAD 和 follow-up，因此这组 A/B 只能作为方向性
  证据；真人 double-talk、安静环境基线和可控噪声回归仍待完成。
- 主动探针的流程是：首个候选立即硬静音，等待硬参考连续 3 帧降低（最多 15 帧），清尾
  10 帧（200 ms）并重置 listener，再以连续 6 帧（120 ms）确认近端语音。自然播放结束
  则由 backend 抑制 15 帧（300 ms）尾音，follow-up 再以连续 20 帧（400 ms）确认新一轮。
  它是业务 containment 候选，不是 AEC 结论；探针区间
  因扬声器被故意静音，不得用于 AEC 效果评分。

## 回归表

| Run ID | 唯一改变量 | 唤醒 | AI 回复 | 无人工新 turn | 误打断 | 自激 | 结论 |
| --- | --- | ---: | ---: | ---: | ---: | --- | --- |
| board3-warm5s-1..3 | 第三块板，双参考，先静音收敛 5 s | 绕过 | 固定 1.4 s PCM | - | 3/3 | 是 | 3A 输出约 -19.7~-17.7 dBFS，门开后连续命中 |
| board1-singadcl-1..3 | 第一块板，镜像默认 `SingadcL` | 绕过 | 同一 PCM | - | 0/3 | 否 | 右通道固定 -32768，双麦结果无效 |
| board1-diffadclr-1..2 | 第一块板，临时改为 `DiffadcLR` | 绕过 | 同一 PCM | - | 1/2 | 偶发 | 双麦恢复，3A 输出约 -68.1/-43.8 dBFS |

## 2026-08-01 跨板对照

- 第一块板 MAC 为 `a2:ad:df:37:a2:0f`，SSH 指纹为
  `SHA256:u4nlJhRiedRdx/RYwgm4xKhgS/KwlmpOcJfvJZqve3U`。DHCP 守护已改为同时允许第一块板和
  第三块板，旧指纹未删除。
- 两块板使用的 `libaec_bf_process.so`、`librkaudio_common.so` 和
  `librkaudio_detect.so` 哈希相同；探针和输入 PCM 哈希也相同。网络、Snowboy 和云端均已绕过。
- 同一数字参考在两块板上均约为 `-29.62 dBFS`。第三块板播放期原始双麦交流量约为
  `-18/-21 dBFS`；第一块板临时切到正确双麦模式后约为 `-40/-42 dBFS`，相差约 20 dB。
- 第一块板镜像的 `/etc/init.d/S60micinit` 固定设置 `ADC Mode=SingadcL`，与当前双麦目标不符；
  临时切换 `DiffadcLR` 后两路数据恢复，测试结束已恢复原值 `1`，未持久修改。
- 结论：问题不能归因于服务端、Qwen、唤醒或通用 AEC 调用逻辑。第三块板的模拟采集增益、
  麦克风噪声、DTS/mixer 或实际声学耦合至少有一项与第一块板显著不同，是当前第一优先级。
  第一块板仍出现过一次低电平 VAD 误判，软件门控也必须继续修正；此外需由现场确认两块板的
  物理扬声器均以相近音量出声，才能把这组数据作为最终硬件结论。
