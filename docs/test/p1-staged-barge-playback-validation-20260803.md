# P1 分阶段打断与播放连续性人工验证（2026-08-03）

## 范围

- 板卡：第一块 RV1106，SSH 别名 `rv1106-board`，主机名 `luckfox`。
- 服务端：既有持久 Qwen 会话，WSS 端口 `17816`；本轮没有修改服务端、AEC profile、VAD、
  网络、扬声器音量或 Snowboy 模型。
- 板端候选：`/userdata/boompi/qwen-test/boompi-client`，SHA-256
  `88c6d502028a50274c19880beb82cbb3792f773d7229750114c6208f2d29584a`。
- 验证结束时间：2026-08-03 11:17:08 +08:00；结束后客户端已按用户要求正常停止。

## 原因与修改

关闭 barge-in 后，用户确认 TTS 播放“比较流畅”，而服务端输出节奏、板端 ALSA 状态和网络队列
没有显示对应的持续拥塞。因此本轮把主要原因定位为旧策略：任意单帧 `near_voice` 都立即把后续
播放降为零；误判恢复又没有冷却，可能形成周期性静音。

现行策略保持原有硬参考确认，但分为以下阶段：

1. 原音量连续近讲 2 帧（40 ms）。
2. 播放降到 30%，继续确认 2 帧（40 ms）。
3. 候选仍持续才降到零，最多等待 8 帧，并要求 reference 连续低 2 帧。
4. 清尾 3 帧（60 ms）、重置 listener，再连续确认近讲 3 帧（60 ms）。
5. 失败恢复原音量并冷却 15 帧（300 ms）；成功才 drop 本地播放并取消云端回答。

这只是产品侧 containment；没有改变双麦、单硬件参考的 Rockchip 3A 输入布局。

## 结果

- Ubuntu 执行 `python3 -m unittest discover -s scripts/tests -p 'test_*.py' -v`，退出码 0，
  72 项通过；其中源码预算检查为 2/2。17 个生产文件共 2300 ELOC，其中 vendor 集成
  439 ELOC，产品核心 1861 ELOC。
- 使用匹配的 RV1106 GCC 8.3/uClibc 环境分别构建 `boompi_client` 和可选
  `boompi_aec_loop_hil`，两个 `cmake --build ... --target <target> --parallel 8` 均退出码 0。
- 现场观察覆盖本次候选从启动到用户要求停止的完整进程日志
  `/tmp/boompi-fast-barge.log`；为避免保存对话文本，原始日志没有提交仓库。按固定字符串统计
  `wake=3`、`voice turn committed=12`、`barge-in confirmed=10`；
  `voice loop failed=0`、`Broken pipe=0`、`jitter queue is full=0`。
- 第一版分阶段候选解决了明显断续，但用户反馈打断偏慢；把生产判定窗缩短后，用户结论为
  “这次可以了”。这是本轮人工体验通过结论。
- 上一版分阶段候选保留为
  `/userdata/boompi/qwen-test/boompi-client.pre-fast-barge-20260803`，可直接回滚。

## 尚未关闭的边界

- 本轮没有板端时间戳或声学采集，因此约 0.30–0.35 s 的响应只是按 20 ms 状态窗和既有播放
  缓冲推算，不是实测延迟。
- 候选来自既有 Debug/HIL 构建树。strip 后 ARM EABI5 hard-float/uClibc 和
  `GLIBCXX_3.4.22 <= 3.4.25` 符合板端，但仍保留固定 `/oem/usr/lib` RPATH 和 WebRTC 静态库
  编译路径字符串；它不是发布包。
- 本轮真人体验不能替代受控 ERLE、double-talk、远场、噪声、多音量和长期稳定性测试。
- `boompi_aec_loop_hil` 仍是独立硬参考压力探针；本轮已同步生产分阶段阈值与冷却并确认交叉
  构建，但它不直接复用 `VoiceClient`，不能把该探针结果当作生产策略单元测试。
