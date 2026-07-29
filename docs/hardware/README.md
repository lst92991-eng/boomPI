# 硬件资料与验证记录

本目录保存 boomPI 产品代码所依赖的硬件事实。开发机下载目录中的原始网表、BSP
临时脚本和板端探测结果不是产品代码的运行时依赖；需要用于实现的结论必须先在这里
留下可审查记录。

## 事实来源优先级

1. 当前主板的 `netlist.json`、对应器件数据手册和实际 PCB。
2. 当前 BSP 的 DTS、驱动源码和编译配置。
3. 目标板 SSH 只读探测与仪器实测结果。
4. 历史网表或其他 RV1106 示例板仅作线索，不可直接形成引脚结论。

`netlist (3).json` 只用于双 16P 屏幕/转接板子系统；它不能覆盖主板
`netlist.json` 的结论。

## 当前记录

- [2026-07-29 BSP 候选镜像不可变清单](bsp-candidate-manifest-20260729.md)：固定最新网表、
  候选 DTS/overlay 和现有镜像哈希；当前因 MIS5001 残留和不可复现工作树被标记为拒绝烧录。
- [2026-07-25 基础硬件测试记录](hardware-test-record-20260725-154016.md)：板卡/镜像版本缺失的
  历史单项 bring-up，只能作为线索，不能替代当前镜像验收。

相关音频和板端证据：

- [P0 vendor 音频证据基线](../test/p0-vendor-audio-inventory-20260727.md)
- [P0 直接 ALSA 全双工验证](../test/p0-alsa-full-duplex-validation-20260728.md)
- [P0 Rockchip MPI preflight](../test/p0-rockchip-mpi-audio-preflight-20260728.md)
- [P0 Rockchip 3A HIL 构建验证](../test/p0-rockchip-3a-hil-build-validation-20260729.md)
- [RV1106 验证闸门](../test/rv1106-validation-gates.md)

新增记录必须写明北京时间、板卡版本、镜像或内核版本、接线与测试条件、原始命令或
仪器、结果和未覆盖范围。无法确认的信息写“未记录”或“未验证”，不得根据预期补齐。
