# boomPI v2 教学协议

状态：本次客户端与服务端共同实施的唯一协议。旧 v1 由 Git 历史保存，不提供双栈回退。

## 固定连接

- WSS `/ws`，TCP 17806，TLS/SPKI 保持现有可信课堂局域网部署方式。
- UDP 17807 请求 `BOOMPI_DISCOVER_V2`，回复 `BOOMPI_SERVER_V2 <port> <spki>`。
- 客户端首个 JSON：`{"type":"hello","device_id":"<canonical uuid>","token":"<existing teaching token>"}`。
- 服务端验证后回复 `{"type":"ready"}`。设备身份只在握手发送一次。
- 握手和 WebSocket ping/pong、连接超时由连接模块负责，不进入对话状态机。

## 音频帧

每个二进制 WebSocket message = 16 字节网络字节序头 + S16_LE mono PCM。

| 偏移 | 大小 | 字段 |
| --- | --- | --- |
| 0 | 4 | ASCII `BPV2` |
| 4 | 2 | flags：START=1、END=2、SUPERSEDE=4 |
| 6 | 2 | reserved，必须为 0 |
| 8 | 4 | generation，非零 uint32 |
| 12 | 4 | sequence，从 0 连续递增 |

- 上行固定 16 kHz，每帧恰好 640 字节 / 20 ms。
- 下行固定 24 kHz，1..480 个 sample；非末帧必须为 960 字节。
- START 只能出现在 sequence 0；END 恰好一次；END 后禁止继续该方向的音频。
- SUPERSEDE 仅允许在上行 START 同时出现，表示撤回上一轮未听完的回答。
- START 创建轮次，END 提交输入；不另发送 turn.start/turn.commit。
- 新 generation 必须大于本连接已退休/使用过的 generation。旧帧安全丢弃，禁止复活。
- generation 耗尽时重建连接，不回绕。

## 控制与文本

所有 JSON 拒绝重复键、未知键、错误类型、非法 UTF-8 和越界值。最大 JSON 8192 字节。

- 文本：`{"type":"text","generation":1,"text":"增量"}`，单次 text 最多 4096 UTF-8 字节。
- 完成：`{"type":"done","generation":1}`，有音频时必须先发送音频 END；纯文本也可完成。
- 错误：`{"type":"error","generation":1,"code":"bounded_code"}`，不发送秘密或原始 provider 错误。
- 停止：`{"type":"stop","generation":2,"retract":true}`。generation 是新的退休栅栏；立即停止旧工作，retract 决定是否撤回上一段未听完的历史。无 ACK。

客户端确认打断时立即停止本地旧播放，并发送新 generation 的 START|SUPERSEDE 和已保留的近讲 PCM。服务端在接收新音频前先封住旧下行，再异步清理旧 provider 工作。普通 START 保留上一轮历史。STOP 用于触屏只停止、输入断帧、拥塞或超时，不得伪造 END 提交残缺输入。

## 有界性与所有权

- 所有文本、音频、完成事件和本地播放完成均绑定 generation；旧代不能结束或污染新代。
- 客户端只有 application 分配 generation，VoiceLink 校验 wire 顺序并丢弃旧代，VoiceAudio 隔离播放生命周期。
- 双向 PCM 不静默丢中间帧。上行缓冲最多 800 ms，溢出明确中止当前轮次。
- STOP 与 START|SUPERSEDE 有保留的控制通道容量；无法有界发送时关闭连接。
- 服务端在本地切代后继续有界接收新输入，旧 provider 取消不能阻塞 WSS 读取；取消或排队超过限额明确失败。
- 网络断开不续传半句话。TLS、身份校验、长度限制和线程有界退出保留。
