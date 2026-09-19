# boomPI配套服务端

Go 1.26，单程序运行，无数据库。学生只需配置中国内地DashScope API Key。

## 启动

首次运行会要求输入Key，在程序旁生成config.yaml和state中的稳定TLS身份，然后开始服务。已有DASHSCOPE_API_KEY环境变量时直接使用。

```sh
boompi-server
```

使用指定配置：

```sh
boompi-server --config /path/to/config.yaml
boompi-server --check-config
```

## 数据流

16k单声道PCM → 实时ASR → 流式LLM → 16k PCM TTS与文本增量。

默认模型、音色和超时在代码中提供，维护者可参考`configs/config.example.yaml`调整。板端通过WSS 17806连接；UDP 17807负责局域网发现，客户端保存并校验服务端公钥指纹。

心跳由客户端每10秒发起Ping，服务端及时返回Pong并续期读取期限。`connection_timeout`默认30秒，可设15～30秒。升级已有维护者配置时删除`heartbeat_interval`字段；只有Key的学生配置可直接继续使用。已有config.yaml、Key和state中的TLS身份均保留。

每个服务端实例同时接入一块板子；多组课堂分别准备服务端及独立网络或预配对。

[BPV4协议](../protocol/protocol-v4.md)使用START/PCM/END/CANCEL及TEXT/AUDIO/DONE/ERROR。END后仍可取消；generation隔离旧回答，sequence检查音频连续性。DONE表示服务端发完，声卡尾播由客户端确认。

## 构建与维护

```sh
CGO_ENABLED=0 go build -trimpath -o boompi-server ./cmd/boompi-server
```

`internal/transport`负责WSS，`app`处理设备业务协议，`session`串行调用provider，`backend/qwenpipeline`编排云端服务。日志只记录必要阶段和耗时，不保存完整语音对话。

本程序用于可信课堂局域网，不直接暴露公网。保留已有config.yaml和state，不随升级重建身份。Key、证书私钥和真实配置不得提交Git。
