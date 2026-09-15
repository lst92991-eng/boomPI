# boomPI v3 语音协议

本版本只服务配套客户端和 Go 服务端。服务端是课程配套黑箱：客户端需要理解连接、音频与轮次，不学习云端 SDK 或服务端实现。

## 连接与握手

使用持久 WSS `/ws`，保留 TLS 公钥 pin 校验、固定课堂口令、心跳和断线重连。第一条业务消息是 hello，服务端认证成功后返回 ready；两端必须声明版本 3 和 16000 Hz。旧版缺字段或声明其他值均拒绝握手，不保留兼容路径。

```json
{"type":"hello","device_id":"00000000-0000-4000-8000-000000000001","token":"课堂共享口令","version":3,"sample_rate":16000}
{"type":"ready","version":3,"sample_rate":16000}
```

每种 JSON 对象只允许列出的字段，拒绝重复键、未知字段、非法 UTF-8、null 和错误类型。generation 为非零 uint32 十进制整数。单条控制消息不超过 8192 字节；字幕增量不超过 4096 字节。

## 上行：START → PCM → END

```json
{"type":"start","generation":1,"supersede":false}
{"type":"end","generation":1}
{"type":"cancel","generation":2,"retract":true}
```

START 是独立控制消息，generation 必须大于本连接之前的值。随后上传该轮 PCM，序号从 0 连续递增；至少一帧后才能 END，END 后禁止继续上传本轮 PCM。每帧固定为 16 kHz / S16_LE / mono 的 20 ms，即 320 点、640 字节；一轮最多 3000 帧（60 秒）。END 不携带 PCM，不需缓存最后一帧来补 flags。

普通 START 退休旧轮并保留已经完成的上下文；`supersede:true` 同时撤回上一段尚未听完的回答。CANCEL 使用新的 generation 作为退休边界，本代不生成回复；`retract` 控制是否撤回未听完的上下文。即使已发送 END 或已收到部分回复，CANCEL 仍有效，不需要等待 ACK 后再开始下一轮。已排队的撤回控制必须按顺序交付，不能被后来的普通 START 擦掉。

## PCM 二进制头

每个 WebSocket binary 消息包含 12 字节头和 PCM；头字段采用网络大端序，PCM 样本采用小端序。

| 偏移 | 长度 | 内容 |
| --- | --- | --- |
| 0 | 4 | ASCII `BPV3` |
| 4 | 4 | generation |
| 8 | 4 | sequence |
| 12 | 可变 | S16_LE PCM |

没有 flags 或保留位。上行负载固定 640 字节；下行负载为 2～640 个偶数字节。序号 UINT32_MAX 不使用，避免递增回绕。短下行帧只出现在尾部，同一轮短帧后不得再有音频。跨线程丢帧、重复序号或中途跳号都必须报错，不能以 WebSocket 有序为由省掉连续性检查。

## 下行：TEXT / AUDIO / DONE / ERROR

```json
{"type":"text","generation":1,"text":"你好"}
{"type":"done","generation":1}
{"type":"error","generation":1,"code":"provider_error"}
```

文本与 PCM 可交错。上行 END 后才开始正常回复，ERROR 可在上传期间报告失败。PCM 序号从 0 连续增加；服务端满 20 ms 就发送，不为给末帧加 END 而保留一帧。厂商音频块不足 20 ms 时保留组帧余数，收到 provider 完成后发送真实短尾，随后发送 DONE；不补静音、不丢尾样本。纯文本或空回答也可直接 DONE。

DONE 表示云端数据交付完成。客户端调用本地 playback.finish，等播放队列和 ALSA 尾音都结束后再进入追问。它不等于扬声器已经播完。

## 取消、背压与断线

客户端分配 generation；服务端校验，网络/播放按当前值过滤旧结果。较旧的合法 PCM 或回复不能污染新轮，未来代或非法包必须使连接失败。连接断开后退休旧轮，重连只恢复空闲态，不复活旧音频或重新提交残缺输入。

输入队列有容量与约 800 ms 的滞留上限。上行背压不是丢一帧继续发，应用必须取消本轮；取消控制有保留空间。服务端云调用同样有期限，失败明确结束当前轮或连接。具体阈值属于维护者配置，学生无需配置。

共享字节样本见 [protocol-v3-golden.json](fixtures/protocol-v3-golden.json)，分别由 C++、Go 与 Python 独立校验。Host 回环不代表真板声卡或真实云端验收。
