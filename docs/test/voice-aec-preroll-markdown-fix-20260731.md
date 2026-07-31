# 语音闭环修复记录（2026-07-31）

- 记录时间：2026-07-31 20:24:00（UTC+08:00）
- 测试对象：RV1106 第三块板、boomPI 本地 Go 服务端
- 分支：`codex/p1-fast-vertical-integration`
- 基线提交：`5df2324`

## 问题与结论

1. 扬声器声音触发新对话
   - Rockchip 3A 输入布局已核对为 `[mic0, mic1, playback_reference]`，`aec->pos=1` 正确，不是双麦通道或设备树回归。
   - 客户端压缩时漏掉了每次播放开始后的 600 ms AEC/打断预热以及预热结束时的 WebRTC VAD 重置，持续 VAD 会被 TTS 残留在 speech 状态。
   - 播放结束后的 follow-up 状态原先只看 `vad_started`，没有采用 `near_voice` 回声门结果，扬声器尾音仍可误开新 turn。
   - 修复后每轮播放重新校准软件回声能量基线，并从首个对齐参考帧开始 600 ms 预热；播放结束会立即收口未完成的预热，不会把禁用状态带入 follow-up。follow-up 需要连续 6 帧（120 ms）`near_voice` 才启动。

2. VAD 开头疑似丢帧
   - 普通唤醒路径原本已有 25 帧、500 ms 环形前滚，并按采集顺序发送；服务端没有主动丢弃首帧。
   - 确认的缺口位于播放结束切换到 3 秒 follow-up：旧代码会清空环，用户若在播放最后不足 160 ms 开口，语音开头会丢失。
   - 修复后环、近讲累计和 VAD speech 状态一同跨越 playback 到 follow-up；`response.done` 统一延后到当前采集帧处理后收口，避免边界帧被 VAD 重置。另新增采集序号连续性检查和 `pre_roll_frames/pre_roll_ms/capture_sequence` 日志。

3. Markdown 符号进入 TTS
   - 新增跨 LLM delta 的 TTS 文本过滤，只处理送往 Qwen TTS 的副本。
   - 标题、列表、引用、星号强调、反引号和 Markdown 链接会转换为适合朗读的纯文本；`2*3`、`2**3`、普通方括号等非 Markdown 内容保留。
   - 屏幕 `EventTextDelta` 和对话 history 仍使用模型原文，不受过滤影响。

## 自动验证

- RV1106 ARM EABI5 严格交叉编译：通过。
- 客户端 Python/vendor/HIL 测试：`69/69` 通过。
- 服务端 Go 1.26.5：`go test ./...` 全部通过。
- 客户端有效生产代码：15 个自研 C/C++ 文件，1952 ELOC，仍低于 2000 行目标。
- `git diff --check`：通过。

## 已部署产物

- 板端客户端：`/userdata/boompi/task2-diagnostic/boompi-client`
  - SHA-256：`ac054acf21a4eed09506eb0abd86bbc1b569288826987c9fcb69f874ec832ced`
  - 启动 PID：`887`
- Ubuntu 服务端：`/home/st/boompi-v1-audio-20260729/boompi-server-task2`
  - SHA-256：`8f97d6dfe27ad0f21f1d558a8da66c7782a3008acbba279c86148825c6559d31`
  - 启动 PID：`108462`
  - 回滚备份：`/home/st/boompi-v1-audio-20260729/boompi-server-task2.bak-pre-mdtts-20260731`
- 板端已重新建立安全会话。

## 人工验收待办

1. 静默连续完成三轮回复，确认扬声器不会自触发 follow-up 或误打断。
2. 在超过 600 ms 的 TTS 回复中说任意话，确认约 80 ms 降音、约 160 ms 完成打断；前 600 ms 暂不允许打断是本版明确权衡。
3. 用爆破音开头的短句测试，日志应显示 25 帧、500 ms 前滚且采集序号连续。
4. 让模型回答带粗体和编号的列表，屏幕保留 Markdown，扬声器不朗读星号等格式字符。
