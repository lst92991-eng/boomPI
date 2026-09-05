# 客户端教学重构执行记录

目标：保留可用的语音、打断、追问、触摸、音量、Wi-Fi 和摄像头体验；服务端作为配套工具；客户端主线只有一个对话状态机。

基线：2026-09-05 工作树，25 个生产 C/C++ 文件、5048 ELOC；完整源码快照在 Git 忽略的 build/teaching-baseline-20260905。既有未提交改动被纳入基线。

## 实施计划

1. 完成唯一 v2 协议及服务端先切代再接收的边界测试。
2. 将网络发现、TLS、握手、心跳、重连与协议收敛到 VoiceLink。
3. 将固定声学 profile、pre-roll、完整打断探针收敛到 VoiceAudio；保留实时线程的硬件所有权。
4. 用 Offline / Idle / Listening / Uploading / Waiting / Speaking 重写 VoiceApp。
5. 收拢 UI 文本、配置和固定帧契约，删除旧协议和失效测试入口。
6. 运行 Host、Go、协议、交叉构建和 ELF 检查，准备同批客户端和服务端供人工验收。

## 预算

- application 约 500–600 ELOC，正常业务函数优先小于 80 ELOC。
- 学生无需调整声学环境变量；音量通过 UI，板级参数由维护者统一标定。
- 完整第一方客户端以 4200 ELOC 为方向，全部源码诚实计数；超出时说明具体责任，不压缩排版。
- 声学策略调整须与结构调整分开验证，不能仅凭 Host 测试宣称声学等效。

## 实施结果（2026-09-05）

- VoiceApp 只保留六状态，业务源码从975 ELOC / 1169物理行降到241 ELOC / 272物理行。
- VoiceLink 合并建链、TLS/WSS、握手心跳重连和单一v2协议边界。
- VoiceAudio 整体接管生产声学探针、25帧pre-roll和32帧打断历史，删除3 s cancel ACK等待。
- 配置只保留自动身份和教师可选固定端点；音量由UI持久化，6项声学profile由维护者统一管理。
- UI使用固定大小UiView快照；只保留小智、摄像头、时间、WiFi四个已实现入口。
- 服务端先切generation再有界接收新输入，普通提问保留历史，SUPERSEDE/STOP显式撤回。
- HIL直接使用生产VoiceAudio，固定60%音量；音源和超时按steady_clock推进，不复制打断状态机。

## 同口径体量

| 范围 | 修改前工作树 | 修改后 |
| --- | ---: | ---: |
| 第一方客户端生产文件 | 25 | 28 |
| 物理行 | 6588 | 5717 |
| 非空非注释行 ELOC | 5048 | 4563 |
| application业务实现 ELOC | 975 | 241 |
| application业务主状态 | 8 + 隐含Offline/取消模式 | 6 |
| 打断历史帧 | 181 | 32 |

统计包括所有第一方私有ALSA、3A适配、UI、网络源码/头文件，排除资源数组、第三方、测试、构建脚本和输出。复现：`python3 scripts/measure_client.py`；旧工作树可用 `--root build/teaching-baseline-20260905`。

完整代码比4200方向值多363 ELOC（8.6%）。保留的主要成本是音频线程/中断生命周期、SPI/GT911与摄像头实现；这些没有被移入third_party或从统计中隐藏。继续缩减前必须先完成真板验收，不能用删除边界检查或压行换数字。

单列教学胶水（入口、application、audio、network、config；不含UI和私有platform）为2304 ELOC，达到约2500方向值。完整总量仍以上表4563为准。

## 已完成验证

- Linux/WSL GCC 13.3，`-Wall -Wextra -Wpedantic -Werror`，CTest22/22。
- 应用测试内含21个行为检查；JSON测试含46个schema正反案例，并读取共享v2 fixture做C++逐字节校验。
- 真TLS测试覆盖pin拒绝、握手超时、重连、连续PCM、旧代、STOP/SUPERSEDE保序、真实慢读背压、队列溢出和END顺序。
- Go test、vet、race全部通过；核心四包race重复10轮通过。
- 最新C++ VoiceLink与Go服务端真实WSS联测通过，不调用真实Qwen。
- 应用与新增音频边界案例通过AddressSanitizer/UBSan；Windows MSVC严格构建及两测试组通过。
- Python11/11及v2 fixture校验通过；当前文档链接、shell语法和git diff --check通过。
- LVGL8.2 SDL模拟器及实际SPI/I2C UI源严格Host编译通过；首页/小智/时间/WiFi预览已检查。
- HIL源使用假硬件后端完成Host编译链接与--help；没有把它当作真板声学测试。

## 产物与待验

`build/teaching-v2-release/boompi-server.exe` 已生成并在Windows执行--help。SHA256：
`971874FB4213881DBD0717DAACCEE9F9388A0C867D8848062401C6C802313E54`。

RV1106工具链、sysroot及私有音频库路径当前未配置；交叉配置明确失败，因此没有生成新的板端ELF、没有部署。不能使用旧build产物替代本版。提供SDK后运行 `sh scripts/build_teaching_release.sh`，完成ELF检查再使用同批客户端/服务端。

真人唤醒、句首、长回答、短/长打断、追问、安静不自激、低/零音量和UI/摄像头仍按 [人工清单](../test/host-validation.md) 验收。旧版本源码在基线快照与Git历史可恢复。未提交、未推送。
