# Host 与交叉构建验证

## 当前边界

2026-07-31 精简后，`boompi-client` 是绑定 RV1106 BSP 与 vendor 库的板端目标，不再维护
一套重复的 host fake 客户端。旧 C++ 单元测试依赖已经删除的通用音频框架，不属于当前
测试矩阵；历史报告只用于追溯设计取舍。

Host 侧仍负责三类与硬件无关的验证：

1. Go 服务端测试、静态检查和构建。
2. Python 协议 fixture、探针/HIL 脚本的确定性测试。
3. 文档、格式、敏感信息和源码体量检查。

2026-08-03 现行生产 3A adapter 由离线 fake 直接编译真实
`client/src/platform/rv1106/rockchip_voice_dsp.cpp`，校验 `init(16000,16,2,1)`、
`[mic0,mic1,refL]` 交织、768-short 输入、mask `1109`、STDT `0.70/0.50` 和硬参考
profile；当前该组 Linux fake 为 7/7 通过。该测试只关闭适配器 ABI/packing，不能替代真板声学。

## Host 命令

在仓库根目录执行协议和脚本测试：

```text
python scripts/verify_protocol_fixtures.py
python -m unittest discover -s scripts/tests -p "test_*.py" -v
```

在 `server/` 目录执行服务端验证：

```text
go test ./...
go vet ./...
go build -trimpath ./cmd/boompi-server
```

真实 Qwen smoke 必须由单独开关启用，并记录区域、模型和费用风险；默认测试不得读取或
打印 `DASHSCOPE_API_KEY`、设备 token 或私钥。

## RV1106 交叉构建

客户端必须使用匹配 BSP 的 uClibc 工具链，并显式提供下列外部输入：

- Rockchip 3A 头文件、固定 archive/shared library 与哈希。
- Snowboy 头文件、模型、资源和旧 ABI archive。
- WebRTC VAD 头文件与 archive。
- Boost header root。
- 固定 OpenSSL 3.5.7 CMake package root。

项目不把个人绝对路径写进 preset。完整、可复现的实参和 ELF 哈希见
[板端客户端 2000 ELOC 收敛与启动记录](client-under-2000-refactor-20260731.md)。交叉构建必须
至少打开 `-Wall -Wextra -Wpedantic -Werror`，检查 ARM EABI、uClibc loader、`NEEDED`、
`RPATH/RUNPATH` 和最终 SHA-256。

## Vendor 与 HIL 门禁

以下探针保留为独立、显式 opt-in 的证据工具，不链接进 `boompi-client`：

```text
sh -n scripts/probes/rv1106_p0_probe.sh
sh -n scripts/probes/rv1106_rockchip_mpi_audio_preflight.sh
sh -n scripts/hil/rv1106_alsa_full_duplex.sh
```

- host fake 只能验证脚本控制流或 ABI，不能证明 Codec、声学、AEC 或唤醒效果。
- raw MPI HIL 不得与占用 `/dev/mpi/*` 或 PCM 的 OEM 服务并发执行。
- 真板语音验收必须覆盖首轮问答、3 s 追问、长回复、播放中打断、噪声误触发、断网恢复
  以及至少一个长时间稳定性窗口。

## 2026-08-01 当前验收状态

当前候选为 17 个生产 C++ 文件、2257 ELOC，其中 Rockchip/Snowboy vendor 集成 458 ELOC，
产品核心 1799 ELOC。严格交叉构建、Mode1 四通道全双工、历史 2 mic + 2 ref direct 3A 调用、
`voice loop ready`/`secure session ready` 启动和 SIGTERM 自动恢复 mixer 已通过。有限低幅 997 Hz
安静样本在适配后得到 processed RMS `0.000435`、wake count `0`，只能证明硬链路/收敛行为；
真人 double-talk、最终壳体 ERLE/残余回声和长期稳定性仍未关闭。见
[P0 Mode1 硬件播放参考验证记录](p0-mode1-hard-reference-validation-20260801.md)。

## 2026-07-31 历史验收状态（原文保留）

1904 ELOC 候选已完成严格交叉构建、板端加载、Rockchip 3A/Snowboy 初始化、持久 WSS 和
空闲运行。真人语音回归尚未关闭，因此当前仍是测试候选，不是发布版。
