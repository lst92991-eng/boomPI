# 第 2 章原理图视觉覆盖与手册证据审计

审计日期：2026-08-10（以 19:18 后生成的第 2 章素材为快照）
审计对象：12 页 boomPI 原理图、现有干净裁剪、现有本地原厂手册与截图。
结论：**REJECTED（暂不满足“12 页全覆盖且每组都有可追溯手册截图”）**。已有素材已从“旧版红框教学卡”升级为 20 张直接裁剪，但仍有 1 个整块主控接口区未覆盖、5 个 P01 小模块未覆盖，且 8 张裁剪存在截断或混入旁块/外接模块文字的问题。手册方面只有 RV1106 Datasheet 与 TS3USB3031 Datasheet 有本地原 PDF；其余关键器件手册尚缺。

## 1. 审计边界与硬规则

- 原理图正文图必须来自 `SCH_Schematic1_2026-08-10.pdf` 的直接裁剪；不得再加红框、箭头、编号框、侧栏、图标或教学卡背景。
- 原理图原文件自带的器件外框、网络名和说明文字属于源内容，不算后加标注；但裁剪不应保留无关分区边框、半截标题或相邻模块。
- 每张图只回答一个问题；不能把 P02 整页缩成一张图让读者自行寻找接口。
- 手册截图必须来自可追溯原 PDF，图注写明厂商、文档名、版本和 PDF 页码。只有截图而无原 PDF 的材料必须注明证据限制。
- 原理图中的“兼容 Luckfox 摄像头模块”“ST7789”“GT911”等网络文字不能证明外接设备属于主板 BOM。FPC1/FPC2 是主板连接器；摄像头、屏幕、触摸芯片、扬声器、麦克风单体、插入的 SD 卡及 USB 设备均按外接件处理。

## 2. 来源与可追溯性

| 来源 | 位置 | SHA-256 | 结论 |
|---|---|---|---|
| 12 页原理图 | `C:\Users\lst\Desktop\小智文档\boomPI\docs\course\chapters\ch01\sources\SCH_Schematic1_2026-08-10.pdf` | `0B759890E6D15D5945C319F3D89096CB93E5D0158C35D46458F4A5332BDE486D` | 主原件；12 页，其中 P07 无电气对象 |
| 网表 | `C:\Users\lst\Desktop\小智文档\boomPI\docs\course\chapters\ch01\sources\netlist.json` | `DCFB9113935DCAD01A9FBA462EA0D1DC20DF043963ABA90920F7DFF21280A801` | 用于 BOM/网络边界复核 |
| RV1106 Datasheet | `C:\Users\lst\Desktop\小智文档\boomPI\docs\course\chapters\ch01\sources\Rockchip_RV1106_Datasheet_Rev1.8.pdf` | `2CAC8EDED045E404DC67D4D4662684CBEF48FFB1737E3533F52E798E5BA4A017` | 25 页原 PDF，可精确引用 |
| TS3USB3031 Datasheet | `C:\Users\lst\Desktop\小智文档\boomPI\docs\course\chapters\ch02\sources\TI_TS3USB3031_RevD.pdf` | `F06C08C4FAB085B049E285434A5AD7978EF6174408759333AE97B357F3974403` | 25 页原 PDF，Rev. D，可精确引用 |
| RV1103G/RV1106G Hardware Guide V1.1 | 仅有 `chapters\ch01\assets\manual\rv1106-hardware-guide-v11-page11-screenshot.png` | 仅衍生截图 | 可引用“第 11 页图 2-6”，但原 PDF 未归档；不得声称已完成整本手册核验 |

旧目录 `tmp\boompi-doc-v5\evidence\schematic\` 的 `*-nav.png`、`*-crop.png` 均带红框、编号或阅读侧栏，**全部只允许作定位索引，不得进入第 2 章正文**。`rv1106-power-sequence-guide-v11-p10-11.png` 是二次整理的蓝色教学卡，也不能冒充手册原页。

## 3. 当前素材盘点

当前 `chapters\ch02\assets\schematic-clean\` 有 20 张模块裁剪和 1 张页码地图。20 张模块图均没有后加红框/箭头/编号，像素尺寸足够；页码地图只作导航，不作元件或接口证据。

### 3.1 逐页覆盖矩阵

| 页 | 源页内容 | 当前干净裁剪 | 覆盖判定 | 必须补齐/修正 | 手册配套 |
|---|---|---|---|---|---|
| P01 | Type-C、输入保护、U14 无线供电、U4 四路电源、RC 上电、0.9 V ARM 滤波、RTC 电池、U16 3.3 V、eMMC 电压选择、Type-C 检测 | 4 张：Type-C+输入保护、U4、RC、eMMC 电压 | **不全** | 补 U14、U16、VDD_ARM_0V9、RTC 电池、TYPEC_USB_DET 五张；Type-C 图需去掉右侧半截相邻块，最好拆成“接口/ESD”和“5 V 输入保护”两张 | RV p20；Hardware Guide V1.1 p11；EA3059QDR 手册缺 |
| P02 | U3.2 时钟/RTC/PMU/GPIO；U3.1 电源/Codec/ETH/USB；U3.4 SDIO/显示/摄像头接口；U3.3 SARADC/eMMC/SD/recovery | 3 张：时钟复位调试、核心接口、存储恢复 | **不全，P0** | U3.4 整区缺失；至少拆成“SDIO/显示引脚”“摄像头/MIPI 引脚”两张，建议再把 U3.2 拆成“晶振+RTC+复位”和“PMU/GPIO/UART”以保证 100% 可读 | RV p6、p7、p11、p12、p13、p18、p19、p20、p23 |
| P03 | M8800DS2、USB 数据、电源/唤醒、未用 SDIO/UART、RF 连接 | 1 张 | **主体有，边缘不全** | 左下 RF 路径和 U2 被截断；重新裁到完整端点。正文必须明确该板实际数据网是 `WIFI_DP/DN`，SDIO/UART 引脚在本图为 NC | M8800DS2 原厂规格书缺；RV p13 仅能证明 SoC USB 能力，不能替代模组手册 |
| P04 | RV1106 内置 PHY 到 HR911105A 磁性体/RJ45 及双 LED | 1 张 | **通过** | 不要把源页左上角的通用 RJ45 示意再作为独立“官方手册图” | RV p13、p18、p20；HR911105A 规格书缺 |
| P05 | microSD 卡座、4-bit 总线、检测、上拉、去耦 | 1 张 | **通过** | 无需再画框；图注说明 CARD2 是主板卡座，SD 卡本体外接 | RV p7、p18；若讲 SD 电气/时序还需 SD 规范或器件资料 |
| P06 | KLM8G1GETF-B041 eMMC 与 D0/CMD 上拉、双电源去耦 | 2 张 | **主体有，P1** | 器件图左侧 `VDD_EMMC` 被裁成 `DD_EMMC`，需向左扩；支持电路图可保留原理图自带虚线分组 | RV p6-p7、p19-p20；Samsung KLM8G1GETF-B041 资料缺 |
| P07 | 标题为“定位孔”，无元件、网络或孔对象 | 无 | **正确留空** | 页码地图写“空白占位页，无电气证据”；不要为了凑数制造图片 | 无需手册图 |
| P08 | FM8002A 单声道功放、CN1 扬声器座、U9/U12 双麦克风连接器及滤波 | 2 张 | **电路完整，P1** | 两图左上标题均被截半；正文图可去掉标题并靠图注命名。U9/U12 是 2Pin 连接器，不是板载麦克风单体 | RV p12、p18、p23-p24；FM8002A 手册缺 |
| P09 | 40Pin、I2C/SPI/UART/PWM/PCM/GPIO、电源/GND | 1 张 | **主体有，P1** | 底部 R76/R77 端点被截断，需向下扩；“兼容树莓派”只能作为源页设计意图，不能推导完整电气兼容 | RV p13、p18-p19、p21；缺完整 pinmux/Hardware Guide 原 PDF |
| P10 | FPC1 摄像头连接器、MIPI/LVDS 两 lane、I2C、复位和电源 | 1 张 | **P0 重裁** | 当前图混入红色 Luckfox/SC3336 兼容文字和网址。重裁只保留 FPC1 与 I2C 上拉；外接 SC3336 仅在另段标成示例，不能写成主板器件 | RV p6、p11、p19、p21、p23；SC3336/外接模组资料缺 |
| P11 | SW5、TS3USB3031、Type-C/Wi-Fi/USB-A 三路、USB-A 座 | 3 张 | **主体有，P0/P1** | SW5 底部 GND 被截；USB-A 的 `USBA_5V` 顶部被截；U15 图混入原理图里粘贴的灰色功能表，需裁成纯电路，功能表改用 TI 原 PDF p10 | RV p13、p18、p22-p23；TI p3、p10、p15 |
| P12 | FPC2，ST7789 SPI 显示网与 GT911 I2C 触摸网 | 1 张 | **主体有，P1** | 底部 R78/R79 的电源端被截，需向下扩。ST7789/GT911 是外接屏/触摸方案网络名，不是主板 BOM 器件 | RV p12、p18-p19、p21；ST7789、GT911 资料缺 |

### 3.2 P0 与 P1

P0（进入正文前必须完成）：

1. 补 P02 U3.4 的显示/摄像头/SDIO 接口裁剪；这是整页主控接口链的实质缺口。
2. 重裁 P10，只留 FPC1 和板级上拉，移除外接模组宣称与网址。
3. 重裁 P11 U15 为纯 TS3USB3031 电路；不能用原理图内嵌灰表代替 TI 手册。
4. 将 TI p10 的功能表重新裁完整；现有 `ti-ts3usb3031-revd-p10-function-table.png` 底部没有完整呈现四行状态表。
5. 若正文要讲器件级工作原理，先归档 EA3059QDR、M8800DS2、HR911105A、KLM8G1GETF-B041、FM8002A 原厂资料；归档前只讲“这张板如何连接”，不写器件内部能力、阈值、增益或时序结论。

P1（排版前完成）：

1. 补 P01 五个小模块，使 P01 真正全覆盖。
2. 修复 P03 RF 端、P06 左侧电源名、P09 底部端点、P11 SW5/USB-A、P12 R78/R79 的裁切。
3. 去掉 P08 两图的半截源页标题；由统一图注承担命名。
4. 页码地图可保留作章首导航，但正文解释必须使用局部图，不能要求读者从缩略全页找器件。

## 4. 手册截图配套清单

以下是可直接执行的截图计划。每个截图保持原厂页眉或在图注中完整写出来源；正文图片不再加框。

| 建议资产 | 原文件与页码 | 应截内容 | 配套原理图组 | 当前状态 |
|---|---|---|---|---|
| `rv1106-rev18-p06-boot-emmc.png` | RV1106 Datasheet Rev1.8，PDF p6 | Overview、启动介质、eMMC 能力 | P02/P06 | 原 PDF 已有；`ch01/assets/manual/rv1106-overview-rev18-p06.png` 可复用 |
| `rv1106-rev18-p07-sdmmc-pmu.png` | 同上，p7 | SD/MMC 4-bit、PMU 三电压域 | P01/P02/P05 | 待从原 PDF 裁 |
| `rv1106-rev18-p11-mipi-csi.png` | 同上，p11 | 两组 MIPI CSI DPHY、2 lane/1.5 Gbps | P02/P10 | 待裁 |
| `rv1106-rev18-p12-display-audio.png` | 同上，p12 | Display interface 与 Audio Codec 两段；建议分两图 | P02/P08/P12 | 待裁 |
| `rv1106-rev18-p13-connectivity.png` | 同上，p13 | Ethernet、USB、SPI、I2C、UART；建议拆成网络/USB与低速总线两图 | P03/P04/P09/P11 | 待裁 |
| `rv1106-rev18-p14-block-diagram.png` | 同上，p14 | 官方 Block Diagram | 全章导航 | `ch01/assets/manual/rv1106-block-diagram-rev18-p14.png` 已有 |
| `rv1106-rev18-p18-pin-list-a.png` | 同上，p18 | SDMMC0、USB、Codec、Ethernet 相关管脚 | P04/P05/P08/P09/P11 | 待裁，表格需保证 100% 缩放可读 |
| `rv1106-rev18-p19-pin-list-b.png` | 同上，p19 | eMMC 与 MIPI/VI 相关管脚 | P06/P10/P12 | 待裁 |
| `rv1106-rev18-p20-operating-voltage.png` | 同上，p20 | Recommended Operating Conditions | P01/P02/P04/P06/P08 | 待裁；不要只截绝对最大额定值 |
| `rv1106-rev18-p21-gpio-mipi-electrical.png` | 同上，p21 | GPIO 电平与 MIPI 输入条件；建议分图 | P09/P10/P12 | 待裁 |
| `rv1106-rev18-p22-usb-electrical.png` | 同上，p22 | USB2.0 电气参数表起始部分 | P11 | 待裁 |
| `rv1106-rev18-p23-mipi-audio-electrical.png` | 同上，p23 | MIPI CSI 与 Audio Codec 参数；分两图 | P08/P10 | 待裁 |
| `rv1106-rev18-p24-audio-cont.png` | 同上，p24 | ADC/DAC 与功耗参数续表 | P08 | 待裁 |
| `rv1106-hwguide-v11-p11-sequence.png` | RV1103G/RV1106G Hardware Guide V1.1，p11，图 2-6 | RV1106G 上电时序原图 | P01 上电顺序 | `rv1106-hardware-guide-v11-page11-screenshot.png` 已有；原 PDF 缺，证据等级低一档 |
| `ti-ts3usb3031-revd-p03-pinout.png` | TI TS3USB3031 Rev.D，p3 | 12-pin VQFN pin configuration | P11 U15 | 原 PDF 已有，待裁 |
| `ti-ts3usb3031-revd-p10-function-table.png` | 同上，p10 | Functional Block Diagram 与 Table 6-1 四行状态 | P11 U15/SW5 | 已有图需重裁到底部完整四行 |
| `ti-ts3usb3031-revd-p15-layout-guidelines.png` | 同上，p15 | 去耦、等长差分、连续地平面等布局要求 | P11 USB 路径 | 已有且可读 |

## 5. 尚缺的原厂资料与写作边界

本地工作区、Downloads 和 `D:\训练codex\projects\docs` 按器件名检索后，未发现下列原厂 PDF：

- EA3059QDR：未取得资料前，不解释反馈公式、各通道额定电流或 EN 时序。
- M8800DS2：未取得资料前，只能依据板图说明 USB、供电、唤醒和 NC 引脚；不能推导 Wi-Fi/蓝牙版本或射频指标。
- HR911105A：未取得资料前，只说明板图中的磁性体/RJ45/LED 连接；不能把通用 RJ45 内部示意当作该型号官方页。
- KLM8G1GETF-B041：未取得资料前，可依据板图讲 D0/CMD 外接上拉和双电源，不把原理图红字当 Samsung 手册结论。
- FM8002A：未取得资料前，只讲单声道功放连接链，不写增益、输出功率、失真或关断阈值。
- ST7789、GT911、SC3336：均属于外接显示/触摸/摄像头方案证据，不能写入 boomPI 主板 BOM；如后续确实使用某个外接模块，应归档该模块的精确型号手册和接线定义。

这些缺口不阻止先讲“看懂主板连接”，但阻止将第 2 章标为“器件级设计依据已闭环”。

## 6. 建议的章内图文顺序

1. 先放页码地图，只告诉读者 12 页分别在哪里；明确 P07 是无电气对象的占位页。
2. 每组采用“手册原页局部 → 本板原理图局部 → 一句话判断”的三步结构。例如先看 RV p13 的 USB 能力，再看 U3 的 `SOC_USB_DP/DN`，最后看 U15 分到 Type-C/Wi-Fi/USB-A。
3. P01、P02 按电源流向和主控分片拆解；其他页一页一模块。不要在同一页交叉讲三条不同链路。
4. 每张原理图图注统一写：`来源：SCH_Schematic1_2026-08-10.pdf，Pxx，模块/网络名；直接裁剪，未添加标注。`
5. 每张手册图图注统一写：`来源：厂商《文档名》版本，PDF pxx，节/表/图号。`
6. 每个小节末尾只给一个“看懂即通过”的判断，例如“能沿 `SOC_USB_DP/DN` 找到 U15，再找到三个输出端”，不要求非技术读者一次记住器件参数。

### 6.1 推荐 22 图顺序

建议正文控制为 **22 个原理图图号**（手册原页截图放在对应图号之前，不计入这 22 个原理图图号）。P01 的小电路使用 a/b 子图并排，但每个子图仍是独立干净裁剪，不拼成带框教学卡。

| 顺序 | 建议图题 | 对应页/当前素材 | 处理要求 |
|---|---|---|---|
| 1 | 12 页原理图导航 | `schematic-page-map.png` | 仅导航，不作元件证据 |
| 2 | Type-C 接口与 5 V 输入保护 | P01 / `p01-typec-power-input.png` | 拆净相邻 U14 残片；必要时 2a 接口、2b 保护 |
| 3 | RV1106 四路核心电源 | P01 / `p01-power-tree.png` | 可用 |
| 4 | 0.9 V→1.35/1.8 V→3.3 V 上电延时 | P01 / `p01-power-sequence-rc.png` | 可用，前置 Hardware Guide p11 原图 |
| 5 | 无线与外设 3.3 V 电源 | P01 / U14、U16 | 当前缺；5a/5b 两张无框子图 |
| 6 | 辅助电源与检测 | P01 / ARM 0.9 V 滤波、RTC 电池、eMMC 电压、Type-C 检测 | 当前只有 eMMC 电压；6a–6d 子图，逐个解释 |
| 7 | 晶振、RTC、复位与调试 | P02 / `p02-main-clock-reset-debug.png` | 建议再收窄，避免混入整片 GPIO |
| 8 | 主控电源、Codec、Ethernet 与 USB 引脚 | P02 / `p02-main-core-interfaces.png` | 可用；按四象限从上到下讲 |
| 9 | 主控显示、摄像头与 GPIO 引脚 | P02 U3.4 | 当前整区缺，P0；至少 9a 显示、9b MIPI |
| 10 | eMMC、SD 与 Recovery | P02 / `p02-main-storage-recovery.png` | 可用 |
| 11 | M8800DS2 无线模块 | P03 / `p03-m8800ds2-wireless.png` | 扩到完整 RF/U2 端点 |
| 12 | HR911105A 以太网接口 | P04 / `p04-hr911105a-ethernet.png` | 可用 |
| 13 | microSD 卡座 | P05 / `p05-microsd-card.png` | 可用 |
| 14 | eMMC 器件与外围 | P06 / `p06-emmc-device.png`、`p06-emmc-support.png` | 14a/14b；修复左侧 `VDD_EMMC` |
| 15 | FM8002A 与扬声器座 | P08 / `p08-fm8002a-amplifier.png` | 去半截标题，电路可用 |
| 16 | 双麦克风连接器与滤波 | P08 / `p08-dual-mic-connectors.png` | 去半截标题；强调是连接器 |
| 17 | 40Pin 扩展接口 | P09 / `p09-expansion-40pin.png` | 扩到底部 R76/R77 完整端点 |
| 18 | 摄像头 FPC1 | P10 / `p10-camera-fpc.png` | P0 重裁，去掉外接模块宣称和网址 |
| 19 | USB 路径选择开关 | P11 / `p11-usb-selector.png` | 向下扩到完整 GND |
| 20 | TS3USB3031 三路复用 | P11 / `p11-ts3usb3031-mux.png` | P0 重裁为纯电路；TI p10 表单独作手册图 |
| 21 | USB-A 连接器 | P11 / `p11-usba-connector.png` | 向上扩到完整 `USBA_5V` |
| 22 | 屏幕与触摸 FPC2 | P12 / `p12-display-touch-fpc.png` | 向下扩到 R78/R79 的完整电源端 |

若版面必须压到 18 图，可合并 3+4、5+6、15+16、19+20+21 为四组 a/b/c 子图；不建议低于 18 图，否则 P01、P02 或 P11 会重新变成不可读的大图。

## 7. 本轮可接受的完成标准

- 11 个有效电气页都有至少一张可读局部图；P01/P02 的每个独立分区无遗漏，P07 诚实标空白。
- 所有正文原理图无后加框、箭头和编号，且没有半截网络名、半截器件端点、相邻模块残片或外接件误导文字。
- RV1106 和 TS3USB3031 的每条规格结论都能回到原 PDF 精确页；缺失器件手册的段落明确降级为“板级连接说明”。
- FPC/连接器与外接设备边界明确，不把外接模组写成主板 BOM。

在上述 P0 完成前，第 2 章可以继续写结构和通俗解释，但不应进入最终排版或标记为证据闭环。
