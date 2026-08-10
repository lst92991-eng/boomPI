# 第2章 原理图介绍

第1章从整板角度认识了 boomPI，这一章把开发板拆开来看。阅读原理图时，不必从第一个电阻一路追到最后一个电容；更实用的方法，是先找出一条完整的功能链，再顺着电源和信号的去向理解每个模块为什么这样连接。下面依次查看供电与启动、系统存储、有线与无线通信、摄像头和显示接口，最后再说明40Pin与音频接口的物理边界。

本章使用 `SCH_Schematic1_2026-08-10.pdf` 及同版 `netlist.json`。原理图回答“这块板打算怎样连接”，器件手册回答“芯片要求怎样连接”；真正装在板上的物料、驱动是否绑定以及功能是否可用，将在后续上板实验中确认。

## 2.1 先用页码地图建立整板印象

boomPI 原理图共有12页。图2-1把各页缩在同一张地图上，方便在纸面和PDF之间来回定位。第7页只有“定位孔”标题，没有电气对象；其余11页组成开发板的完整硬件连接。原理图中相同的蓝色网络名属于同一条电气网络，即使它们画在不同页，也可以沿着网络名继续追踪。

[[IMAGE:assets/schematic-clean/schematic-page-map.png|图2-1 boomPI 12页原理图的页码地图，第7页为空白定位孔页。]]

| 页码 | 主要内容 | 本章阅读位置 |
|---:|---|---|
| P01 | 5 V入口、各路电源、RTC与eMMC电压选择 | 2.2、2.3 |
| P02 | RV1106G3的供电、时钟、复位及主要接口 | 2.2～2.5 |
| P03～P06 | 无线、以太网、microSD与eMMC | 2.3、2.4 |
| P07 | 空白定位孔页 | 本节登记 |
| P08～P09 | 功放、麦克风连接器与40Pin排针 | 2.6 |
| P10～P12 | 摄像头、USB选择、屏幕与触摸接口 | 2.4、2.5 |

页码地图只负责回答“去哪里找”。接下来沿着板子工作的因果关系阅读：电源先建立，主控得到时钟并退出复位，随后才能读取存储中的系统，再通过网络和外接接口交换数据。

## 2.2 供电、时钟与复位怎样让主控开始工作

开发板由 Type-C 接口取得5 V电源。图2-2中，CC1和CC2分别接有5.1 kΩ下拉电阻，用来表明这一端是取电设备；D+、D−则经过静电保护后送往后面的USB选择电路。电源线与数据线虽然共用一个插座，却是两条相互独立的路径，因此切换USB数据去向并不会切断整板供电。

[[IMAGE:assets/schematic-clean/p01-typec-interface.png|图2-2 Type-C接口的5 V取电、CC下拉与USB数据线保护。来源：原理图P01。]]

进入板内的 `TYPEC_VBUS_5V` 先经过保险丝、肖特基二极管、滤波电容和浪涌保护器件，形成 `SYS_5V`。同一入口还分出 `OUT_5V` 和 `USBA_5V`：前者送往扩展接口，后者专供USB-A。三个网络的电压都写作5 V，但负载和保护边界不同，阅读时要按照网络名区分，不能把它们当成一根任意互换的导线。

[[IMAGE:assets/schematic-clean/p01-input-protection.png|图2-3 5 V输入经过保护和滤波后，分配到板内、扩展口与USB-A。来源：原理图P01。]]

RV1106G3内部包含处理器、片内DDR和多种I/O电路，这些部分需要不同的工作电压。图2-4中的四路降压输出把 `SYS_5V` 分成0.9 V、1.35 V、1.8 V和3.3 V。反馈电阻设定各路标称电压，输出电感和电容共同完成降压与滤波。这里应先记住各路电源的用途，而不是背诵所有元件值：0.9 V主要服务于核心电压域，1.35 V服务于片内DDR，1.8 V与3.3 V服务于不同I/O和外围电路。

[[IMAGE:assets/schematic-clean/p01-power-tree.png|图2-4 从SYS_5V生成0.9 V、1.35 V、1.8 V和3.3 V四组主电源。来源：原理图P01。]]

第1章已经把公开阅读资料中的RV1106G上电时序与本板RC延时放在一起说明。本章不再重复计算，只补充原理图中容易漏掉的连接：1.35 V和1.8 V两路使能由R19相连，因此板级表现为0.9 V、1.35/1.8 V、3.3 V三个使能阶段；R20又以0 Ω把 `VDD_0V9` 与 `VDD_ARM_0V9` 直接连接。换句话说，图纸表达了分阶段上电的设计意图，却没有为ARM 0.9 V再创造一个独立延时。各电压是否满足手册中的时间关系，仍要在同一次上电过程中用示波器观察。

[[IMAGE:assets/schematic-clean/p01-arm-0v9-filter.png|图2-5 VDD_0V9经R20直接送到ARM核心电源域，并配置就近去耦。来源：原理图P01。]]

主3.3 V之外，板上还有两条单独的3.3 V支路。图2-6中的降压电路为无线模块生成 `WIFI_BLE_3V3`，它随1.8 V阶段使能；图2-7中的另一颗降压器生成 `OUT_3V3`，并经过保险丝送到扩展接口。单独供电有助于把无线模块的瞬时负载、SoC主电源与外接扩展负载分开。两处采用的TLV62568是一颗同步降压转换器，TI[《TLV62568》Rev.B](https://www.ti.com/lit/ds/symlink/tlv62568.pdf)第1页给出的器件能力为最高1 A；这个数字是芯片条件下的额定能力，整板允许长期带多少负载还要同时考虑电感、散热、保险丝和PCB走线。

[[IMAGE:assets/schematic-clean/p01-wifi-3v3-supply.png|图2-6 无线模块使用独立的3.3 V降压支路。来源：原理图P01。]]

[[IMAGE:assets/schematic-clean/p01-out-3v3-supply.png|图2-7 OUT_3V3经过保险丝送往扩展接口。来源：原理图P01。]]

P01右下角还保留了RTC后备电源。主电源存在时，限流电阻和二极管给RTC电源侧供电；主电源断开后，纽扣电池继续维持低功耗时钟域。图纸明确标注ML1220，这是一类可充电纽扣电池；松下[ML1220数据表](https://mediap.industry.panasonic.eu/assets/imported/industrial.panasonic.com/cdbs/www-data/pdf/AAF4000/ast-ind-174504.pdf)把它列为可充电锂电池，而[CR1220数据表](https://energy.panasonic.com/dam/master/pdf/en/datasheet/lithium/CR1220_Datasheet_EN.pdf)把CR1220列为一次锂电池。两者外形接近，电气用途不同；在重新计算并验证充电条件以前，这个位置只使用图纸指定的ML1220，禁止换成CR1220或未经验证的替代品。

[[IMAGE:assets/schematic-clean/p01-rtc-backup.png|图2-8 RTC后备电源及ML1220充电通路。来源：原理图P01。]]

Type-C入口还有一条很细的小支路。5 V经电阻分压和电容滤波后形成 `TYPEC_USB_DET`，主控据此感知Type-C电源是否存在。这条线只负责“看见插入”，实际USB数据仍走D+、D−。

[[IMAGE:assets/schematic-clean/p01-typec-detect.png|图2-9 Type-C插入检测电路。来源：原理图P01。]]

电压稳定以后，主控还需要时钟与复位。图2-10中，24 MHz晶振提供主时钟，32.768 kHz晶振服务于低速时钟域，`nPOR`按键可以把芯片拉回复位状态。Recovery按键接到 `SARADC_IN0`，它不是普通GPIO按键；后续烧录章节会结合官方工具确认进入下载模式的操作条件。

[[IMAGE:assets/schematic-clean/p02-main-clock-reset-debug.png|图2-10 RV1106G3的24 MHz、32.768 kHz、复位、Recovery与调试连接。来源：原理图P02。]]

电源、时钟和复位只让主控具备开始工作的条件。复位释放后，它还要从断电后仍能保留内容的存储器中找到系统。

## 2.3 eMMC与microSD怎样保存系统

第1章已经介绍了RV1106 BootROM具备的启动与下载接口，本节只看boomPI实际接出的两种存储。eMMC焊在主板上，适合长期保存系统镜像和文件；microSD可以拔插，更适合替换介质和转移数据。具体启动顺序由平台启动方案决定，原理图本身不提供优先级。

eMMC的数据电源和I/O电源可以分开。图2-11用R25、R26的装配组合选择 `VDD_EMMC` 来自3.3 V还是1.8 V；图纸中R25标为不装、R26为0 Ω，表达的是1.8 V I/O方案。这个选择属于生产配置，实际板卡应由BOM和测量复核，带电时也不应改焊。

[[IMAGE:assets/schematic-clean/p01-emmc-voltage-select.png|图2-11 通过0 Ω与不装电阻选择eMMC的I/O电压。来源：原理图P01。]]

网表把板载eMMC标注为 `KLM8G1GETF-B041`。图2-12显示其关键连接：`EMMC_D0`～`EMMC_D7`组成8位数据通道，CLK提供时钟，CMD传递命令；VCC接3.3 V，VCCQ接前面选择的 `VDD_EMMC`。图2-13中的上拉电阻给CMD和D0提供默认状态，去耦电容靠近两组电源。8位连线说明总线宽度，不等于系统已经进入某种高速模式，实际时钟和读写速度要在Linux中读取控制器状态并测试。

[[IMAGE:assets/schematic-clean/p06-emmc-device.png|图2-12 eMMC的CLK、CMD、8位数据线与两组电源连接。来源：原理图P06。]]

[[IMAGE:assets/schematic-clean/p06-emmc-support.png|图2-13 eMMC的上拉与电源去耦电路。来源：原理图P06。]]

microSD使用4位数据通道。图2-14中的卡座引出CLK、CMD、DAT0～DAT3和插卡检测，供电为3.3 V。卡座旁的上拉和去耦服务于可拔插介质，插卡检测则让系统知道卡是否在位。它能否作为启动介质、支持多大容量以及使用哪种文件系统，留到镜像与硬件测试章节用当前板卡验证。

[[IMAGE:assets/schematic-clean/p05-microsd-card.png|图2-14 microSD卡座的4位数据、命令、时钟、插卡检测与供电。来源：原理图P05。]]

系统可以从存储器运行以后，开发板仍然是孤立的。下一组电路负责把它接入网线，或把同一组USB数据线交给无线模块和外接USB设备。

## 2.4 网线与USB无线链路怎样到达主控

RV1106内部集成百兆以太网MAC与PHY，所以P02上的 `ETH_PHY_TXP/TXN/RXP/RXN` 可以直接连到P04的HR911105A，中间不再放独立PHY芯片。HR911105A把隔离磁性器件、RJ45插座和指示灯装在同一器件中。汉仁[HR911105A官方产品页](https://www.hanrun.com/rj45_100/154.html)将这一型号列为10/100 BASE-T、非PoE；本板原理图也没有PoE受电电路，因此网线在这里负责通信，不负责给开发板供电。

[[IMAGE:assets/schematic-clean/p04-hr911105a-ethernet.png|图2-15 RV1106内置以太网PHY连接到HR911105A磁性RJ45。来源：原理图P04。]]

无线模块画在P03，图纸值为 `M8800DS2`。当前明确闭合到主控的数据线是 `WIFI_DP/WIFI_DN`，也就是USB D+、D−；符号上的SDIO、UART和PCM引脚没有在同版网表中形成通往主控的完整数据通道。左下角的天线连接器说明板上预留了射频出口，真实天线和认证状态仍以装配资料为准。

[[IMAGE:assets/schematic-clean/p03-m8800ds2-wireless.png|图2-16 M8800DS2无线模块的USB、电源、控制与天线连接。来源：原理图P03。]]

必联[BL-M8800DS2产品页](https://www.b-link.net.cn/product_38_320.html)列出了Wi-Fi 6与Bluetooth 5.4，但当前图纸只写 `M8800DS2`，制造商、完整料号和实物丝印尚未闭合。因此这份资料适合作为候选模块的能力背景；本板最终的无线版本与蓝牙通路，要由BOM、丝印和上板枚举共同确认。

这条无线USB并不独占主控接口。无线模块、USB-A和Type-C共用RV1106的一组USB 2.0 D+、D−，由TS3USB3031决定当前接通哪一路。图2-17只看两处：左侧D+、D−是公共端，右下表格说明两位选择信号把它接向哪组端口。TI[《TS3USB3031》Rev.D](https://www.ti.com/lit/ds/symlink/ts3usb3031.pdf)第10页给出的结果是：公共端可以接到USB1、USB2或器件内部名为MHL的第三端口；两位都为高时，所有通路进入高阻态。本板只是把“MHL”这个器件端口名用作一组普通差分通道接Type-C，并不由此获得MHL视频功能。

[[IMAGE:assets/manual/ti-ts3usb3031-revd-p10-function-table.png|图2-17 TS3USB3031的公共端、三条支路与四种选择状态。来源：TI《TS3USB3031》Rev.D第10页。]]

图2-18中的SW5是两位置双刀开关。一个位置把选择条件固定到Type-C，另一个位置把决定权交给 `USB_MUX_SELECT`，再在无线模块与USB-A之间二选一。开关朝哪个物理方向对应哪种状态，应以板上丝印或通断测量为准。

[[IMAGE:assets/schematic-clean/p11-usb-selector.png|图2-18 两位置SW5生成USB路径选择条件。来源：原理图P11。]]

图2-19把三条路线放到同一个器件周围：左侧公共端接 `SOC_USB_DP/DN`，右侧USB1接无线模块、USB2接USB-A、第三端口接Type-C。图2-20单独给出USB-A插座的D+、D−；它使用的 `USBA_5V` 已在图2-3的5 V分配中出现。复用器只切换D+、D−，不切换5 V。后续测试某个USB资源前，先确认SW5位置与软件控制状态，能够避免把“设备未枚举”误判成驱动故障。

[[IMAGE:assets/schematic-clean/p11-ts3usb3031-mux.png|图2-19 一组SoC USB数据线通过3:1复用器接往三条物理路径。来源：原理图P11。]]

[[IMAGE:assets/schematic-clean/p11-usba-connector.png|图2-20 USB-A插座的数据线。来源：原理图P11。]]

网口和USB解决了与外部设备交换数据的问题。对视觉开发板来说，还要有一条输入路线接收摄像头图像，以及一条输出和交互路线连接屏幕与触摸。

## 2.5 摄像头、屏幕与触摸分别走哪条线

摄像头、屏幕和触摸都接到主控，但传递的内容不同：摄像头把图像送进来，屏幕接收画面，触摸把手指位置送回去。原理图用三组网络名把这些路线分开，下面分别沿连接查看。

图2-21是触摸主控侧连接。`GT911_SCL/SDA`组成I²C控制总线，`GT911_RES`复位触摸控制器，`GT911_INT`在出现触摸事件时通知主控。图2-22是显示主控侧连接，`ST7789_SCK/MOSI`负责时钟与单向像素数据，RST、DC、CS和BLK分别承担复位、命令/数据区分、片选和背光控制。图中没有显示MISO，因此软件不应依赖从屏幕控制器读回寄存器。

[[IMAGE:assets/schematic-clean/p02-touch-interfaces.png|图2-21 RV1106G3触摸I²C、复位与中断的主控侧连接。来源：原理图P02。]]

[[IMAGE:assets/schematic-clean/p02-display-interfaces.png|图2-22 RV1106G3显示SPI与控制信号的主控侧连接。来源：原理图P02。]]

摄像头的数据路线与前两者不同。图2-23中，两组MIPI数据差分对和一组差分时钟负责搬运图像，MCLK给传感器提供主时钟，I²C用于写入传感器配置，RESET控制模组复位。这些信号最终落到P10的FPC1。图2-24同时送出3.3 V，但传感器核心、模拟和I/O究竟需要怎样的电压转换，必须由所选摄像头模组的准确资料确认；仅凭主板接口电压不能反推出裸传感器供电。

[[IMAGE:assets/schematic-clean/p02-camera-mipi-interfaces.png|图2-23 RV1106G3摄像头MIPI、I²C、MCLK与复位的主控侧连接。来源：原理图P02。]]

[[IMAGE:assets/schematic-clean/p10-camera-fpc.png|图2-24 外接摄像头FPC1的两lane MIPI、I²C、MCLK、复位与供电。来源：原理图P10。]]

P12的FPC2把屏幕与触摸装在同一连接器上。图2-25左侧是SPI显示信号，右侧是I²C触摸信号，中间共享电源与地。共用FPC只是节省连接器，并不代表两部分共用协议。原理图采用ST7789、GT911和SC3336作为接口命名或设计参考；实际外接模组的料号、针序、FPC方向和工作电压仍要与模组资料逐项核对。

[[IMAGE:assets/schematic-clean/p12-display-touch-fpc.png|图2-25 SPI屏幕与I²C触摸共用的FPC2接口。来源：原理图P12。]]

摄像头、屏幕和触摸都有明确用途，剩下的40Pin用于通用扩展。音频电路也画在原理图中，但在课程顺序上只先认清接法，把实际录放留到最后的音频阶段。

## 2.6 40Pin与音频接口先认清物理边界

P09是一只2×20、2.54 mm排针。1、17脚提供3.3 V，2、4脚提供5 V，其余位置分布地线和多种控制信号。它的针脚排布接近Raspberry Pi 40Pin，但“外形相近”不等于扩展板一定兼容；接入前还要核对针脚、电压、板卡尺寸和软件配置。GPIO属于3.3 V电压域，外部5 V逻辑信号必须先做电平转换。需要设计正式HAT时，再查Raspberry Pi[《HAT+ Specification》](https://datasheets.raspberrypi.com/hat/hat-plus-specification.pdf)。

[[IMAGE:assets/schematic-clean/p09-expansion-40pin.png|图2-26 40Pin排针上的电源、地与复用接口网络。来源：原理图P09。]]

播放路线从RV1106的 `SPEAKER_LINEOUT` 开始，经过隔直与电阻网络送入FM8002A，再由VO1、VO2接到外接扬声器座。图2-27是一条桥接输出路线，扬声器接在两个功放输出端之间，两端都不是地。图旁“2 W、4 Ω”是设计标注，不等于当前板卡在任意音量和温度下都已验证能持续输出2 W。

[[IMAGE:assets/schematic-clean/p08-fm8002a-amplifier.png|图2-27 RV1106模拟输出、FM8002A功放与外接扬声器座。来源：原理图P08。]]

录音路线由两个2Pin连接器分别引入。图2-28和图2-29中，每路都有MICBIAS、1.1 kΩ偏置电阻和小电容滤波；同时，MIC0_N、MIC1_N分别通过R49、R52的0 Ω电阻接地。因此本图表达的是两路带偏置的模拟麦克风连接，而不是两组完整悬浮差分输入。U9、U12只是连接器，也不能把它们当成已经焊在主板上的麦克风胶囊。

[[IMAGE:assets/schematic-clean/p08-mic0-connector.png|图2-28 MIC0外接麦克风的偏置、滤波及N端接地。来源：原理图P08。]]

[[IMAGE:assets/schematic-clean/p08-mic1-connector.png|图2-29 MIC1外接麦克风的偏置、滤波及N端接地。来源：原理图P08。]]

原理图把供电、存储、网络、图像和外接接口连到主控，但这些连接还不能直接供程序使用。下一章转向操作系统，看看Linux怎样把处理器、存储器和外设组织成程序可用的资源；音频录放仍留在课程最后。
