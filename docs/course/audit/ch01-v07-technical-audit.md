# 第 1 章 v0.7 技术事实审计

审计日期：2026-08-10

审计对象：

- Markdown：C:\Users\lst\Desktop\小智文档\tmp\boompi-chapter-review\ch01\chapter01.md
- DOCX：C:\Users\lst\Desktop\小智文档\tmp\boompi-chapter-review\ch01\第01章_开发板简介_审阅稿_v0.7.docx
- 最终核对渲染：C:\Users\lst\Desktop\小智文档\tmp\boompi-chapter-review\ch01\qa\v0.7\chapter01-v0.7c.pdf

## 1. 结论

当前 v0.7 的总体判定为：**REJECTED，修完 P0 后可进入下一轮技术复核。**

版式方面，v0.7c 共 8 页，逐页检查未见正文、表格或图片被裁切、遮挡。技术方面有两项会直接误导初学者，必须在发布前修正：

1. 把 RV1106G2 的 5 MP@30 fps 与 RV1106G3 的 8 MP@15 fps 写成同一 G3 型号下的两档取舍；
2. 把 U15 的 3:1 USB 数据复用器写成板上的“三选一开关”，没有说明实际是 SW5 与 SoC 控制信号共同决定路径。

其余问题主要属于证据边界和措辞精度：2 Gb DDR3L 不能直接等同于 Linux 可用 256 MB；8 GB 应写成标称容量；BootROM 数据手册只列支持项，不给启动优先级；RC 名义时间常数不能证明上电时序已经满足；四层板、阻抗和等长不能由原理图/netlist 证明。

## 2. 审计基线与证据等级

### 2.1 文件身份

| 文件 | SHA-256 |
|---|---|
| chapter01.md | B74ED79061496E4678E6E297CFE3F92AE6BE3B0E7AC2F5D9BEBC547A2D5F89D1 |
| 第01章_开发板简介_审阅稿_v0.7.docx | BD91FA64B6E6AFD187FA501156F0B1DB96333C2057041FBCB922B42EA4A7DC75 |
| chapter01-v0.7c.pdf | A3C94EDDB95E5AD6D67398035E23E1BE37CD3CC60C5AD80D0D5D91A8AD6CBBD5 |
| 当前 netlist.json | DCFB9113935DCAD01A9FBA462EA0D1DC20DF043963ABA90920F7DFF21280A801 |
| 当前 12 页原理图 PDF | 0B759890E6D15D5945C319F3D89096CB93E5D0158C35D46458F4A5332BDE486D |
| RV1106 Datasheet Rev.1.8 | 2CAC8EDED045E404DC67D4D4662684CBEF48FFB1737E3533F52E798E5BA4A017 |

### 2.2 证据等级

1. **芯片能力**：以 Rockchip RV1106 Datasheet Rev.1.8 的具体型号表、性能表和接口表为准。
2. **板级设计事实**：当前 12 页原理图和当前 netlist 只能证明“设计中这样连接”，不能证明器件已装配、信号完整性合格或功能实测通过。
3. **板端实测事实**：只引用 20260810-dhcp-recovery 目录中的原始记录和 technical-verdict.md。
4. **追溯限制**：板端 baseline 记录的主网表哈希为 f668a52a…5bcc，当前 netlist 哈希为 dcfb9113…a801。两者不一致，因此不能把当前板端结果写成“已验证当前这份 netlist 对应的硬件”。
5. **公开网页**：Rockchip 当前产品页主要沿用 G2/5M30 的家族宣传口径；型号级结论以 Rev.1.8 第 9、11、15 页为准。

优先级定义：

- **P0**：事实错误或会造成错误操作，发布前必须改。
- **P1**：证据边界、条件或来源不完整，应在本版关闭。
- **P2**：表达精度和教学性优化，可随本轮一起处理。

## 3. P0：发布阻断项

### P0-01：5M30 与 8M15 被写成 G3 的两个可选档位

位置：

- DOCX 第 4 页，1.2 节“图像输入由 ISP 和摄像头接口配合完成”段
- Markdown 第 39 行

当前问题：

正文先引用产品页“5 百万像素 30 帧”，再说 Rev.1.8 的 G3 “还列出了 8 百万像素 15 帧这一档位”，随后解释“分辨率提高时帧数下降”。初学者会把它理解为 RV1106G3 可以在 5M30 与 8M15 之间切换。

证据：

- Rev.1.8 第 9 页的视频编码性能按型号写明：G2 为 5 MP@30 fps，G3 为 8 MP@15 fps。
- Rev.1.8 第 11 页的 VICAP 最大输入按型号写明：G2 为 3072×1728@30 fps，G3 为 3840×2160@15 fps。
- Rev.1.8 第 15 页的订购信息再次把 G2 对应到 5M30，把 G3 对应到 8M15。
- Rockchip 当前产品页仍主要展示 G2/5M30 口径，不能据此推导 G3 存在一个已经确认的 5M30 模式。
- 当前板端证据只证明一次 576×324、NV12 单帧采集，未验证 5M30、8M15、连续视频或多路传感器。

必须直接替换为：

> Rev.1.8 按型号区分图像性能上限：RV1106G2 为 5 MP@30 fps，RV1106G3 为 8 MP@15 fps，见第 9、11、15 页。瑞芯微当前产品页仍主要展示 G2/5M30 的家族宣传口径，不能把它当作 G3 的另一档已验证模式。boomPI 当前只完成一次 576×324、NV12 单帧采集，尚未验证 5M30、8M15 或连续视频。开发板只引出一组摄像头连接器，也不能把芯片的多传感器接口能力写成板上已经具备多路摄像头。

禁止保留的结论：

- “G3 有 5M30 和 8M15 两档”；
- “把分辨率调低就一定能在 G3 上得到 5M30”；
- “boomPI 已验证 8M15”；
- “boomPI 已经具备三路摄像头”。

### P0-02：把 USB 3:1 复用器误写成三挡机械开关

位置：

- DOCX 第 3 页，图 1-1 后说明段
- DOCX 第 8 页，“接口数量有限时需要复用”段
- Markdown 第 13、69 行

当前问题：

“板上的三选一开关”容易让读者寻找一个三挡实体拨动开关。原理图中的 U15 是 TS3USB3031 3:1 USB2 数据复用器；SW5 是两位置控制件，且它还与 SoC 的 USB_MUX_SELECT 信号共同产生 U15 的 SEL0/SEL1。物理控制关系不是“机械开关直接三选一”。

当前 netlist 可确认的设计拓扑：

- U15 公共端连接 SoC USB2 数据线；
- USB1± 分支连接 Wi-Fi 模组；
- USB2± 分支连接 USB-A；
- MHL± 分支连接 Type-C 数据端；
- U3 的 USB_MUX_SELECT 参与 U15 控制；
- Type-C 的 5 V 供电路径与 USB 数据复用不是同一条电路。

必须直接替换为：

> Type-C、Wi-Fi 和 USB-A 复用 SoC 的一组 USB 2.0 数据通道。U15 是 3:1 USB2 数据复用器，不是三挡机械开关；SW5 与 SoC 输出的 USB_MUX_SELECT 共同决定 U15 的 SEL0/SEL1。按当前原理图，一种开关位置可把数据路径固定到 Type-C，另一种位置再由 SoC 在 Wi-Fi 与 USB-A 之间选择。开关方向、软件控制值和板端当前路径仍需实测确认。这个选择只改变 USB 数据去向，不等于切断 Type-C 的 5 V 输入。

图 1-1 中的“USB 三选一开关”建议改为“USB 3:1 数据复用器 U15”，把 SW5 单独标为“模式选择/控制”。

禁止保留的结论：

- “板上有一个三挡开关，三挡分别是 Type-C、Wi-Fi、USB-A”；
- “三路可以同时工作”；
- “拨到某个方向必然就是某一路”，除非增加板端实测和开关方向照片；
- “数据路径选择会切断 Type-C 5 V”。

## 4. P1：本版应关闭的问题

### P1-01：2 Gb DDR3L 与“256 MB 运行内存”的边界

位置：

- DOCX 第 3 页，图 1-1 后说明段与资源表
- DOCX 第 4 页，1.2 节首段
- Markdown 第 13、31 行

可确认事实：

- Rev.1.8 第 15 页明确列出 RV1106G3 集成 2 Gb DDR3L。
- 2 Gb 除以 8 等于 256 MB 名义物理容量。

当前问题：

“G3 封装内部已经集成 2 Gb DDR3L，也就是 256 MB 运行内存”容易被理解为 Linux 一定能看到完整 256 MB。实际 Linux 可用内存还会扣除固件、内核、保留区、CMA/媒体缓冲等占用。现有板端证据没有保存 MemTotal，不能给出当前镜像的 Linux 可用值。

建议替换为：

> RV1106G3 这颗器件集成 2 Gb DDR3L，折算为 256 MB 名义物理容量，所以原理图中看不到独立 DDR 芯片。Linux 实际可用内存会因内核、保留区和媒体缓冲而小于该值，以板端 /proc/meminfo 中的 MemTotal 为准。

表格“片内 DDR3L”建议改为“G3 器件内 DDR3L（2 Gb 标称）”。

补证命令：

    grep '^MemTotal:' /proc/meminfo
    dmesg | grep -i -E 'memory|reserved|cma'

禁止写“Linux 可用内存就是 256 MB”。

### P1-02：8 GB eMMC 应写成标称容量，不能等同于可用空间

位置：

- DOCX 第 2、3、8 页
- Markdown 第 7、13、67 行

可确认事实：

- 当前 netlist 的 U5 型号为 KLM8G1GETF-B041，设计为 8 GB 级 eMMC 器件。
- 板端 cmdline 记录 storagemedia=emmc、root=/dev/mmcblk0p7。
- 板端 /proc/partitions 记录 mmcblk0 为 7,634,944 KiB，符合标称 8 GB 级存储器经容量计量、保留区和分区后的表现。

建议统一为：

> 8 GB 标称容量的 eMMC

首次出现时补一句：

> “8 GB”是器件标称容量，不是 Linux 用户可以完整使用的文件空间；实际可用空间还要扣除引导区、系统分区和文件系统开销。

当前证据还不能确认：

- 当前实物 U5 的丝印与当前 netlist 完全一致；
- 当前板端镜像与 dcfb9113…a801 这份网表具有可追溯关系；
- eMMC 版本、速度等级、寿命或温度等级。

上述参数若要写入，必须补 Samsung 对应料号的官方数据手册或器件丝印照片。

### P1-03：MCU 的项目用途仍应收紧

位置：

- DOCX 第 4 页，Cortex-A7/MCU 段
- Markdown 第 35 行

Rev.1.8 可确认：

- 芯片存在 MCU；
- MCU 有 16 KB cache；
- CPU 中断也可连接 MCU；
- CPU 与 MCU 有 mailbox；
- 支持 MCU JTAG 调试。

当前 boomPI 仓库未发现独立 MCU 固件、构建、烧录或板端启动证据。因此“适合承担更靠近硬件、对响应时间更敏感的控制工作”只能作为一般能力说明，不能落成当前项目架构事实。

建议替换为：

> 芯片还带有 MCU，并提供中断、mailbox 和调试能力。当前仓库没有发现独立 MCU 固件、构建或部署证据，因此本章只介绍“芯片具备这项资源”，不把快速启动、电源管理、实时控制、摄像头或音频任务分配给它；后续只有在补齐对应固件和板端证据后，才能说明本项目怎样使用 MCU。

禁止写“当前项目由 MCU 负责某某任务”。当前正文未写 MCU 为 RISC-V，建议继续保持；如需增加架构类型，应引用与 G3 型号对应的正式资料。

### P1-04：四路 EN 引脚不等于四个独立时序阶段

位置：

- DOCX 第 6 页，图 1-4 后说明
- Markdown 第 57 行

当前问题：

EA3059 有四个 EN 引脚，但当前设计用 R19 0 Ω 把 VDD_1V35_EN 与 VDD_1V8_EN 相连。因此从本板的顺序控制看是三个阶段，不是四路完全独立调时。

建议替换为：

> 四路降压输出各有 EN 引脚；在 boomPI 的连接中，1.35 V 与 1.8 V 的 EN 通过 R19 0 Ω 相连，因此两路在同一控制阶段启动，整板形成 0.9 V、1.35/1.8 V、3.3 V 三个设计阶段。

### P1-05：RC 名义时间常数不能证明已满足手册上电时序

位置：

- DOCX 第 6 至 8 页，上电时序、RC 计算和结论
- Markdown 第 53、61、65 行

当前计算本身正确：

- 47 kΩ × 100 nF = 4.7 ms；
- 120 kΩ × 100 nF = 12 ms；
- 270 kΩ × 100 nF = 27 ms；
- R19 0 Ω 把 1.35 V 与 1.8 V 放在同一控制阶段。

但这些数值只是 RC 名义时间常数，不是电源轨达到有效电压的实测时刻。实际时序还取决于：

- SYS_5V 的上升沿；
- EA3059 的 EN 阈值、迟滞、泄漏和软启动；
- 电阻、电容和芯片阈值的误差；
- 各路负载和输出电容；
- nPOR 的外部 RC 与芯片内部 POR 行为。

当前 netlist 还显示 nPOR 由 R22 10 kΩ 上拉、C36 100 nF 对地，外部名义时间常数约 1 ms。这个值本身既不能证明，也不能直接否定图 1-4 中 T2=10 ms 的要求；必须结合完整官方硬件指南、nPOR 阈值和示波器波形判断。

图 1-4 可见的要求应准确保留为：

- T0、T1 均大于 1 ms；
- T2 为 10 ms；
- Trise 大于 100 μs。

当前正文写了“这些数值是设计估算，不是实测波形”，方向正确，但还应把“正好对应手册”改为更严格的条件句：

> 三组 R×C 表明设计意图是按 0.9 V/ARM、1.35/1.8 V/DDR、3.3 V 的顺序启动；它们不能证明 T0、T1、T2 和 Trise 已满足。正式验收必须在同一次上电中测量 VDD_0V9、VDD_ARM、VCC_1V8/VCC_DDR、VCC_3V3 与 nPOR，并把游标差值和上升时间保存在示波器截图中。

本项关闭条件：

1. 取得来源可追溯的完整《RV1103G/RV1106G Hardware Design Guide》V1.1；
2. 取得 EA3059 正式数据手册；
3. 用示波器测 T0、T1、T2、Trise；
4. 未完成测量前只能写“设计意图”，不能写“满足手册时序”或“已验收通过”。

### P1-06：BootROM 支持列表不是启动顺序，也不是自动回退承诺

位置：

- DOCX 第 8 页，“再看启动存储”段
- Markdown 第 67 行

Rev.1.8 第 6 页只确认：

- BootROM 支持的启动设备包括 SPI、eMMC、SD/MMC；
- 系统代码下载接口包括 USB、UART。

该页没有给出启动优先级、介质失败后的自动回退顺序、板级 strap 条件、Loader/Maskrom/Recovery 进入条件。当前板端只证实从 eMMC 启动。

建议替换为：

> Rev.1.8 第 6 页列出 BootROM 可支持 SPI、eMMC、SD/MMC 启动，以及 USB、UART 下载，但没有给出优先级或失败回退顺序。boomPI 原理图设计有 eMMC、microSD 卡座和 Type-C USB 数据路径；当前板端只证实从 eMMC 启动。microSD 启动、USB/UART 下载和故障恢复条件，必须结合官方 BootROM/SDK 文档、Recovery/strap 状态及 USB 复用器当前路径另行验证。

禁止写：

- “启动顺序是 eMMC → microSD → USB”；
- “eMMC 失败后会自动从 microSD 或 USB 恢复”；
- “Type-C 插线后一定进入下载模式”；
- “microSD 已验证可启动”。

### P1-07：四层板、阻抗和等长的来源链未闭合

位置：

- DOCX 第 8 页，“高速接口还要考虑 PCB 布线”段
- Markdown 第 71 行

当前可确认：

- USB、MIPI、eMMC、RMII/以太网等高速接口需要 PCB 级布线规则；
- 当前原理图/netlist 不能证明实际走线阻抗、等长、参考平面或板层数；
- 工作区没有找到 boomPI 的 PCB 源文件、Gerber、叠层表或制造阻抗记录。

当前不能确认：

- boomPI 实板一定是四层；
- 实际差分阻抗、单端阻抗或等长已经满足；
- “四层板是芯片厂建议”这一句对应硬件指南的准确页码和原文。

建议替换为：

> USB、以太网、MIPI CSI 和 eMMC 等高速接口需要在 PCB 阶段控制参考平面、阻抗、间距和长度。当前原理图与 netlist 只能证明器件连接，不能证明 boomPI 的实际层数、走线阻抗或等长结果。正式结论要以 PCB 源文件、叠层表、Gerber、制造阻抗要求或测量记录为准；在取得可追溯的官方硬件指南页码前，本章不把“四层板”写成瑞芯微对 boomPI 的确定要求。

若希望保留“四层板”，必须同时补：

1. 官方渠道取得的完整硬件设计指南及文件哈希；
2. 指南中对应四层示例或要求的准确页码；
3. boomPI 的 PCB 叠层或制造文件。

### P1-08：硬件设计指南截图的来源可追溯性不足

当前图 1-4 已改为干净的原图裁剪，视觉处理合格；但工作区中没有找到从 Rockchip 官方站点或对应 SDK 归档的完整指南 PDF。现有原始捕获来自公开在线阅读页面，不能单独承担“官方原文件已归档”的结论。

发布前有两种处理方式：

1. 从 Rockchip 官方渠道或可追溯 SDK 包取得完整 V1.1 文件，记录文件名、版本、页码和 SHA-256；
2. 在未取得前，把图注写成“《RV1103G/RV1106G Hardware Design Guide》V1.1 第 11 页公开阅读版节选；官方原文件待归档”，且不扩展截图中没有出现的结论。

图中能够直接读出的只有 T0、T1、T2、Trise 及各电源轨关系，不能据一张裁剪图替代整份指南的上下文。

### P1-09：当前板端证据不能替当前 netlist 背书

板端 baseline 保存的主网表哈希 f668a52a…5bcc 与当前 netlist 的 dcfb9113…a801 不一致。正文可以分别使用：

- 当前原理图/netlist 描述“设计连接”；
- 当前板端日志描述“这块被测板和当前镜像的实测状态”。

但禁止把二者合成：

- “当前这份 2026-08-10 netlist 已在板上验证”；
- “当前运行镜像依据当前 netlist 构建”；
- “板端通过证明本章全部原理图连接正确”。

关闭条件是完成两份网表差异审查、更新 BSP/硬件 manifest、重新构建并重新进行板端验收。

## 5. P2：建议优化项

### P2-01：多传感器数量不要写成芯片绝对上限

Rev.1.8 第 6 页概述了两路 MIPI CSI/LVDS 加一路 DVP 可同时接收三路传感器；第 12 页又给出多传感器复用 ISP“最多 4 路”的表述。两处描述指向的接口组合和复用条件不同。

建议正文不写“芯片最多接入三路传感器”这一绝对结论，改为：

> 数据手册描述了多传感器接入能力，但数量取决于接口组合和 ISP 复用条件；boomPI 当前只引出一组摄像头连接器。

### P2-02：SoC 与封装内存的教学措辞

“SoC 把处理器、内存控制……放进同一颗芯片”没有问题，但 RV1106G3 的 2 Gb DDR3L 更适合写成“G3 器件/封装内集成”，避免读者把“内存控制器”和“DDR 存储阵列”混成同一概念。

### P2-03：把“芯片能力”和“项目已使用”做成固定句式

建议本章后续统一使用三层句式：

1. “Rev.1.8 说明芯片具备……”；
2. “当前原理图设计把它连接到……”；
3. “当前板端证据只验证了……，其余待实测。”

这能避免把 NPU、MCU、多路摄像头、RTC、Audio Codec、BootROM 和 PCB 规则从“能力/设计”误写成“当前项目已启用/实测通过”。

## 6. 八项重点事实的最终判定

| 主题 | 判定 | 可以写 | 不能写或必须带条件 |
|---|---|---|---|
| RV1106G3 内存 | PASS，需改措辞 | G3 集成 2 Gb DDR3L，折算 256 MB 名义物理容量 | Linux 可用内存必然为 256 MB |
| 8 GB eMMC | PASS，需标称边界 | 当前设计 U5 为 8 GB 级 eMMC；被测板从 eMMC 启动，块设备约 7,634,944 KiB | 用户可完整使用 8 GB；当前实物和当前 netlist 已闭环追溯 |
| 5M30/8M15 | FAIL/P0 | G2=5M30，G3=8M15，按 Rev.1.8 型号表区分 | G3 有两档；boomPI 已验证任一高规格模式 |
| MCU 用途 | CONDITIONAL | 芯片具备 MCU、cache、中断、mailbox、调试能力 | 当前项目已由 MCU 承担某任务 |
| USB 三路复用 | FAIL/P0 | U15 是 3:1 数据复用器；SW5 和 SoC 控制共同选路 | 板上有三挡机械开关；三路同时可用 |
| BootROM | CONDITIONAL | 支持 SPI/eMMC/SDMMC 启动设备，支持 USB/UART 下载 | 数据手册给出启动顺序、自动回退；microSD/USB 已验证 |
| RC/上电时序 | CONDITIONAL | 4.7/12/27 ms 是名义 RC 时间常数，设计意图为三阶段顺序 | 已满足 T0/T1/T2/Trise；仅凭 RC 数值即可验收 |
| 四层板/阻抗 | BLOCKED | 高速接口需要 PCB 级规则；原理图不能证明层数/阻抗/等长 | boomPI 确认四层且阻抗合格；四层是已核实的芯片厂要求 |

## 7. 可直接执行的修订顺序

1. 先改 P0-01，把 G2 5M30 与 G3 8M15 按型号拆开。
2. 再改 P0-02，把“三选一开关”改为“U15 3:1 数据复用器 + SW5/SoC 控制”。
3. 把“256 MB 运行内存”改成“256 MB 名义物理容量”，增加 MemTotal 边界。
4. 全章把“8 GB eMMC”首次出现处改成“8 GB 标称容量的 eMMC”并解释可用空间。
5. 把 MCU 段收紧为芯片能力，不给当前项目分配职责。
6. 把“四路独立 EN”改成“四个 EN 引脚、三个设计阶段”。
7. 保留 RC 算术，改写为“设计意图”，明确 T0/T1/T2/Trise 必须示波器验收。
8. 把 BootROM 支持列表与板级启动/下载条件拆开。
9. 删去或暂缓“四层板是芯片厂建议”，直到官方指南与 PCB 叠层证据归档。
10. 修订后重新生成 PDF，逐页检查第 3、4、6、7、8 页，并重新跑本审计。

## 8. 仍需补齐的官方来源

下列资料需要官方网络查询或从对应 SDK/BSP 归档，不能用供应商字段、论坛转载或公开阅读截图替代最终证据：

1. 《RV1103G/RV1106G Hardware Design Guide》V1.1 完整官方文件；
2. RV1106 BootROM、Loader/Maskrom/Recovery 的官方流程及启动优先级说明；
3. EA3059 官方数据手册，重点是 EN 阈值、迟滞、泄漏、软启动和上电顺序；
4. Samsung KLM8G1GETF-B041 官方数据手册，确认容量、版本、速度与温度等级；
5. Texas Instruments TS3USB3031 官方数据手册，确认 SEL0/SEL1 真值表和高阻状态；
6. boomPI PCB 源文件、层叠、Gerber、阻抗/等长约束和制造验收记录。

已核对的 Rockchip 官方网页：

- RV1106 产品页：https://www.rock-chips.com/a/cn/product/RV11xilie/2022/0926/1661.html
- RV1106/RV1103 发布资料：https://www.rock-chips.com/a/cn/news/rockchip/2022/0217/1541.html

注意：产品网页的家族宣传参数不能覆盖 Rev.1.8 中 G2/G3 的型号级表格。

## 9. 审计依据

- C:\Users\lst\Downloads\netlist.json
- C:\Users\lst\Downloads\SCH_Schematic1_2026-08-10.pdf
- C:\Users\lst\Desktop\小智文档\tmp\boompi-chapter-review\ch01\sources\Rockchip_RV1106_Datasheet_Rev1.8.pdf
- C:\Users\lst\Desktop\小智文档\tmp\boompi-chapter-review\ch01\assets\manual\rv1106-hardware-guide-v11-page11-rv1106-crop.png
- C:\Users\lst\Desktop\小智文档\tmp\boompi-doc-v5\evidence\board\20260810-dhcp-recovery\board-baseline-raw.txt
- C:\Users\lst\Desktop\小智文档\tmp\boompi-doc-v5\evidence\board\20260810-dhcp-recovery\technical-verdict.md
- C:\Users\lst\Desktop\小智文档\tmp\boompi-chapter-review\ch01\technical-facts.md

本报告只审计技术事实和证据边界，没有修改第 1 章 Markdown、DOCX、原理图、netlist、代码或状态文件。
