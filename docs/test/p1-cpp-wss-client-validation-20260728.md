# P1 C++ WSS 单轮闭环验证记录（2026-07-28）

> 后续更新：真实 `boompi-client` 已在 RV1106 上完成三秒单麦手动单轮采集、流式上行、
> fake 24 kHz 下行和 48 kHz ALSA playback 写入与 drain。见
> [RV1106 手动单轮音频闭环 HIL 验证记录](p1-rv1106-manual-single-turn-hil-validation-20260728.md)。

- 时间：2026-07-28（Asia/Shanghai，UTC+08:00）
- 范围：最小 C++ WSS 传输、单麦单轮协议闭环、固定 OpenSSL 3.5.7、
  RV1106 交叉链接与真机离线 fake HIL。
- 初次验证的证据边界：真机仅执行独立 WSS smoke，并通过临时 SSH reverse tunnel 连接 VM 内的
  deterministic fake；没有访问 Qwen，也没有接入产品状态机、真实音频、正常局域网
  路由、发现/配对、重连或长期稳定性测试。

## 实现结论

- 客户端严格完成 `hello -> hello.ack -> turn.start -> 16 kHz PCM ->
  turn.commit -> response.start/text/audio/done`。
- `session_id` 与 `epoch` 来自服务端应答，不在客户端硬编码；上行 smoke 使用一个
  640 字节哨兵帧，服务端 fake backend 验证收到完整数据。
- 设备令牌只从 `BOOMPI_DEVICE_TOKEN` 子进程环境读取，不进入命令行；跨语言测试会
  过滤 DashScope 凭据，并且不创建 Qwen backend，不产生付费 API 请求。
- SPKI 模式固定校验服务器用途和 SHA-256 pin，并依靠 TLS 握手证明私钥持有；该模式
  有意不依赖证书有效期。CA 模式仍保留证书链、有效期和 hostname 校验，不能用来
  绕过校时。
- SNI 与 WebSocket `Host` 使用同一个经过有界安全 ASCII 字符集校验的 `server_name`；纯
  SPKI/IP 部署才回退到数值 IP，IPv6 authority 使用方括号。
- POSIX 等待改用 `poll`，不受 `FD_SETSIZE` 限制。唯一允许并发调用的
  `RequestStop()` 会用 socket shutdown 唤醒阻塞 I/O；离线 smoke 已实际覆盖阻塞
  `Receive()` 的取消路径。

## OpenSSL 依赖闸门

WSS 默认关闭；一旦显式打开，CMake 必须收到绝对路径
`BOOMPI_OPENSSL_ROOT`，并且只使用该目录中的 OpenSSL 3.5.7 CMake config、头文件
和静态 archive。配置过程禁止 system、BSP sysroot 或旧 CMake cache 回退，并检查：

- `OpenSSL::SSL`、`OpenSSL::Crypto` 必须是 imported static library；
- imported location 必须精确等于显式 package root 内的 `libssl.a` 与
  `libcrypto.a`；
- RV1106 构建还会在加载 package config 前核对两个 archive、两个配置头和两个
  CMake config 的固定 SHA-256，并对 `include/openssl` 的 172 个文件执行排序
  path+SHA-256 manifest 聚合校验，拒绝缺失、额外或混入旧版的头文件；
- `rv1106-release` 明确打开 WSS，缺少依赖会配置失败，不会生成一个没有 WSS 的
  假发布产物；
- 部署相关 executable 清空并禁止 build/install RPATH。

负向验证结果：

- WSS 打开但没有 `BOOMPI_OPENSSL_ROOT`：按预期配置失败；
- 把 native host 的 3.5.7 package 冒充 RV1106 固定 package：按预期在 SHA-256
  闸门失败；
- 在固定 package 的 OpenSSL include tree 中加入额外头文件：按预期在 manifest
  闸门失败。

## Ubuntu 离线验证

环境为 Ubuntu、GCC 11.4、CMake 3.22、Go 1.26.5，以及仓库外构建的静态
OpenSSL 3.5.7。使用 `-Wall -Wextra -Wpedantic -Werror`：

```text
host build: passed
CTest: 17/17 passed
ASan/UBSan CTest: 17/17 passed
go test -count=1 ./...: passed
go vet ./...: passed
Go race (C++ happy path and device handler): passed
protocol fixture + offline Python tests: 1 fixture set + 62/62 passed
```

跨语言 CTest 启动真实 Go TLS/WSS server 和 deterministic fake backend。测试证书
是 ECDSA P-256、serverAuth，并设置为执行时再过 24 小时才生效；C++ 客户端仍通过
精确 SPKI pin 完成握手和协议闭环，证明 pin 模式没有错误依赖墙钟。测试还确认：

- 合法格式但内容错误的 pin 在 TLS 阶段失败，provider 打开次数保持为零；
- provider 只打开一次；
- 收到完整 640 字节输入；
- 下行控制序列和 24 kHz PCM 标识一致；
- `RequestStop()` 在 500 ms 硬上限内终止一个已进入的阻塞 `Receive()`，并返回精确的
  cancellation code/message；
- 带 CRLF 的恶意 `server_name` 在连接前被拒绝。

## RV1106 交叉构建与 ELF 证据

环境为匹配 BSP 的 GCC 8.3.0、uClibc hard-float sysroot、ALSA 1.2.8，以及固定的
RV1106 OpenSSL 3.5.7 静态 package。Release/tests-off 全量默认构建在严格警告下
通过，其中 `boompi-wss-client-smoke` 进入默认 `ALL`，因此真正解析了完整 TLS/WSS
静态链接闭包。

对初次独立 WSS smoke 的 `--strip-unneeded` 部署副本运行 `verify_rv1106_elf.py` 与额外
动态段检查；这是历史 smoke 产物，不是后续手动音频 client：

```text
ELF:             ELF32 little-endian ARM EABI5 hard-float
interpreter:     /lib/ld-uClibc.so.0
max GLIBCXX:     GLIBCXX_3.4.22 (limit GLIBCXX_3.4.25)
file bytes:      4,244,740 (below temporary 6 MiB milestone cap)
text/data/bss:   3,964,255 / 273,272 / 5,636
RPATH/RUNPATH:   absent
libssl.so:       absent
libcrypto.so:    absent
developer paths: absent
```

动态依赖仅为当前 rootfs 已存在的 `libatomic.so.1`、`libstdc++.so.6`、
`libgcc_s.so.1`、`libc.so.0` 与 `ld-uClibc.so.1`。核心 `boompi-client` 也通过同一
ABI 校验，但当前 WSS 单轮入口仍由独立 smoke 组成；把它接入最终客户端状态机属于
下一实现阶段，不能把本记录解释成完整产品 runtime 已组成。

## RV1106 真机离线 HIL

板卡通过 `rv1106-board` SSH 恢复连接，环境为 Linux 5.10.160、ARMv7。当次短 smoke
读到板端时间 `2026-07-28T10:20:48+00:00`；后续掉电后的手动音频 HIL 又观察到墙钟
回到 2021 年，因此时间同步仍未关闭。该历史 smoke 的剥离目标以 SHA-256
`b8e5d9b52af7826e8c5c16c60c9f055cdb471fc71c089b2580893b69f92277db` 复制到 `/tmp`，
主机与板端哈希一致；它不是后续手动音频 client 产物。

板卡只有 link-local Ethernet，VM 位于 NAT 网段，因此本次用一次性 SSH reverse
tunnel 把板端 loopback 动态端口转到 VM 的动态测试端口。VM 运行同一个 Go app、
future-validity TLS identity 和 roundTrip fake backend；外层命令给板端进程设置 30 秒
硬超时。结果：

```text
board:  WSS_SMOKE_OK
server: BOOMPI_EXTERNAL_WSS_UPLINK_COMMITTED_AND_SESSION_CLOSED
```

服务端确认恰好一个 commit、完整 640 字节上行和 session close；板端进程确认完整下行
序列以及有界 `RequestStop()`。首次连接曾碰到已占用的固定测试端口，面对另一个 TLS
identity 时因 pin 不匹配在握手阶段失败；改用动态端口后才成功，没有降低校验或发送
`hello` 到错误身份。

另一次相同短 HIL 以约 10 ms 周期采样进程，从启动到单轮结束观察到
`VmRSS` 约 884 KiB 起步，`VmHWM` 峰值 6,108 KiB。按峰值加 15%、向上取 256 KiB
得到 7,168 KiB，可作为该独立 smoke 的暂定回归参考；它不是包含音频、UI、Snowboy
和产品状态机后的内存基线。

## 真机待关闭项

1. 重启后确认板端时钟能够保持，并用当前有效的 CA/hostname 证书验证完整 CA 模式；
   不得全局关闭 TLS 校验。
2. 在正常以太网/主机转发路径验证发现、配对、half-open timeout 与自动重连；本轮
   reverse tunnel 不能替代这些网络证据。
3. WSS 接入产品 runtime 后重新记录 connect 前、握手后、单轮结束后的
   `VmRSS`/`VmHWM`，再形成整机内存基线。
4. 手动单麦单轮已关闭，证据见
   [RV1106 手动单轮音频闭环 HIL 验证记录](p1-rv1106-manual-single-turn-hil-validation-20260728.md)；
   后续仍需关闭真全双工、VAD、打断、Snowboy 和双麦/AEC。
