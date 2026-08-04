# boomPI 教学版软件收口记录（2026-08-04）

- 记录时间：2026-08-04 13:50 +08:00；最后更新 18:19 +08:00
- 分支：`codex/p1-fast-vertical-integration`
- 范围：客户端语音主线、Go 服务端、协议、构建与发布闸门
- 边界：13:50 基线仅静态部署；18:19 已增补真人 Qwen 闭环、连续播放、打断、LVGL 触摸、
  摄像头和实时音量最终验收。

## 本轮关闭的问题

- 客户端 actor 在 ALSA 暂时无帧时仍推进 hello、heartbeat、首响、取消、drain、追问和 60 秒录音墙钟；录音超时只取消当前 turn，不伪造 commit，也不重建健康 WSS。
- touch wake 不再复用同一轮已取出的旧 VAD 帧；`ResetListener()` 只清旧判定，保留已经捕获的 PCM 和 sequence。
- drain 尾音仍可由触摸或真人打断；取消 fence 后的旧 response 事件不会污染下一轮。
- 播放中同一句近讲同时承担“取消旧 TTS”和“新 turn 输入”；客户端在
  `response.cancelled` 前有界缓存 AEC 后 PCM，ACK 后立即以新 epoch 发送，不再要求用户重复第二遍。
- direct Qwen 握手改为异步；首个媒体操作最多等待 500 ms。上行或接收队列拥塞只退役当前 turn，保留健康的 WSS 和 provider 会话。
- 复现并修复 TTS 阶段性卡顿：两个 Qwen 后端都按 960 bytes/20 ms 产生音频事件，旧 pacing 却要求缓存大于 960 bytes 才发送，导致完整帧等待下一 delta。现在完整 20 ms 帧立即发送；若 provider 在整帧边界后才报告结束，则用 sequence 连续的 2-byte 静音 S16_LE `END` 包封口。
- 删除未接入生产路径的 pairing/tools/update 占位代码、旧手动单轮入口和第二套非流式 TTS/reasoning 路径。
- 摄像头临时日志迁到 `/run`；LVGL 和重建后的 WebRTC VAD 使用 `-ffile-prefix-map`，最终 ELF 不再泄漏开发机绝对路径。
- LVGL 小智页新增 0–100% 实时音量滑块和真实静音/恢复。拖动只覆盖一个合并邮箱、松手才
  持久化，避免 UI 事件或 flash 写入阻塞音频；滑块触摸区域不再误触旧亮度手势。
- 服务端 20 ms 下行 pacer 增加 15 ms 最小追赶间隔，修复桌面定时器迟到后产生 5–10 ms
  连发的跨平台问题，同时保留最多 5 ms 的渐进追赶。

## 代码体量

音频主线固定为 15 个生产 C/C++ 文件，共 3078 ELOC：

- 产品胶水：2798 ELOC；设计目标 2500，CI 硬线 2800。
- Rockchip/Snowboy vendor ABI：280 ELOC；CI 硬线 300。
- 合计硬线：3100 ELOC。

LVGL UI、网络启动和测试代码独立统计。当前相对 2929 ELOC 历史候选净增 149 ELOC，
用于展开状态转换、序号、缓冲和错误恢复路径；没有通过压行或挪目录规避计数。

## 自动化证据

- Client Debug：41/41 CTest 通过，包含 ACK 前结束和跨 ACK 长句两条同句打断回归。
- Client Release：34/34 CTest 通过。
- Client ASan + UBSan：34/34 CTest 通过，无 sanitizer 报告。
- Client Debug 核心场景重复 10 轮：330/330 通过。
- Source contract：3/3 通过；协议 fixture：2/2 通过；Ubuntu Python unittest：77/77 通过。
- Server：`go test ./...`、`go vet ./...`、`go test -race ./...` 通过；pacing、cancel、warmup、拥塞和 fence 场景重复 20 轮通过。
- Server 跨平台构建：Linux amd64、Windows amd64、macOS arm64 通过。
- RV1106 `rv1106-candidate` 严格配置、构建和显式 `boompi_protocol_json_test` 通过。

13:50 基线严格交叉构建输入源码 content manifest SHA-256：

```text
87159fea99c94b46c57ec82a71a451236ac62cd76b4d6155583afc246700d330
```

13:50 基线 RV1106 产物：

```text
raw boompi-client     e5f93c1684d19579c7ed717a95bbfa27d5714edc03e34014720d8ef9c78d1765
stripped client       a5771878e024c6b46c0509b542be8621bc53adf935487b3b8ee1c47283e03d3a
protocol JSON target  a63fad750cac201e63f0fac604de9ccc25f9c67af4a3cd368a6534ee637ca408
clean WebRTC VAD      1545d6ca028f1f1a4d6b68dc6246949730f8edbcabf82e4d5aa6ba2ab94dc5bf
```

ELF verifier 返回 `compatible=true`、`failed_checks=[]`：ELF32 little-endian ARM、EABI5、hard-float、uClibc 均匹配；没有 glibc、RPATH/RUNPATH 或开发机绝对路径。

交叉构建后只刷新了 `README.md` 和本文档；逐文件 SHA 比较确认代码、CMake、协议和其他构建输入均未变化，因此没有用文档修改掩盖未重建的代码。

## 板端部署与最终验收

18:08 前最终产物位于本机 Git 忽略目录
`build/boompi-client-volume-slider-v2.stripped`。已通过 `boompi-clientctl update` 部署到
`/userdata/boompi/bin/boompi-client`；本机 stripped 产物和板端 SHA-256 均为
`e28a7d64afe30c4d552ae0998d93e122143db591c2cfa747e836e727f84fe96f`。客户端以音量 79%、
Snowboy 0.7、VAD `-35 dBFS`、barge-in `-25 dBFS` 运行并进入 `secure session ready`。
用户完成真人闭环后确认问答、连续播放、无明显自激、播放中同句打断、新问题提交、屏幕触摸、
摄像头和实时音量滑块均通过。

发布终审随后只修正摄像头页面的真实预览标注（`30 FPS` → `4 FPS`）并同步 CMake 版本；没有
改变音频、触摸或摄像头管线。终审源码再次通过严格交叉构建和 ELF verifier，stripped
`boompi-client-v1.0.0` 为 5,345,336 bytes，SHA-256
`b8476d42a4520669ed02a8aabd52e5d829fafa5b9252d76bd0cd1d70cb245a37`。板端物理链路此后断开，
所以人工验收仍准确绑定到上一段的 `e28a...` 产物，不把终审标签修正冒充新的真人 HIL。

如需重新部署，可在仓库根目录执行：

```powershell
scp build/boompi-client-volume-slider-v2.stripped rv1106-board-3:/tmp/boompi-client
ssh rv1106-board-3 "/usr/sbin/boompi-clientctl update /tmp/boompi-client && /usr/sbin/boompi-clientctl status"
```

如需手动重启并打开日志：

```powershell
ssh rv1106-board-3 "/usr/sbin/boompi-clientctl start"
ssh rv1106-board-3 "/usr/sbin/boompi-clientctl log"
ssh rv1106-board-3 "/usr/sbin/boompi-clientctl stop"
```

当前已验收正常问答、连续播放、无明显自激以及播放中同句打断提交。
最终壳体 ERLE、受控噪声/double-talk 和长时稳定性仍是后续量化项，不由本次主观验收代替。
