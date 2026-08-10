# 第 1 章 v0.8 最终技术事实复核

复核日期：2026-08-10

## 1. 最终判定

**REJECTED。**

本轮没有剩余 P0；v0.7 的两项 P0 均已关闭。仍有 **1 项 P1**：正文先把四路降压输出写成“每一路都有独立的 EN 使能端”，后文才说明 1.35 V 与 1.8 V 的 EN 已由 0 Ω 电阻相连。对初学者而言，前一句仍会被理解为四路可以独立控制。

除这一句外，本轮指定复核的摄像头型号边界、USB 复用拓扑、2 Gb/8 GB 标称容量、RC 设计意图、BootROM 无优先级、PCB 层数边界和公开阅读版手册来源均已关闭。

完成下文 P1-01 的单句替换并重新生成 DOCX/PDF 后，可直接进行一次增量复核；没有必要重写其他段落。

## 2. 本轮基线

| 对象 | SHA-256 |
|---|---|
| chapter01.md | F0BB195F2CBC75549188A107E61266878805EB626B20473039857E2FB1B0062C |
| 第01章_开发板简介_审阅稿_v0.8.docx | 66E13506926983A5B14CFA12AEE49D0778857372D953CA017195B42288569D4F |
| chapter01-v0.8-clean.pdf | 2EFF5D7CC4C73C4FF552DAD79347274FF3EEC5477154BD4241092832B38ED93A |
| 当前 netlist.json | DCFB9113935DCAD01A9FBA462EA0D1DC20DF043963ABA90920F7DFF21280A801 |
| 当前 12 页原理图 | 0B759890E6D15D5945C319F3D89096CB93E5D0158C35D46458F4A5332BDE486D |
| RV1106 Datasheet Rev.1.8 | 2CAC8EDED045E404DC67D4D4662684CBEF48FFB1737E3533F52E798E5BA4A017 |

v0.8-clean PDF 共 10 页，已逐页检查。DOCX 内部文本与 PDF 可见内容均包含本轮关键修订，没有发现“Markdown 已修改但 DOCX/PDF 仍保留旧结论”的情况。

## 3. 剩余问题

### P1-01：四个 EN 引脚仍被写成四路“独立”使能

位置：

- Markdown 第 63 行
- DOCX/PDF 第 7 页，图 1-5 前一段

当前文字：

> boomPI 的 5 V 输入先进入四路降压电源，分别产生 0.9 V、1.35 V、1.8 V 和 3.3 V。每一路都有独立的 EN 使能端，只有 EN 达到有效电平，对应输出才会启动。

问题：

- EA3059 的确为四路输出提供四个 EN 引脚；
- 但当前板级设计用 R19 0 Ω 把 VDD_1V35_EN 与 VDD_1V8_EN 相连；
- 因此 1.35 V 与 1.8 V 不是两个可独立安排先后的板级控制阶段；
- 第 73 行虽已正确说明“三个设计阶段”，但不能依赖后文去抵消前文的错误第一印象。

直接替换为：

> boomPI 的 5 V 输入先进入四路降压电源，分别产生 0.9 V、1.35 V、1.8 V 和 3.3 V。四路输出各有 EN 引脚，但本板把 1.35 V 与 1.8 V 的 EN 相连，因此只有 0.9 V、1.35/1.8 V、3.3 V 三个控制阶段；某一阶段的 EN 达到有效电平后，对应输出才会启动。

关闭标准：

1. Markdown、DOCX 和 PDF 三处同步替换；
2. 第 73 行的 R19 0 Ω 与“三个设计阶段”说明保留；
3. 不再出现“四路独立使能”“四个独立阶段”等等价说法。

## 4. 已关闭项

以下只记录 v0.8 的关闭证据，不重复 v0.7 的完整论证。

| 复核项 | 状态 | v0.8 关闭证据 |
|---|---|---|
| G2/G3 摄像头规格 | PASS | Markdown 第 44、47 行；DOCX/PDF 第 5 页明确写 G2 为 5 百万像素 30 帧、G3 为 8 百万像素 15 帧，并禁止合并为 G3 两档 |
| USB 两位置控制件与 3:1 复用器 | PASS | Markdown 第 21、83、87、91 行；DOCX/PDF 第 3、8、9 页拆开 SW5 两位置控制与 U15 3:1 数据复用，且保留开关方向、软件值待上板确认 |
| 2 Gb DDR3L / 256 MB 边界 | PASS | Markdown 第 17、42 行；DOCX/PDF 第 3、5 页写成 256 MB 名义物理容量，Linux 可用值以 MemTotal 为准 |
| 8 GB eMMC 边界 | PASS | Markdown 第 17 行；DOCX/PDF 第 3 页写成 8 GB 标称容量，并说明需扣除引导区、系统分区和文件系统开销 |
| RC 与上电时序 | PASS，除 P1-01 措辞 | Markdown 第 73、75 行；DOCX/PDF 第 8 页明确 4.7/12/27 ms 只表示设计意图，不能证明 T0/T1/T2/Trise 已满足，要求示波器验收 |
| BootROM 启动边界 | PASS | Markdown 第 81 行；DOCX/PDF 第 8 页明确 Rev.1.8 只列支持项，没有优先级或自动回退顺序；当前板端只证实 eMMC 启动 |
| PCB 层数、阻抗与等长 | PASS | Markdown 第 99 行；DOCX/PDF 第 10 页明确原理图/netlist 不能证明实际层数、阻抗或等长，结论须来自 PCB/叠层/Gerber/制造或测量记录 |
| 硬件指南来源 | PASS | Markdown 第 59 行；DOCX/PDF 第 6 页图注写明“公开阅读版节选；官方原文件待归档”，未冒充已归档的官方原文件 |
| MCU 项目用途边界 | PASS | Markdown 第 35 行；DOCX/PDF 第 4 页明确仓库无独立 MCU 固件、构建或部署记录，不给当前项目分配 MCU 任务 |

## 5. P0/P1 汇总

- 剩余 P0：**0**
- 剩余 P1：**1**
- 结论：**REJECTED，完成 P1-01 的局部替换后可转 PASS**

## 6. 复核依据

- C:\Users\lst\Desktop\小智文档\tmp\boompi-chapter-review\ch01\chapter01.md
- C:\Users\lst\Desktop\小智文档\tmp\boompi-chapter-review\ch01\第01章_开发板简介_审阅稿_v0.8.docx
- C:\Users\lst\Desktop\小智文档\tmp\boompi-chapter-review\ch01\qa\v0.8\chapter01-v0.8-clean.pdf
- C:\Users\lst\Downloads\netlist.json
- C:\Users\lst\Downloads\SCH_Schematic1_2026-08-10.pdf
- C:\Users\lst\Desktop\小智文档\tmp\boompi-chapter-review\ch01\sources\Rockchip_RV1106_Datasheet_Rev1.8.pdf
- C:\Users\lst\Desktop\小智文档\tmp\boompi-doc-v5\evidence\board\20260810-dhcp-recovery\technical-verdict.md

本报告仅新增本审计文件，没有修改第 1 章 Markdown、DOCX、PDF、构建脚本、代码、AGENTS.md 或状态台账。
