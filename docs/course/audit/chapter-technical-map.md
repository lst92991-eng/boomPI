# boomPI 教学文档 18 章技术事实与验证地图

更新时间：2026-08-10（Asia/Shanghai）
用途：供章节作者、技术审校和截图任务共同使用；本文不是正文，不替代每章的实测记录。
章节基线：docs/course/status/chapters.md（2026-08-10 18:05 版）。

路径约定：以 docs、client、server、scripts 开头的路径均相对仓库根目录 C:\Users\lst\Desktop\小智文档\boomPI；证据与旧稿位于仓库外的工作目录时使用绝对路径。

## 1. 本轮只读盘点结论

### 1.1 固定源快照

| 对象 | 本轮身份 | 能证明什么 | 不能证明什么 |
|---|---|---|---|
| 当前原理图 | C:\Users\lst\Downloads\SCH_Schematic1_2026-08-10.pdf；12 页；SHA-256 0B759890E6D15D5945C319F3D89096CB93E5D0158C35D46458F4A5332BDE486D | 这版设计中模块、网络名、连接关系和标注 | 实物一定按此装配、驱动已工作、性能达标 |
| 当前网表 | C:\Users\lst\Downloads\netlist.json；EasyEDA 2.0.0；SHA-256 DCFB9113935DCAD01A9FBA462EA0D1DC20DF043963ABA90920F7DFF21280A801 | 元件位号、封装字段、网络连接，可辅助核对原理图 | 供应商字段等于官方规格、BOM 已投产、当前板与它同版 |
| RV1106 官方数据手册 | C:\Users\lst\Desktop\小智文档\tmp\boompi-chapter-review\ch01\sources\Rockchip_RV1106_Datasheet_Rev1.8.pdf；Rev1.8，2025-03-12；SHA-256 2CAC8EDED045E404DC67D4D4662684CBEF48FFB1737E3533F52E798E5BA4A017 | 芯片厂商声明的 RV1106/RV1106G3 能力、电气和封装信息 | boomPI 实际启用了所有能力、板上接口一定达到手册上限 |
| 源码 | boomPI 仓库 v1.0.0，盘点时提交 0292e5d7836059c4daee724865e100766ea5ecb1 | 当前实现、默认配置、构建门禁、协议和测试约束 | 当前板正在运行这份源码、外设已通过实测 |
| 当前板端证据 | C:\Users\lst\Desktop\小智文档\tmp\boompi-doc-v5\evidence\board\20260810-dhcp-recovery | 2026-08-10 当次、当板、当镜像下记录到的窄范围结果 | 量产一致性、长期稳定性、换镜像后仍成立 |
| 当前主机证据 | C:\Users\lst\Desktop\小智文档\tmp\boompi-doc-v5\evidence\host-validation.md | WSL2/x86-64 下 host 构建、测试和 Go 构建结果 | RV1106 交叉构建、目标板运行、真实云端问答 |
| 历史发布/HIL | docs/releases/v1.0.0.md 与 docs/test 下带日期记录 | 当时指定代码、二进制和板卡的历史结果 | 当前板状态、最终发布二进制已经完成同一轮 HIL |

原理图页码地图：第 1 页供电；第 2 页 RV1106G3；第 3 页 M8800DS2 无线模块；第 4 页以太网磁性接口；第 5 页 microSD；第 6 页 eMMC；第 7 页定位孔空页；第 8 页音频功放与外接麦克风/扬声器连接器；第 9 页 40 针扩展；第 10 页摄像头 FPC；第 11 页 USB 数据复用；第 12 页显示与触摸 FPC。正文只放这些原图的干净模块裁剪，不加框、不改字、不用重绘图冒充原图。

### 1.2 证据等级

1. 原理图/网表：只能写“设计为”“连接到”“预留/引出”。
2. 官方手册：只能写“芯片/器件支持”“厂商给出的上限或条件”。
3. 源码/构建日志：只能写“当前实现配置为”“该环境构建/测试通过”。
4. 带日期板端证据：才可写“本轮板端在该条件下观察到/通过”。
5. 人工观察照片或视频：才可证明屏幕实际显示、扬声器实际发声等物理现象。

正文不要把这五类证据混成一句。例如，“FPC2 接出 ST7789 和 GT911 信号”是设计事实；“存在 /dev/spidev0.0”是系统枚举事实；“脚本退出 0”是命令结果；“肉眼看到完整彩条”还需要同轮实物照片。

### 1.3 当前板端事实边界

| 项目 | 当前状态 | 可以写 | 不能写 |
|---|---|---|---|
| 板卡身份 | PASS（窄范围） | hostname 为 luckfox pico；Linux 5.10.160 armv7l；设备树 model 为 LST RV1106 Custom Board，compatible 含 rockchip,rv1106g3 | 因 hostname 推断板型；说当前镜像来自当前网表 |
| DHCP/SSH | PASS（窄范围） | 单目标直连 DHCP 恢复；保存的 host key 下可执行 SSH 和 1 MiB 传输 | 已验证路由器、DNS、互联网、首次 host key 来源可信 |
| 以太网 | PASS（窄范围） | 本轮为 100 Mbps/Full；1 MiB 双向文件哈希一致 | 吞吐达到 100 Mbps、长稳、互联网均通过 |
| Wi-Fi | 扫描 PASS；连接 FAIL | wlan0/AIC8800 可枚举，扫描见 36 个 BSS，含 2.4/5 GHz | 已连上保存网络、已获 IPv4/网关、目标 SSID 可见 |
| eMMC/SD | 1 MiB 窄范围 PASS | /userdata 与 /mnt/sdcard 当次写入、回读、哈希、清理成功 | 容量/耐久/掉电/全盘通过；现有固定路径脚本可安全复跑 |
| 显示 | 命令 PASS；物理 BLOCKED | spidev 节点存在，8 MHz 测试脚本退出 0 | 屏幕已显示正确彩条、80 MHz 已实测 |
| 触摸 | FAIL | sysfs 发现名为 gt911 的 3-005d 节点，但 driver=none | GT911 已绑定、I2C 应答、触摸坐标可用 |
| 摄像头 | 发现 PASS；单帧窄范围 PASS | SC3336 media graph；一个无人占用且原格式已为 NV12 576x324 的节点采得一帧，三处 SHA-256 同为 67367551ce06dfdbfafdfdef6e43829fc9968ae8682852b7ae7cdbac44930066 | 固定是 /dev/video14；16 个 FD 等于 16 个相机；连续预览、帧率、RTSP、画质通过 |
| 音频 | 枚举 PASS；I/O BLOCKED | card0 名 rv1106-acodec，存在一个播放和一个采集 PCM 子流 | 当前 48 kHz/4 通道/Mode1/3A/声学效果通过 |

当前板端基线记录的是旧 netlist SHA-256 f668a52a…5bcc，与本轮当前网表 DCFB9113…A801 不一致。因此所有章节都不得写“当前实物与 2026-08-10 当前网表一一对应”。必须补板版号/BOM/装配记录或重新建立可追溯基线。

## 2. 建议的 18 章依赖顺序

本地图沿用状态台账中的章节号和标题，不另造第二套编号。学习主线为：

~~~text
01 开发板
 └─> 02 原理图
      └─> 03 操作系统 ─> 04 Linux ─> 05 Buildroot/镜像/HelloWorld
                                      └─> 06 逐项硬件测试
                                           └─> 07 需求与整体架构
                                                ├─> 08 LVGL/触摸
                                                ├─> 09 V4L2/摄像头
                                                └─> 10 配网/Go/云端
                                                     └─> 11 并发架构
                                                          └─> 12 协议/WSS
                                                               └─> 13 日志/性能/调试
                                                                    └─> 14 ALSA
                                                                         └─> 15 重采样/3A
                                                                              └─> 16 唤醒/VAD/状态机
                                                                                   └─> 17 TTS/打断/HIL
                                                                                        └─> 18 源码通读/二次开发
~~~

第 02 章只建立“看图找模块”的低门槛地图，不提前展开复杂电气推导。第 14～17 章是最后一个专题区；第 17 章的音频 I/O/HIL 又必须放在该区最后。第 18 章只是回看源码、总结扩展入口，不再新增硬件功能。任何章节都不得把音频实作前置到非音频硬件闭环之前。

## 3. 逐章技术地图

### 第 01 章 开发板简介

- **依赖/目标**：无前置；先让读者知道“这块板有什么”和“哪些是外接模块”。只讲一层因果。
- **可用事实源**：
  - 当前原理图第 1～12 页和当前 netlist.json。
  - RV1106 Datasheet Rev1.8：第 6 页 CPU/NPU 概览，第 11～13 页摄像头、显示、以太网、USB 与低速接口，第 14 页框图，第 15 页订货信息。
  - 当前板身份证据 board-baseline-raw.txt、terminal-board-identity.png。
- **已验证边界**：设计中心器件为 U3 RV1106G3；当前板设备树 compatible 含 rockchip,rv1106g3。官方订货表把 RV1106G3 写为 Cortex-A7、1.0 TOPS NPU、2 Gb DDR3L；2 Gb 是位数，名义容量约 256 MB，不能写成 2 GB。当前板可枚举 eMMC、microSD、以太网、Wi-Fi、显示 SPI、相机 media graph 和音频 PCM；工作状态见 1.3，不可统一写“全部正常”。
- **必须避免**：
  - 不把外接 SC3336 相机、ST7789P3 屏、GT911 触摸、两个麦克风和扬声器写成主板上的芯片。
  - 不把 40 针“尽可能兼容树莓派”写成电气/软件完全兼容。
  - 不把 SoC 手册能力写成项目实测性能；不把 NPU 能力写成本项目已经使用 NPU。
- **可执行验证/证据**：

~~~powershell
Get-FileHash -Algorithm SHA256 -LiteralPath 'C:\Users\lst\Downloads\netlist.json'
Get-FileHash -Algorithm SHA256 -LiteralPath 'C:\Users\lst\Downloads\SCH_Schematic1_2026-08-10.pdf'
~~~

~~~sh
tr -d '\000' </proc/device-tree/model; echo
tr '\000' '\n' </proc/device-tree/compatible
uname -srmo
~~~

通关条件是“资源图上每个模块都能落到原理图页码，并标明板载/连接器/外接”，不是外设全部通过。

### 第 02 章 原理图介绍

- **依赖/目标**：依赖第 01 章的资源名称；按电源→SoC→存储→网络/USB→显示/触摸→相机→音频连接器讲，先说“做什么”，再说“为何这样接”，最后才对应芯片手册能力。
- **可用事实源**：
  - 原理图页码地图和当前网表。
  - RV1106 官方数据手册。
  - 本地现有干净裁剪；重用前必须以源 PDF 页码和哈希反查，不能仅凭文件名判断版本。
- **可确认的设计事实**：
  - 第 1 页由 Type-C 5 V 经保护和 EA3059/TLV62568 电源树形成多路电源；第 2 页 U3 是 RV1106G3。
  - 第 3 页 M8800DS2 实际使用 USB D+/D−，图中 SDIO 未接；第 11 页 TS3USB3031 在 Type-C、无线模块和 USB-A 之间复用同一组 SoC USB 数据线。
  - 第 4 页 HR911105A 磁性网络接口接 SoC 内置 10/100 PHY 相关差分线。
  - 第 5/6 页分别是 4-bit microSD 和 8-bit eMMC；U5 标为 KLM8G1GETF-B041。
  - 第 10 页为 2-lane MIPI CSI 相机 FPC；第 12 页同一 FPC 引出 ST7789 SPI 显示与 GT911 I2C 触摸相关信号。
  - 第 8 页 FM8002A 单声道 BTL 功放去外接扬声器 CN1，U9/U12 是外接麦克风连接器。
- **未验证边界**：没有当前板装配 BOM/板号与当前网表的闭环；没有完整本地 M8800DS2、EA3059、面板模组等官方手册。网表供应商字段不能替代制造商数据手册。
- **必须避免**：
  - 不写“Type-C 支持 PD/高速 USB”；图中只能确认 5 V、CC/检测和 USB 2.0 D+/D−，未见 SuperSpeed/PD 控制器。
  - 不写 M8800DS2 一定支持某个 Wi-Fi/蓝牙版本或内部一定是 AIC8800；板端日志只显示当前驱动/芯片字符串。
  - 不凭信号名推断电源时序、最大频率、I2C 地址选择和模组兼容性。
- **可执行验证/证据**：本章以哈希、页码和网表交叉核对为通关；不做板端写操作。可用 ripgrep 在导出的 JSON 中定位位号：

~~~powershell
rg -n '"U3"|"U5"|"U15"|"FPC1"|"FPC2"|"CN1"' 'C:\Users\lst\Downloads\netlist.json'
~~~

每个模块小节至少附“PDF 页码＋位号＋官方手册页码/待补官方来源”，否则不能写“为何这样接”的确定结论。

### 第 03 章 操作系统简介

- **依赖/目标**：依赖第 01/02 章；解释操作系统负责启动、驱动、文件和进程，不进入命令细节。
- **可用事实源**：当前板 board-baseline-raw.txt；Linux 5.10.160/Buildroot 字样的 terminal-board-identity.png；仓库 README；后续需补 Linux/Buildroot 官方文档。
- **已验证边界**：当前板一次启动显示 Linux 5.10.160 armv7l 与 Buildroot dirty 版本。它只能说明当前镜像标识，不能说明来源提交、补丁集、安全支持周期或可复现构建。
- **必须避免**：
  - 不把 Linux、Buildroot 和固件镜像当成同一件事。
  - 不把 Linux 版本号当成系统健康证明。
  - 不写 Ubuntu 运行在板上；Ubuntu/WSL 是主机开发环境，板端是 Buildroot 系统。
- **可执行验证/证据**：

~~~sh
uname -a
cat /etc/os-release 2>/dev/null || true
cat /proc/cmdline
cat /proc/version
~~~

正文应把每条命令解释为“只读身份证”，不可由输出推导未显示的 BSP/SDK 版本。

### 第 04 章 Linux 系统简介

- **依赖/目标**：依赖第 03 章；只教后续测试必需的路径、设备节点、挂载、进程、权限、退出码和哈希。
- **可用事实源**：board-baseline-raw.txt、board-topology-raw.txt、listening-ports.txt；现有参考文档中的 Linux 命令只可作写法参考，结果必须由本轮证据替换。
- **已验证边界**：当前板可通过 SSH 执行只读命令，但首次 host key 的可信来源没有独立闭环；板端时钟显示 2021，不能用于本轮证据时间排序。
- **必须避免**：
  - 不硬编码 /dev/video14、/dev/input/eventN、eth0、wlan0、/dev/spidev0.0 后直接操作；先发现再选择。
  - 不把 root 权限等同于操作安全。
  - 不把命令“无输出”写成成功；必须记录退出码或通关句。
- **可执行验证/证据**：

~~~sh
pwd
id
ip -br link
cat /proc/partitions
mount
df -h
ps
~~~

所有教学命令都要在命令块前写明“运行位置、是否写数据、预期结果、失败如何退出/恢复”。

### 第 05 章 Buildroot、固件烧录与 HelloWorld

- **依赖/目标**：依赖第 01～04 章；先建立镜像身份门禁，再讲参考烧录，最后用 HelloWorld 证明“编译→传输→运行”最小闭环。
- **可用事实源**：C:\Users\lst\Desktop\小智文档\tmp\boompi-doc-v5\artifact.md；README.md 的交叉构建边界；docs/test/p0-feasibility-report-20260725.md；docs/test/rv1106-validation-gates.md；CMakePresets.json；client/cmake/toolchains/rv1106.cmake；当前板 identity 证据。
- **已验证边界**：
  - 当前教学候选镜像在 artifact.md 中为 REJECTED，不能把参考流程写成“本轮已烧录成功”。
  - 历史 2026-07-25 有 RV1106 Release 交叉构建/ELF 证明，但本轮主机证据未重做 RV1106 交叉构建。
  - 当前板运行镜像与当前 netlist 哈希不一致；没有镜像 manifest、BSP commit、分区表和板版号的完整链。
- **必须避免**：
  - 未通过 SHA-256、目标板型、分区/loader/SDK 版本门禁前，不给读者点“下载/擦除/升级”。
  - 不执行或示范修改分区、设备树、启动项、持久网络配置；这些都需单独授权。
  - 不编造 Rockchip 工具按钮、地址或镜像名；正式步骤必须来自匹配 SDK/工具官方说明。
- **可执行验证/证据**：

~~~powershell
Get-Item -LiteralPath '<待验镜像绝对路径>' | Select-Object FullName,Length,LastWriteTime
Get-FileHash -Algorithm SHA256 -LiteralPath '<待验镜像绝对路径>'
~~~

门禁未通过时到此停止，不提供实际烧录命令。HelloWorld 只在工具链/ABI 已确认后进行，并用 file/readelf/sha256sum 证明目标架构，再由用户授权的传输路径复制到板端临时目录；“打印一句话”不等于所有依赖与驱动正常。

### 第 06 章 登录后逐项测试硬件资源

- **依赖/目标**：依赖第 05 章的镜像/登录身份；固定内部顺序为：身份→存储只读→经授权的小文件写回→以太网→Wi-Fi→显示→触摸→摄像头。音频不在本章做 I/O。
- **可用事实源**：C:\Users\lst\Desktop\小智文档\tmp\boompi-doc-v5\evidence\board\20260810-dhcp-recovery\technical-verdict.md 及同目录原始文本/截图；tools 下本轮板端脚本；当前原理图对应页。
- **当前边界**：严格采用 1.3 的 PASS/FAIL/BLOCKED。尤其是 Wi-Fi 连接 FAIL、触摸 FAIL、显示物理 BLOCKED、摄像头连续预览 BLOCKED、音频 I/O BLOCKED。
- **必须避免**：
  - 不把扫描到热点写成已联网；不把 carrier=1 写成互联网可用。
  - 不把脚本退出 0 写成屏幕肉眼正确。
  - 不把 rkipc 打开的 16 个文件描述符写成 16 个相机。
  - 不复跑当前固定路径的 board_storage_rw_test.sh；其路径安全审计为 FAIL。
  - 不复跑当前 Wi-Fi 连接脚本或显示脚本，除非先修复恢复策略并取得写状态授权。
- **可执行验证/证据**：

~~~sh
# 先发现，不写状态
ip -br link
ip -4 -o addr
cat /proc/partitions
mount
ls -l /dev/spidev* 2>/dev/null
cat /proc/bus/input/devices
media-ctl -p 2>/dev/null
v4l2-ctl --list-devices 2>/dev/null
cat /proc/asound/cards
cat /proc/asound/pcm
~~~

网络接口、输入 event、video 节点和 SPI 节点必须从当次输出中选择后再填入后续命令。Wi-Fi 扫描会主动发射/改变无线活动，显示、存储写回、相机采帧都会写状态或数据，必须在各自步骤前单独标注。现有证据优先引用 ethernet-transfer.txt、storage-rw.txt、wifi-scan-sanitized.txt、wifi-connect-sanitized.txt、touch-binding.txt、display-colour-bars.txt、camera-single-frame.txt。

### 第 07 章 boomPI 项目需求与整体架构

- **依赖/目标**：依赖第 06 章的硬件现实；明确板端 C++ 客户端、局域网 Go 服务端、Qwen/DashScope 三个角色和信任边界。
- **可用事实源**：README.md；docs/architecture/system-overview.md；client/include 与 client/src；server；protocol/protocol-v1.md；docs/releases/v1.0.0.md。
- **已验证边界**：仓库能证明当前设计和实现职责；本轮 host 构建/测试通过。不能证明当前板运行该客户端，也没有本轮真实 Qwen 会话或板端 WSS 闭环。
- **必须避免**：
  - 不把相机图像说成上传云端；需以当前源码的数据路径为准。
  - 不把开发默认设备令牌当生产级每设备认证。
  - 不用未来占位图替代当前实现；每个方框须能落到目录/类型/协议事件。
- **可执行验证/证据**：

~~~sh
git rev-parse HEAD
git describe --tags --always
rg -n "17806|17807|BOOMPI_DISCOVER_V1|BOOMPI_SERVER_V1" README.md server client docs
~~~

架构图应给每条箭头标注方向、协议、数据种类和是否含敏感信息。

### 第 08 章 LVGL 图形界面与 GT911 触摸

- **依赖/目标**：依赖第 06 章显示/触摸结果和第 07 章状态职责；先在主机预览页面，再部署显示，最后接触摸。
- **可用事实源**：client/src/ui/lvgl_screen.cpp、device_ui.cpp；client/include/boompi/ui；client/CMakeLists.txt；CMakePresets.json；原理图第 12 页；touch-binding.txt、display-colour-bars.txt。
- **已验证边界**：
  - 源码的物理面板配置为 240×320、逻辑界面 320×240；源码请求 80 MHz SPI，当前板端证据只运行过 8 MHz 命令。
  - 源码用户态 fallback 写有 /dev/i2c-3、0x14，并注释当前烧录 DT 描述 0x5d；当前板证据只发现 3-005d 且 driver=none，不能证明 0x14 或 0x5d 真实应答。
  - host-debug 本轮 41/41 测试没有构建 UI simulator，因为 preset 中 BOOMPI_BUILD_UI_SIMULATOR=OFF；桌面预览仍需补依赖与截图。
- **必须避免**：
  - 不把源代码常量写成通用硬件事实。
  - 不用 i2cdetect 扫正在使用的 I2C 总线；可能干扰设备。
  - 不在触摸未绑定时写“点击按钮可用”。
- **可执行验证/证据**：

~~~sh
# 主机：具备 LVGL 8.2、Freetype、SDL2 后
cmake -S . -B build/ui-sim \
  -DBOOMPI_BUILD_TESTS=OFF \
  -DBOOMPI_BUILD_UI_SIMULATOR=ON \
  -DBOOMPI_LVGL_ROOT='<已校验的 LVGL 8.2 源码目录>'
cmake --build build/ui-sim --target boompi_ui_simulator
~~~

~~~sh
# 板端只读发现
cat /proc/bus/input/devices
for d in /sys/bus/i2c/devices/*; do
  test -r "$d/name" && printf '%s ' "$d" && cat "$d/name"
done
~~~

桌面预览通过不等于板端显示通过；板端显示命令通过也不等于触摸通过。三者分别截图/记录。

### 第 09 章 V4L2、摄像头与图像链路

- **依赖/目标**：依赖第 06 章相机发现与第 08 章显示；先讲 media graph 和资源所有者，再做一帧，最后才谈连续预览。
- **可用事实源**：原理图第 10 页；RV1106 手册第 11 页；client/src/ui/device_ui.cpp；camera-existing-service-readonly.txt、camera-spare-nodes-readonly.txt、camera-single-frame.txt、camera-frame-576x324.png。
- **已验证边界**：当前证据确认 SC3336 media graph，sensor pad 为 SBGGR10 2304×1296；一条无人占用节点在原格式已经为 NV12 576×324 时，采得 279,936 字节单帧，前后格式一致、三处哈希一致。源码仍硬编码 /dev/video14、请求 576×324 NV12，ffmpeg 输入标 25、滤镜目标 fps=4/320×180；这些是配置，不是实测帧率。
- **必须避免**：
  - 不固定 video14；每次从 media-ctl/v4l2-ctl/fuser 发现。
  - 不抢占 rkipc 正在使用的节点，不擅自 stop 服务。
  - 不把单帧成功写成连续预览、画质、帧率或 RTSP 通过。
- **可执行验证/证据**：

~~~sh
media-ctl -p
v4l2-ctl --list-devices
for n in /dev/video*; do
  printf '\n%s\n' "$n"
  fuser "$n" 2>/dev/null || true
  v4l2-ctl -d "$n" -V 2>/dev/null || true
done
~~~

采帧属于写文件/改变设备流状态；只有当节点无人占用、格式原本符合要求、临时文件唯一、格式前后核对且已授权时才能运行有界采帧。现有修复后的证据可作示范，不能把其中节点号抄成固定教程。

### 第 10 章 配网、Go 服务端与云端 AI 链路

- **依赖/目标**：依赖第 06 章网络与第 07 章架构；顺序为本机离线构建→配置摘要→局域网发现→WSS→可选真实云端。
- **可用事实源**：client/src/network/network_bootstrap.cpp；client/src/config/voice_client_config.cpp；server/README.md；server/configs/config.example.yaml；server/internal；protocol fixtures；host-validation.md。
- **已验证边界**：
  - 本轮便携官方 Go 1.26.5 下 go test、go vet、CGO_ENABLED=0 build 通过；Linux amd64 server SHA-256 为 78c6d25b2c8dd569c9aaa0041064faba08d4921bff3991487e71d9875a034941。
  - 源码默认 eth0/wlan0、/etc/wpa_supplicant.conf、/userdata/boompi/config/server.conf；这些必须在文档中写成“当前仓库默认”，并在实际板上先发现。
  - 当前板只证明 Wi-Fi 扫描，不证明 Wi-Fi 连接；本轮没有真实 Qwen 调用。
- **必须避免**：
  - 不展示 API Key、Workspace ID、设备令牌、Wi-Fi 密码或未脱敏 config.yaml。
  - 不把热点默认口令 boompi-setup 当生产安全方案。
  - 不把 --check-config 通过写成云端可达或模型可用。
- **可执行验证/证据**：

~~~sh
cd server
go test ./...
go vet ./...
CGO_ENABLED=0 go build -trimpath -o '<输出目录>/boompi-server' ./cmd/boompi-server
'<输出目录>/boompi-server' --check-config
~~~

运行服务端会创建本地 TLS 身份/config/state 并监听端口，属于本机写状态；教程必须给独立临时目录、端口检查和 Ctrl+C 恢复。真实 Qwen 步骤是可选、可能计费且必须先查官方当前文档。

### 第 11 章 多线程、实时性与并发架构

- **依赖/目标**：依赖第 08～10 章已有的 UI、相机、网络职责；解释“每个资源只有一个主人”和跨线程只传消息。
- **可用事实源**：client/src/application/voice_client.cpp、audio/audio_engine.cpp、network/voice_transport.cpp、ui/device_ui.cpp；docs/test/client-responsibility-layout-20260801.md；docs/test/client-under-2000-refactor-20260731.md；当前单元测试。
- **已验证边界**：源码和 host 测试可证明当前队列、线程和状态转换实现；不能证明 RV1106 上实时调度、无竞态或长期不泄漏。UI action queue 容量等常量只能写为当前实现配置。
- **必须避免**：
  - 不用“多线程所以更快/实时”作结论。
  - 不画源码中不存在的线程。
  - 不把一次无崩溃运行写成线程安全证明。
- **可执行验证/证据**：

~~~sh
cmake --preset host-debug
cmake --build --preset host-debug
ctest --preset host-debug --output-on-failure
python3 scripts/tests/test_client_source_contract.py
~~~

若需性能结论，必须补同一目标板、同一构建、同一负载下的时间戳、队列水位、CPU/内存和丢帧/欠载统计。

### 第 12 章 网络协议、WSS 与流式通信

- **依赖/目标**：依赖第 10 章服务端和第 11 章并发；先 fixture，再局域网发现，再 TLS/WSS，最后流式媒体。
- **可用事实源**：协议文档；scripts/verify_protocol_fixtures.py；client/src/network/voice_transport.cpp；server/internal；docs/test/p1-cpp-wss-client-validation-20260728.md。
- **已验证边界**：当前协议约束包括 UDP 17807、WSS 17806、发现文本、64 字节 PCM 头、上行 16 kHz 与下行 24 kHz等；必须逐项以当前协议文件和 fixture 为准。本轮 fixture 通过，但没有当前板 WSS 或真实云端闭环。
- **必须避免**：
  - 不把 WSS 等同于设备身份认证；开发共享 token 不是生产级每设备身份。
  - 不声称 flow.credit 存在；当前 v1 无该事件。
  - 不只凭端口监听写“语音链路成功”。
- **可执行验证/证据**：

~~~sh
python3 scripts/verify_protocol_fixtures.py
cd server
go test ./...
~~~

WSS 实测证据必须同时含证书/SPKI 校验、认证结果、事件 ID、序列/大小边界和关闭原因；敏感字段一律脱敏。

### 第 13 章 系统日志、性能分析与故障调试

- **依赖/目标**：依赖第 08～12 章；用“现象→最小只读检查→单一变量→恢复→重新验收”代替大段命令堆。
- **可用事实源**：当前板 board-baseline/topology/listening-ports；docs/test/rv1106-validation-gates.md；源码日志点；历史测试记录仅作故障样例。
- **已验证边界**：当前证据可支撑 DHCP 恢复、Wi-Fi 连接失败、触摸未绑定、显示缺物理证据、摄像头所有者等真实案例；没有长稳、满负荷 CPU/内存、温升、延迟分布。
- **必须避免**：
  - 不把板端 2021 时间戳当本轮事件时间；以主机采集时间为准。
  - 不用 killall、重启网络、卸载驱动、改设备树作为首步。
  - 不把 top 的瞬时截图写成性能上限。
- **可执行验证/证据**：

~~~sh
uptime
free -k
ps
cat /proc/meminfo
cat /proc/uptime
ip -s link
cat /proc/interrupts
~~~

如系统提供 top、dmesg 或 journal/logread，再按实际可用工具选一个；读取内核日志也可能含敏感标识，截图前脱敏。长稳必须预先定义时长、负载、采样间隔、失败阈值和恢复检查，本轮仍待验收。

### 第 14 章 Linux 实时音频与 ALSA

- **依赖/目标**：必须在第 01～13 章非音频硬件、项目主线和调试方法之后；本章先讲 PCM 概念和安全枚举，不立刻录音。
- **可用事实源**：原理图第 2/8 页；RV1106 手册音频相关页；client/src/platform/rv1106/audio_backend.cpp；docs/architecture/audio-backends.md；/proc/asound 当前证据；docs/test/p0-alsa-full-duplex-hil-guide.md。
- **已验证边界**：
  - 当前只确认 rv1106-acodec 和一组播放/采集 PCM 子流可枚举。
  - 源码配置为采集 48 kHz、S16LE、4 通道，播放 48 kHz、S16LE、2 通道，20 ms/960 帧；这是实现契约，不是本轮音频 I/O 通过。
  - 旧 ALSA HIL 是历史证据，不可替代当前板复测。
- **必须避免**：
  - 不给裸 arecord 4 通道命令；错误通道映射、设备占用和隐私风险都未处理。
  - 不在未获授权时播放、录音、停止 OEM 音频服务或抢占 /dev/snd。
  - 不把 /proc/asound 枚举写成麦克风和扬声器实物正常。
- **可执行验证/证据**：

~~~sh
cat /proc/asound/cards
cat /proc/asound/pcm
for n in /dev/snd/*; do
  printf '%s: ' "$n"
  fuser "$n" 2>/dev/null || true
done
~~~

以上只读枚举是本章当前通关上限。任何声音输入/输出须另获授权，先确认现场隐私、音量、外接麦克风/扬声器、资源所有者、最长时长、临时文件与恢复动作。

### 第 15 章 数字音频处理、重采样与 3A

- **依赖/目标**：依赖第 14 章的格式与资源所有权；按“48 kHz 多通道采集→16 kHz 2 mic+参考→3A→VAD/上传”和“24 kHz TTS→48 kHz 播放”解释。
- **可用事实源**：client/src/platform/rv1106/rockchip_voice_dsp.cpp；audio_backend.cpp；docs/architecture/audio-runtime.md、audio-backends.md；docs/test/p0-mode1-hard-reference-validation-20260801.md；Rockchip 3A 历史构建/HIL 记录。
- **已验证边界**：当前源码使用 Mode1，捕获 4 个通道，但 3A 固定输入为 16 kHz 的 2 mic + refL，refR 被捕获后丢弃；TTS 24 kHz mono 重采样到 48 kHz。源码/host 测试只能证明合同和逻辑；降噪、回声消除效果必须由同轮板端声学 HIL 证明。
- **必须避免**：
  - 不把“3A 链接成功”写成 AEC/ANS/AGC 效果好。
  - 不把 4 通道叫作 4 个麦克风；设计有两个外接麦克风，其他通道是参考链路含义。
  - 不宣称 refR 参与当前 3A。
- **可执行验证/证据**：

~~~sh
python3 scripts/tests/test_audio_backend_reference_contract.py
python3 scripts/tests/test_rockchip_3a_hil.py
python3 scripts/tests/test_audio_vendor_cmake.py
~~~

这些是主机合同/解析器测试，不产生声音。板端 3A HIL 要在第 17 章统一授权和执行，且记录输入映射、ABI、库哈希、profile、延迟和客观/主观结果。

### 第 16 章 唤醒词、VAD 与语音状态机

- **依赖/目标**：依赖第 15 章产生的干净 16 kHz 流；先用确定性测试讲状态，再做唤醒/VAD，避免一上来就接云端。
- **可用事实源**：client/src/platform/rv1106/snowboy_legacy_bridge.cpp；audio_engine.cpp；voice_client.cpp；WebRTC VAD/Snowboy CMake 配置；相关 host tests；docs/architecture/audio-runtime.md。
- **已验证边界**：可从源码核对 20 ms 帧、队列上限与状态转换；历史唤醒/HIL 不代表当前最终二进制。本轮没有板端唤醒词、误唤醒率、漏唤醒率或噪声环境测试。
- **必须避免**：
  - 不把一次唤醒写成准确率。
  - 不把 VAD 当 ASR，也不把本地唤醒写成云端能力。
  - 不忽略 Snowboy 模型/运行库的来源、哈希和再分发许可。
- **可执行验证/证据**：

~~~sh
ctest --preset host-debug --output-on-failure
python3 scripts/tests/test_client_source_contract.py
~~~

若需给准确率，须另建脱敏语料、说话人/距离/噪声分层、命中定义和统计表；当前文档只能把这部分标为待板端授权实测。

### 第 17 章 流式 TTS、播放系统、语音打断与 HIL 验收

- **依赖/目标**：依赖前 16 章全部通关；这是实际音频实现和物理 I/O 的最后门。顺序必须是资源清点→低音量播放→经隐私授权的有界采集→全双工/Mode1→3A→唤醒/VAD→WSS/ASR/TTS→追问→触摸打断→语音打断→长稳。
- **可用事实源**：scripts/hil/rv1106_alsa_full_duplex.sh；docs/test/p0-alsa-full-duplex-hil-guide.md；p0-rockchip-3a-hil-guide.md；p0-rockchip-mpi-audio-hil-guide.md；docs/releases/v1.0.0.md；历史 qwen/teaching-v1/barge-in 记录；当前源码。
- **已验证边界**：
  - 历史最后真人 HIL 客户端 SHA-256 为 e28a7d64afe30c4d552ae0998d93e122143db591c2cfa747e836e727f84fe96f。
  - 最终 v1.0.0 stripped 客户端 SHA-256 为 b8476d42a4520669ed02a8aabd52e5d829fafa5b9252d76bd0cd1d70cb245a37；它通过严格构建/ELF 门禁，但板端随后断线，未完成同一轮真人 HIL。
  - 当前 2026-08-10 板证据未授权音频 I/O，也未做长稳。
- **必须避免**：
  - 绝不能写“v1.0.0 最终二进制已经完成真人全链路 HIL”。
  - 不把旧 e28a 的结果移植给 b847，不把历史板端结果写成本轮通过。
  - 不在文档中直接给会抢占资源、裸录 4 通道、外放高音量或保留人声文件的命令。
- **可执行验证/证据**：

~~~sh
# 仅做脚本语法与主机测试，不产生音频
sh -n scripts/hil/rv1106_alsa_full_duplex.sh
python3 scripts/tests/test_rv1106_alsa_full_duplex_hil.py
python3 scripts/tests/test_rockchip_3a_hil.py
~~~

实际板端命令必须在授权后从相应 HIL guide 逐项执行，并先动态发现 PCM/owner、采用唯一临时目录、限定音量/时长、记录恢复与删除策略。验收矩阵要将构建、协议、板端运行、物理听感、双讲、打断、隐私清理和长稳分列；未做的写 BLOCKED/待验收。

### 第 18 章 boomPI 源码完整阅读与二次开发

- **依赖/目标**：依赖第 01～17 章；把前面已经亲手验证过的模块映射回目录、类和测试，给一个不改变硬件风险边界的最小扩展路线。
- **可用事实源**：README.md；AGENTS.md；CMakeLists.txt、CMakePresets.json；client、server、scripts、docs/architecture、protocol、docs/releases；本轮 host/board evidence。
- **已验证边界**：
  - 本轮 host-debug 配置/构建通过，CTest 41/41（13.37 s），Python 完整测试 77/77（41.182 s），协议 fixture 通过。
  - Go 1.26.5 下 test/vet/CGO=0 build 通过。
  - 本轮 RV1106 严格交叉构建、最终二进制板端运行、当前板全硬件闭环仍未完成。
- **必须避免**：
  - 不把“读完目录”写成掌握全部第三方 SDK。
  - 不让二次开发教程从改设备树、分区、音频 ABI 或云端协议开始。
  - 不掩盖当前源码中的硬编码设备路径；/dev/video14、/dev/i2c-3、eth0、wlan0 等应成为可发现化改造清单。
- **可执行验证/证据**：

~~~sh
git status --short
git rev-parse HEAD
cmake --preset host-debug
cmake --build --preset host-debug
ctest --preset host-debug --output-on-failure
python3 -m unittest discover -s scripts/tests -p 'test_*.py'
python3 scripts/verify_protocol_fixtures.py
cd server
go test ./...
go vet ./...
CGO_ENABLED=0 go build -trimpath -o '<输出目录>/boompi-server' ./cmd/boompi-server
~~~

RV1106 材料齐全后再执行 rv1106-candidate，并用 file、readelf -h/-d、scripts/probes/verify_rv1106_elf.py 和 sha256sum 建立 staging manifest。目标板运行需单独授权和新一轮证据，不能借用历史结果。

## 4. 必须通过官方网络补齐的信息

本轮本地只有完整的 Rockchip RV1106 Datasheet Rev1.8。以下信息若进入“器件能力、最大值、时序、接口规范、当前云服务”正文，必须查询制造商/项目官方站点并保存 URL、标题、版本、发布日期、访问日期和文件 SHA-256；博客、电商参数、论坛和搜索摘要不能作为唯一依据。

| 类别 | 首选官方发布者 | 必须核对的内容 | 写作边界 |
|---|---|---|---|
| RV1106 硬件/SDK | Rockchip、匹配板卡/BSP 的官方发布方 | 完整 Hardware Design Guide、TRM、pinmux/clock/reset/power sequence；当前 BSP/SDK、GCC wrapper、uClibc sysroot、烧录工具/loader、release notes 与许可 | 本地几张 Hardware Guide 截图不是完整官方来源；SDK 版本必须与目标镜像匹配 |
| eMMC | Samsung | KLM8G1GETF-B041 容量、总线、速度等级、电压、封装和可靠性条件 | 网表供应商字段不能当官方规格 |
| 无线模组 | M8800DS2 制造商/模块认证机构；必要时 AIC 官方 | 实际芯片、USB/SDIO 选项、Wi-Fi/BT 版本、频段、天线、电源、驱动/BSP、法规认证 | 当前设计只确认 USB 连接；AIC8800 日志不能自动证明模组全规格 |
| USB 复用 | Texas Instruments | TS3USB3031 真值表、带宽、电平、断电行为、控制脚定义 | 原理图只能证明连接和控制网名 |
| 电源 | EA3059 制造商、Texas Instruments | EA3059 各路功能/时序；TLV62568 输入输出、最大电流、布局要求；RTC 充电条件 | 不从阻容值单独推断整板稳定性 |
| 以太网接口 | HR911105A 制造商、Rockchip | 磁性参数、LED、中心抽头、PHY 布线/终端要求 | SoC 支持 10/100 不等于实测吞吐 |
| 显示 | 实际 ST7789P3 面板/模组制造商 | 分辨率、SPI 模式/最大时钟、初始化表、方向、背光电气、FPC pinout | 控制器通用手册不能替代具体面板模组手册 |
| 触摸 | GOODIX 与实际触摸模组制造商 | GT911 地址选择/复位时序、坐标范围、中断、供电、FPC pinout | 0x14/0x5d 必须区分源码 fallback、DT 描述和真实应答 |
| 摄像头 | SmartSens 与实际 SC3336 模组制造商 | sensor 供电/MCLK/寄存器、lane/速率、模组 pinout、镜头/方向、ISP/BSP 配套 | 传感器支持不等于 FPC 模组兼容或当前图像质量 |
| 音频功放 | FM8002A 制造商 | BTL 输出、增益/功率条件、负载、关断、去耦 | 禁止从典型值推断本板实测音量 |
| USB Type-C | USB-IF 与所用接口/保护器件官方资料 | CC 角色、电流声明、USB 2.0、ESD/过压条件 | 未见 PD 控制器时不得宣传 PD 或快充 |
| Linux/BSP 媒体 | kernel.org、Linux media/ALSA 官方文档、Buildroot 官方 | 与 5.10 BSP 匹配的 V4L2/media-controller、ALSA、设备树、Buildroot 说明 | 上游最新文档可能与厂商 5.10 BSP 不一致，要标版本 |
| LVGL/基础库 | LVGL、OpenSSL、Boost、WebSocket++、WebRTC 官方仓库/发布页 | LVGL 8.2 API；OpenSSL 3.5.7；Boost 1.74；WebSocket++ 0.8.2；WebRTC VAD 来源、许可和哈希 | 不用“最新版”替代仓库固定版本 |
| Rockchip 3A/媒体库 | Rockchip/匹配 SDK 发行方 | 头文件、ABI、so/a 哈希、profile、调用限制、再分发许可 | 私有库链接或历史 HIL 不证明效果与许可 |
| Snowboy | 可追溯官方/维护仓库及模型授权方 | runtime/model 来源、版本、哈希、平台支持、再分发和隐私条款 | 找不到可接受官方来源时只描述仓库现状，不作推荐 |
| Go | go.dev 与模块各自官方仓库 | 当前 Go 1.26.5 下载、校验值、支持平台；go.mod 中模块版本/安全公告 | 本轮临时工具可复现，但正式文档要给官方固定下载入口 |
| Qwen/DashScope | 阿里云百炼/DashScope 官方文档 | 当日可用模型、端点、区域、事件格式、采样率、voice、Workspace、API Key、费用/额度/限流/隐私 | 模型名、端点和价格高度时效；必须在发布前重查并标访问日期 |

对于“外接兼容”还需查询实际购买的屏、触摸、相机、麦克风和扬声器模组官方页面/规格书。即使原理图备注某款 Waveshare/Luckfox 模组，也只能写“设计目标/兼容意图”，直到 FPC pinout、电压、方向和当前实物料号逐一核对。

## 5. 全文统一门禁

1. **发现门禁**：网络接口、输入 event、video、SPI、I2C、PCM 先发现，再操作；源码默认值必须标“当前实现默认”。
2. **写状态门禁**：烧录、分区、设备树、启动项、持久网络、服务 stop、存储写回、显示输出、摄像头采帧、音频 I/O 均在步骤前单独说明并取得相应授权。
3. **证据门禁**：命令退出、哈希、物理观察、长期稳定四类结果分开；没有证据写待实测，不写“正常”。
4. **恢复门禁**：临时文件使用唯一目录和白名单清理；改配置前备份并验证恢复；不使用固定共享路径脚本冒险复跑。
5. **隐私门禁**：Wi-Fi、SSH、API Key、token、证书、音频、人像和环境画面先脱敏；录音/相机须限定目的、时长、保存位置和删除策略。
6. **版本门禁**：每个可执行产物记录源码 commit/tag、工具链/SDK、构建参数、bytes、SHA-256、ELF/依赖/RPATH；历史 e28a、最终 b847 和本轮待构建产物永不混用。
7. **章节通关门禁**：每章至少包含运行位置、是否写数据、预期结果、失败恢复、通关卡和证据路径；依赖章未通过时只能做主机/只读部分。

最终写法建议统一为：“在 2026-08-10 这块板、当前镜像和本次步骤下，观察到……；它还不能证明……”。这比“硬件正常”“已经支持”更准确，也能让初学者知道失败并不等于整块板损坏。
