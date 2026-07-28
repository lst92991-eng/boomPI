# Qwen Realtime adapter

This package is the minimal Model Studio WebSocket adapter used by boomPI. It
does not read environment variables, log credentials, retry requests, or expose
tool calling.

Protocol boundary verified against the Alibaba Cloud Model Studio documentation
on 2026-07-28:

- China (Beijing) endpoint: `wss://{WorkspaceId}.cn-beijing.maas.aliyuncs.com/api-ws/v1/realtime`
- Singapore endpoint remains configurable: `wss://{WorkspaceId}.ap-southeast-1.maas.aliyuncs.com/api-ws/v1/realtime`
- Authentication: `Authorization: Bearer <API key>` during the WebSocket handshake
- Model query: `?model=qwen3.5-omni-plus-realtime` (the configured model remains overridable)
- Input: 16 kHz, mono, 16-bit PCM encoded as Base64 in `input_audio_buffer.append`
- Output: 24 kHz, mono, 16-bit PCM from `response.audio.delta`
- Manual turns: `input_audio_buffer.commit`, then `response.create`; interruption uses `response.cancel`

Official references:

- <https://www.alibabacloud.com/help/en/model-studio/realtime>
- <https://www.alibabacloud.com/help/en/model-studio/client-events>
- <https://www.alibabacloud.com/help/en/model-studio/server-events>

The adapter uses `github.com/gorilla/websocket` v1.5.3 (BSD-2-Clause) for the
RFC 6455 transport. It is a small pure-Go server dependency and is not linked
into the RV1106 C++ client.
