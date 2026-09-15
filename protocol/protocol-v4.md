# BPV4：固定文本控制与16 kHz PCM

客户端与服务端成套使用。WSS、TLS/SPKI与课堂鉴权保持；旧JSON控制和BPV3音频不兼容，不保留双栈。

每个WebSocket文本消息是一条完整控制。字段间使用一个ASCII空格；正文是剩余全部UTF-8字节，不做JSON转义。TEXT正文可以包含空格、换行和反斜杠，它们不会被当作新命令。所有消息拒绝NUL、非法UTF-8、超长消息、缺字段、未知命令和无效数字。

| 方向/含义 | 线格式 |
| --- | --- |
| 客户端握手 | `HELLO 4 16000 <设备UUID> <固定课堂口令>` |
| 服务端就绪 | `READY 4 16000` |
| 开始输入 | `START <generation> <supersede:0或1>` |
| 提交输入 | `END <generation>` |
| 取消/退休 | `CANCEL <新的generation> <retract:0或1>` |
| 文本增量 | `TEXT <generation> <正文>` |
| 完成 | `DONE <generation>` |
| 失败 | `ERROR <generation> <小写机器码>` |

generation为1～4294967295的十进制整数，不接受前导零、正负号、小数或指数。START与CANCEL的generation必须递增；CANCEL是退休边界，无需ACK。输入必须先START，至少一帧PCM之后才能END；END只结束输入，等待回复和播放时仍能取消。

HELLO使用规范小写UUID；口令限定1～256个可打印非空白ASCII字节，沿用现有课堂口令，云端Key不出现在协议中。版本和采样率必须精确匹配。TEXT正文1～4096字节；ERROR码1～64字节且仅允许`a-z0-9_`；控制总长最多8192字节。

## PCM

二进制消息为12字节头加S16_LE/mono PCM：

| 偏移 | 长度 | 内容 |
| --- | --- | --- |
| 0 | 4 | ASCII `BPV4` |
| 4 | 4 | generation，大端uint32，非零 |
| 8 | 4 | sequence，大端uint32，从0连续，UINT32_MAX不使用 |
| 12 | 可变 | PCM小端样本 |

上行固定640字节（20ms），下行2～640个偶数字节。短下行包只能是最后一包，此后允许TEXT、DONE或ERROR，但不能再有音频。服务端满帧立即发送，余数只在完成时发出，不补静音。

客户端网络是接收sequence的唯一校验者；播放队列仍检查内存边界、generation和取消状态，不再重复维护网络序号。服务端socket消息的PCM所有权直接转给会话worker，不追加第二份PCM副本。

DONE只表示服务端发完；应用调用playback::finish后，播放线程取完有效滤波尾音、等待ALSA drain，才发布Drained并进入追问。队列满、缺帧或超时取消整轮，不跳帧后继续，也不伪造END。断线重连不重发半句话，旧轮次不得复活。

共享样本：[protocol-v4-golden.json](fixtures/protocol-v4-golden.json)。该文件用JSON保存测试元数据，线上报文不是JSON，板端已无cJSON依赖。
