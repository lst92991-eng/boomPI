# 教学版第一版集成验证

- 记录时间：2026-08-03 12:39:49 +08:00
- 分支：`codex/p1-fast-vertical-integration`
- 目标：学生只配置 Qwen API Key，即可启动本地服务端并让 RV1106 完成语音对话

## 本次落地范围

- 板端语音：双麦、单硬件参考 Rockchip 3A、Snowboy、VAD、500 ms pre-roll、流式 TTS、
  播放中打断和 3 s 追问。
- 板端交互：ST7789P3 七种简单状态、中文两行字幕、GT911 点击打断、音量和亮度手势。
- 板端联网：以太网优先、已保存 Wi-Fi 备用、UDP 服务发现、endpoint/SPKI 持久化。
- 首次配网：静态热点二维码、手机 HTTP 表单和凭据标准输入传递；不记录 SSID 或密码。
- 服务端：Windows/Linux/macOS 单文件构建；首次运行生成私有配置并退出，填 Key 后的第二次运行生成 TLS 身份；Qwen 公共
  新加坡端点、WSS 会话和 UDP 发现。
- 最小运维：`boompi-clientctl` 提供启动、停止、重启、状态、日志、单备份更新和配网；异常退出后
  总计最多启动三次，不实现企业级 supervisor 或 A/B OTA。

## 代码体量

统一口径为非空行减去纯 `//` 注释：

| 范围 | 文件数 | ELOC |
| --- | ---: | ---: |
| 客户端生产 C/C++ 总量 | 19 | 2500 |
| 其中 Rockchip/Snowboy vendor 集成 | 4 | 280 |
| 其余产品逻辑 | 15 | 2220 |

Shell/Python 辅助脚本不挪入 C++ 规避预算，单独公开：`boompi-clientctl` 146 ELOC，
`boompi-provision` 34 ELOC，`boompi-provision.py` 135 ELOC，开机脚本 6 ELOC。精确 C/C++ 文件集合与 2500 上限由
`scripts/tests/test_client_source_contract.py` 自动校验。

## 自动验证结果

- 客户端使用匹配 BSP 的 GCC 8.3/uClibc 工具链完成严格 RV1106 交叉构建，FreeType、ALSA、
  OpenSSL、Rockchip 3A、Snowboy 和 WebRTC VAD 链接通过。
- 服务端 `go test ./...` 与 `go vet ./...` 通过；Windows、Linux、macOS amd64 单文件交叉构建通过。
- 全新目录首启会创建权限为 `0600` 的 `config.yaml`；仅提供 API Key 的配置检查通过，教学版
  默认设备令牌与客户端一致。
- UDP 协议固定为请求 `BOOMPI_DISCOVER_V1`，响应
  `BOOMPI_SERVER_V1 <wss_port> <spki_base64>`；客户端使用 UDP 来源 IP，不信任包内主机名。
- Shell 语法、Python 编译和共享协议 fixture 校验通过。

## RV1106 实机结果

- 新客户端和脚本已安装到 `/userdata/boompi`、`/usr/sbin` 与 `/usr/lib/boompi`；所有动态库可解析。
- 客户端经 `boompi-clientctl` 启动并进入 `secure session ready`，Rockchip 3A 和 Snowboy 初始化
  成功；实测约 11.8 MiB RSS、5 个线程。
- UI 打开没有出现 display fallback 日志，说明 SPI/GPIO/字体初始化成功；CJK 字体位于
  `/userdata/boompi/fonts/DroidSansFallbackFull.ttf`。
- 配网冒烟中 `hostapd` 到达 `AP-ENABLED`，`wlan0` 使用 `192.168.4.1`，HTTP 配网页可访问，
  DHCP 服务启动成功。测试没有提交伪造凭据，因此没有覆盖用户保存的 Wi-Fi。
- 语音候选此前已完成人工真实 Qwen 问答、完整播放、3 s 追问和播放中打断；双麦加单参考后，
  用户确认未再观察到自激。该结论不替代最终壳体声学量化。

## 仍需人工验收

1. 手机实际扫描屏幕二维码，提交真实 2.4 GHz Wi-Fi，并确认断开以太网后自动关联。
2. 目视七种状态和中文字幕，实测 GT911 点击、音量与亮度手势。
3. 用本次新构建的服务端和有效 Qwen Key 完成一次付费端到端回归；本次没有重复消耗额度。
4. 在安静可控环境完成最大音量、残余回声、真人 double-talk、首音延迟和长时间稳定性测试。

上述四项是体验与量化验收，不代表当前实现仍缺占位模块。SC3336 多模态、长期记忆、企业配对、
签名更新和量产守护明确不属于本教学版交付。
