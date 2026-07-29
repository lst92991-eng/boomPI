# RV1106 自研板 BSP 候选镜像清单（2026-07-29）

## 文档状态

| 项目 | 内容 |
| --- | --- |
| 采集时间 | 2026-07-29 17:06:18 +08:00（Asia/Shanghai） |
| 采集方式 | Windows 开发主机经 SSH 对 Ubuntu BSP 工作区做只读盘点 |
| BSP 工作区 | `<BSP_ROOT>`，公开来源为 Luckfox Pico SDK |
| 主板版本 | 未记录 |
| 当前结论 | **拒绝烧录：现有 DTB 同时启用 SC3336 与板上不存在的 MIS5001，且 BSP 工作树不可复现** |
| 操作边界 | 本轮没有修改或构建 BSP，没有刷机、重启、修改板端配置或调用付费服务 |

本文固定 2026-07-29 现有候选输入和产物，供后续修复前后比较。它不是发布清单，也不构成
烧录授权。任何烧录仍须针对修复后重新生成、重新哈希的具体镜像取得明确授权。

## 结论摘要

独立 DTB 与 `rootfs.img` 可以和当前候选工作区建立只读关联：`.BoardConfig.mk` 指向
自研板配置；自研板 DTS 的修改时间早于同名 DTB 13 秒；编译后 DTB 的 `model` 为
`LST RV1106 Custom Board`；`rootfs.img` 内的硬件基线和 `S60micinit` 与 overlay 文件
SHA-256 完全一致。尚未只读解包 `boot.img`/`update.img` 并逐组件比对，因此不能把这两项
或整套 `output/image` 自动认领为同一输入集生成的完整自研板镜像。

但是它仍不能烧录：

1. 编译后 DTB 中 `sc3336@30` 和 `mis5001@31` 都为 `status = "okay"`，二者还共享
   `MCLK_REF_MIPI0` 与 GPIO3_C5 reset。最新主板网表只提供一个两 lane 相机接口，现场使用的
   是 SC3336；MIS5001 是 Ultra IPC 继承文件的残留。
2. BSP HEAD 虽已固定，但板级核心文件均未跟踪，另有已修改内核 defconfig 和三份 Wi-Fi
   预编译二进制。仅凭 HEAD 无法重现现有镜像。
3. BoardConfig 和 `S60micinit` 的注释仍把历史 `netlist (5).enet` 当作依据，虽然 rootfs
   中另一个 baseline 文件已经写入最新 `netlist.json` 哈希。事实来源尚未统一。
4. 工作区还含测试二进制、编辑器 swap、旧构建日志和一个名为 `%ln` 的空文件。它们不是
   发布输入，必须从正式构建输入集合中隔离。

## 主板事实基线

本清单只使用用户最后确认的主板 `netlist.json`：

| 字段 | 值 |
| --- | --- |
| 逻辑文件名 | `netlist.json` |
| 文件大小 | 694,291 bytes |
| 主机修改时间 | 2026-07-24 16:35:08 +08:00 |
| SHA-256 | `f668a52ac19debdeb1eb257e8c4601fec14a57e1662424d8969db060e8da5bcc` |

排除规则：`full_netlist (4).csv` 和 `netlist (5).enet` 是历史主板资料，不能形成新的引脚、
电源或设备树结论；`netlist (3).json` 只用于双 16P 屏幕转接板子系统。

### 音频电气矩阵

| 功能 | 连接器侧 | 偏置/耦合 | RV1106 侧 | 软件含义 |
| --- | --- | --- | --- | --- |
| MIC0 正端 | U9.1 `MIC0_P` | R47 100 Ω + R48 1.1 kΩ 取 MICBIAS；C57 1 µF 隔直 | U3.34 `CODEC_MIC0P` | 候选左路；物理 slot 未验证 |
| MIC0 负端 | U9.2 `MIC0_N` | R49 0 Ω 接地；C60 1 µF 隔直 | U3.33 `CODEC_MIC0N` | 两线偏置单端/伪差分接法，不是浮地差分麦 |
| MIC1 正端 | U12.1 `MIC1_P` | R50 100 Ω + R51 1.1 kΩ 取 MICBIAS；C61 1 µF 隔直 | U3.36 `CODEC_MIC1P` | 候选右路；物理 slot 未验证 |
| MIC1 负端 | U12.2 `MIC1_N` | R52 0 Ω 接地；C62 1 µF 隔直 | U3.35 `CODEC_MIC1N` | 两线偏置单端/伪差分接法，不是浮地差分麦 |
| MICBIAS | U3.32 | 经 R47/R50 分成两条偏置支路，C71/C72 各自对地去耦 | `CODEC_MICBIAS` | 这是单个公共偏置输出；须确认有效，左右 ADC/Work 另行回读 |
| Codec AVDD | U3.31 | VDD_1V8 经 L12，C88 100 nF 去耦 | `CODEC_AVDD1V8` | 实测历史值 1.78 V |
| Codec VCM | U3.30 | C63 4.7 µF 对地 | `CODEC_VCM` | 未启用模拟通路时测得 0 V 不能单独判定主控损坏 |
| Speaker | U3.29 `CODEC_LINEOUT` | C55 1 µF，R35/R34 各 20 kΩ | U8 FM8002A BTL → CN1 | PA `SHUTDOWN` 仅由 R33 10 kΩ下拉，无 GPIO |

网表只证明电气连接，不证明 U9/U12 对应的 PCM 左右 slot、声学极性、幅相一致性或数字
播放 reference。候选 `S60micinit` 使用 `DiffadcLR`；该枚举已有旧镜像临时 A/B 观察支持，
但仍须在正确镜像上用单侧近场刺激重新验证。DTS 不得虚构 `pa-enable-gpios`。

### 外设矩阵与 DTS 约束

| 子系统 | 板级事实来源 | 当前候选配置 | 判定 |
| --- | --- | --- | :---: |
| ST7789P3 | 主板网表只证明 FPC2 的 SPI0 M0 信号：SCLK GPIO1_C1、MOSI GPIO1_C2、GPIO CS GPIO1_C4、DC GPIO2_A2、RESET GPIO2_A3、BL GPIO1_C5；器件身份来自屏幕资料/历史实测 | 关闭 Ultra RGB/DRM；SPI0 使用 GPIO CS；仅注册 `spidev@0` 8 MHz；内核 backlight disabled | 原型可用，正式内核显示/背光驱动未固化 |
| GT911 | 主板网表只有 `GT911_*` 网络名及 I2C3 M0、INT GPIO2_B1、RESET GPIO2_A0；器件身份/地址来自历史实测 | `touchscreen@5d`，I2C3 M0，100 kHz | 信号映射一致；地址和上电时序仍须目标镜像复测 |
| SC3336 | 主板网表只证明 FPC1 的两 lane、I2C4 M2、MCLK0、RESET GPIO3_C5；器件身份来自现场装配和历史实测 | `sc3336@30` enabled，两 lane | 拓扑方向匹配；地址、xvclk、reset 极性和模组电源仍须实测，且被 MIS5001 残留污染 |
| MIS5001 | 最新主板网表没有传感器器件实体，现场也未安装 MIS5001 | `mis5001@31` 仍 enabled，共用 MCLK/reset | **阻断烧录** |
| Ethernet | RV1106 片内 PHY 直连 HR911105A；不是外置 RMII/MDIO PHY | `&gmac status = "okay"` | 方向正确；禁止虚构 MDIO PHY/reset GPIO |
| Wi-Fi | U1 M8800DS2 的主机接口是 USB；SDIO D0..D3/CMD/CLK 全悬空；经 U15 与 USB-A/Type-C 物理三选一；PWR_KEY 经 R56 上拉到 1.8 V，并由 R57 接 GPIO3_B2 | USB host；GPIO0_A1 hog 为 low；BoardConfig 选 `AIC8800DW_USB`，但未配置/验证 PWR_KEY 时序 | 仅 USB 拓扑方向匹配；SW5 位置、PWR_KEY 有效电平/时序和 AIC8800 驱动兼容性均待关闭 |
| Audio | 片内 Codec + I2S0；两路模拟麦；PA 无软件 GPIO | `acodec`、`i2s0_8ch`、simple-audio-card enabled；`mclk-fs=256`；TRCM=1 | 只证明候选配置，不证明四通道或 reference |

网名中的 `GPIO7/GPIO10/...` 是板级标签，不能直接当 Linux GPIO 编号；DTS 必须使用
Rockchip bank/pin，例如 GPIO1_C2、GPIO2_A7。

## BSP 源码身份

| 字段 | 值 |
| --- | --- |
| 公开远端 | `https://gitee.com/LuckfoxTECH/luckfox-pico.git` |
| 分支 | `main`，跟踪 `origin/main`，ahead/behind `0/0` |
| HEAD | `994243753789e1b40ef91122e8b3688aae8f01b8` |
| HEAD 说明 | `Add Support for Luckfox Pico Zero (#310)` |
| 已跟踪 dirty diff 摘要 | `git diff --binary --full-index` 的 SHA-256 为 `fe4f254812c788787bb3c6c81ef5974e66e672a8c0fd2a9011fc6de69a12fb2a` |
| 当前 BoardConfig 链接 | `.BoardConfig.mk` → `project/cfg/BoardConfig_IPC/BoardConfig-EMMC-Buildroot-RV1106_LST_CustomBoard-IPC.mk` |

### 关键构建输入

| `<BSP_ROOT>` 相对路径 | 状态 | SHA-256 |
| --- | --- | --- |
| `project/cfg/BoardConfig_IPC/BoardConfig-EMMC-Buildroot-RV1106_LST_CustomBoard-IPC.mk` | untracked | `f28d3154e94ff53761a6252a4eea68ae93a563634a5194ffa64296f38018c87b` |
| `sysdrv/source/kernel/arch/arm/boot/dts/rv1106g-lst-custom-board.dts` | untracked | `290a56928cf41b8889cc4d39109cf8fa9cbda128ea6d9e2312910d13f3230f67` |
| `sysdrv/source/kernel/arch/arm/boot/dts/rv1106-luckfox-pico-ultra-ipc.dtsi` | tracked | `473ec55480469a89a07149ad448aa327376227f103b600482549d60c91dc5930` |
| `sysdrv/source/kernel/arch/arm/boot/dts/rv1106-evb.dtsi` | tracked | `c490b80189fdac4a812272fda9c7cf532d854c6369cb564fc80883c43d454926` |
| `sysdrv/source/kernel/arch/arm/boot/dts/rv1106.dtsi` | tracked | `d8b1e4f08b8b93480876c4281bbb78cbc9f28781b6b964ce77b4fad215031aa0` |
| `sysdrv/source/kernel/arch/arm/configs/luckfox_rv1106_linux_defconfig` | modified | `1a5ab80e10a396b524903918f303cf6581e80dce1ebef5e279d9fb772e9616a9` |
| `sysdrv/tools/board/buildroot/luckfox_pico_w_defconfig` | tracked | `13ee95213bc0eaff087fc571bf2c99654602271cc78af4edd1dd464c77e04cd1` |
| `sysdrv/source/uboot/u-boot/configs/luckfox_rv1106_uboot_defconfig` | tracked | `8ebb59c0a0dacd107cd0cb53c112a2c282aa0a73cc95801a8d8baf207b9d4e91` |
| `sysdrv/source/uboot/u-boot/configs/rk-emmc.config` | tracked | `c7271c23effaa93bd363b394e2f8618b4ca92744192444aa33ab72487c3ce025` |
| `sysdrv/source/uboot/u-boot/configs/rv1106-luckfox-rgb-reset.config` | tracked | `a6685c911f8be984dc004e56a29d68eb8d6a5bf0a5d83eb8f2e7edcaa35ddee4` |
| `project/cfg/BoardConfig_IPC/lst-custom-oem-pre.sh` | untracked | `be34d8ba16a4e7a8bce2a7e09ab9f7cd9370f0495e99ad41afdfedf06bdd8c43` |
| `project/cfg/BoardConfig_IPC/lst-custom-insmod_wifi.sh` | untracked | `f572f287384d963cd41a053906b56f25155f3fcb67f7bdc290bceec1fed72c2b` |

内核 defconfig 的 dirty 差异为启用 `CONFIG_POSIX_MQUEUE`，并移除显式
`CONFIG_JUMP_LABEL=y` 与 `# CONFIG_STACKPROTECTOR_STRONG is not set` 两行。它与音频/网表
没有直接关系，但会改变发布内核，必须作为候选输入审查。

### Rootfs overlay

| 相对路径（前缀 `project/cfg/BoardConfig_IPC/overlay/overlay-lst-custom-audio/`） | SHA-256 |
| --- | --- |
| `etc/boompi-hardware-baseline` | `2cdf9fb9dfa7be6adb158800dbe0b20a4ee3fed12cdbad45293fe0d142e6978a` |
| `etc/init.d/S41eth0dhcp` | `68a7724562601d2febddad28f07b817ff28af556e57c70a557f6a5af8cec5ddf` |
| `etc/init.d/S49rootperms` | `154c3ca9b18bbbee26c8c827da041377da12563b43d6f4e02e77be4665dabf4c` |
| `etc/init.d/S60micinit` | `a0986b59bef5b7d3694f6cdbbaca6d6e1d609ae685a1c7a7188f56cdb5cb1316` |
| `usr/bin/gt911_st7789p3_touch_paint.py` | `69df2853434ae336583aa2a01aacde3125535dd5d2868ae050a21a8d1ba56927` |
| `usr/bin/lst-audio-test` | `c9f8892f6d4a214dbbb7f96211c4a54cf9d4807016ed5609330bba4fb0529810` |
| `usr/bin/st7789p3_runtime_test.py` | `67e17dd7955b175e8bb9e1128403dd9039c46bd5085af203184e94b38c35817d` |

`rootfs.img` 经 `debugfs` 只读读取后，内部
`/etc/boompi-hardware-baseline` 和 `/etc/init.d/S60micinit` 的 SHA-256 分别仍为
`2cdf9f...` 与 `a0986b...`，证明这两个 overlay 文件进入了现有 rootfs。

### 修改的 vendor Wi-Fi 二进制

| 相对路径 | SHA-256 |
| --- | --- |
| `project/app/wifi_app/hostapd-2.6/hostapd/hostapd` | `85183e6caca63cd98dd7a00cc70e016e52c94bebed653a5527647bf8c7651864` |
| `project/app/wifi_app/hostapd-2.6/hostapd/hostapd_cli` | `250df6836494da17c11083187219c203375195f9de3eee978c184a316249a397` |
| `project/app/wifi_app/wifi/librkwifibt.so` | `d84f637ff484db61f487c7b085d71d8893070e7a6f90173499f5ce0bec1e7840` |

这些二进制缺少本轮可核验的来源、版本和构建命令。修复 DTS 并不能自动解除它们的供应链
风险；正式镜像必须恢复到已知 vendor 版本，或单独固定来源和许可证。

### 不纳入发布输入的未跟踪项

- `project/app/my_rv1106_app/` 含 4 个文件、24,476 bytes，包括可执行文件和
  `.Makefile.swp`；按“相对路径 + 各文件 SHA-256”排序后得到树摘要
  `4ea7b7baef214be4a9767283ab1fb477ac654c2ec19ac70e0626913ab51084e1`。
- `%ln` 是 0-byte 空文件，SHA-256 为
  `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`。
- `project/build_*.log` 是历史构建日志，其中多个文件名仍含 `netlist5`/`micfix`。日志只能
  作为线索，不能作为当前输入/产物对应关系证明。

## 工具链和构建参数

| 项目 | 值 |
| --- | --- |
| Buildroot defconfig | `luckfox_pico_w_defconfig`（SDK 标识 Buildroot 2023.02.6） |
| Kernel defconfig | `luckfox_rv1106_linux_defconfig` |
| U-Boot defconfig | `luckfox_rv1106_uboot_defconfig` + `rk-emmc.config` + `rv1106-luckfox-rgb-reset.config` |
| 交叉编译器 | `arm-rockchip830-linux-uclibcgnueabihf-gcc`，crosstool-NG 1.24.0 / GCC 8.3.0 |
| 编译器 SHA-256 | `146a12266d534a8e527535bc0da6a233b7de7052ec19601696734f8e435f765a` |
| Boot medium | eMMC |
| Kernel DTS | `rv1106g-lst-custom-board.dts` |
| Rootfs | ext4，6 GiB 分区定义 |
| OEM / userdata | ext4，分别 512 MiB / 256 MiB 分区定义 |
| CMA | 66 MiB |
| Wi-Fi build option | `RK_ENABLE_WIFI=y`，`RK_ENABLE_WIFI_CHIP=AIC8800DW_USB` |

本轮没有重新执行构建，历史日志也没有稳定记录完整命令。因此“构建命令”仍为待关闭项；
修复后的候选必须从明确的 `.BoardConfig.mk` 开始记录一次干净的 `build.sh` 命令和退出码。

## 现有二进制产物（拒绝烧录基线）

时间记录均换算为 +08:00：候选 DTS 为 2026-07-29 14:02:03；同名独立 DTB 为
14:02:16；boot/oem/rootfs/userdata/update 位于 14:02:15 至 14:04:54。`boot.img` 反而比
独立 DTB 早 1 秒，且本轮没有解包 `boot.img`/`update.img` 做组件哈希比对，所以时间只能
用于识别文件，不能证明整包输入关系。独立 DTB 反编译得到
`model = "LST RV1106 Custom Board"`，且同时包含 enabled 的 `sc3336@30` 和
`mis5001@31`；`acodec-sound`、`rockchip,clk-trcm = <1>`、`touchscreen@5d`、
`spidev@0` 和 USB mux GPIO hog 也已进入该 DTB，`pa-ctl-gpios` 不存在。

| 产物 | 大小（bytes） | SHA-256 |
| --- | ---: | --- |
| `output/out/sysdrv_out/board_uclibc_rv1106/rv1106g-lst-custom-board.dtb` | 38,366 | `d1c1de16466d32a3925c3bbe5f5f6ec1aabe582e3486e2c423da2d5a8a04cdea` |
| `output/image/env.img` | 32,768 | `2938e689b63db92f48f06c2c2ccac85ecffef91a2227ff5c61122ae3c09e788f` |
| `output/image/idblock.img` | 188,416 | `7e6c15ef7bea896c6f47afb72cf26cdf779b679ea1ceab087c8d12365b577fb3` |
| `output/image/uboot.img` | 262,144 | `12dd15f088bca2ea9a77328c24dda356fa8e3a41fbc04a9bb2d66822b1caeb12` |
| `output/image/boot.img` | 3,278,336 | `240911ca0430564948edd939bf5403012d4e4f6fb26d50f371ebb43ffa956264` |
| `output/image/oem.img` | 43,917,312 | `30f0a4d1f1e96bb82394ab71eae924b18756c45a594e8e8210b997b234525a6c` |
| `output/image/userdata.img` | 9,999,360 | `470cf4bd703602601279310e2395355552595ea2147f15ccb3bc67b356738f09` |
| `output/image/rootfs.img` | 415,928,320 | `188f1f0c6170d41c7bbd40cd9a4b3c16a4950f1431ba1a3ae8e7932d2b30185f` |
| `output/image/update.img` | 474,151,498 | `7712bbc6a1d922163be87be390f1bc1ce4257eee1cb3baf03c63a47a449fb400` |

这些哈希的作用是确保后续不会误烧这一版。修复任一输入后，以上所有受影响产物都必须重新
生成并使用新哈希；禁止沿用本表中的 `update.img`。

## 修复和重建闸门

修复后的下一版至少要满足：

1. 自研板 DTS 显式禁用 Ultra 继承的 `mis5001`，并检查 CSI D-PHY 不再保留第二个活动输入；
   反编译 DTB 必须只保留一个 enabled 的 SC3336 路径。
2. BoardConfig 与 `S60micinit` 注释改为引用 `netlist.json` 及其 SHA-256，不再出现历史
   `netlist (5).enet`。
3. 把 BoardConfig、DTS、overlay 和钩子纳入可审计提交或独立补丁集合；剔除 swap、测试
   二进制、空文件与日志。dirty patch、所有输入和产物重新计算 SHA-256。
4. 说明三份修改 Wi-Fi 二进制的来源；无法说明时恢复 SDK 已跟踪版本后再构建。
5. 记录完整构建命令、开始/结束时间和退出码；检查 DTB、boot、rootfs、OEM、userdata、
   `update.img` 的大小与 SHA-256。
6. 只读检查 rootfs 内 `boompi-hardware-baseline`、`S60micinit`，以及 OEM 内 USB Wi-Fi
   驱动/脚本；不得以源码目录文件冒充镜像内容。

## 烧录后的首次验收（尚未执行）

取得对修复后具体镜像的明确授权并烧录后，按以下顺序记录：

1. `model`、Linux/Buildroot 版本、boot/DTB 身份、`S60micinit` 和硬件 baseline 哈希。
2. 声卡、PCM、全部相关 mixer；记录候选启动值 `DiffadcLR`、公共 MICBIAS、左右 ADC Work
   和 gain/ALC，但以两只物理麦分别刺激后的真实双路结果判断模式。若其他枚举才正确，必须
   修改 `S60micinit` 和清单，不能把 `DiffadcLR` 名称本身当作验收条件。
3. Ethernet link、DHCP、SSH；确认板卡新 SSH 指纹后才更新开发主机 `known_hosts`。
4. direct ALSA 48 kHz/S16_LE/2ch 真全双工；再分别刺激 U9/U12，确认物理 slot 和极性。
5. 低幅可辨识播放序列确认数字 reference、tap、延迟与漂移；TRCM/Mode2 名称不能替代数据。
6. `rkipc` 不占用 MPI 资源的 maintenance 启动条件下，才执行 raw MPI AI/AO HIL。
7. 物理 packing 关闭后，才执行 16 kHz、256-sample、2 mic + 1 reference 的 Rockchip 3A
   有界探针；随后再评估实时率和声学效果。

当前板仍运行旧 `RV1106-Atguigu` / `SingadcL` 镜像。新 SSH 主机指纹尚未由用户确认，
所以本轮没有绕过严格主机密钥检查，也没有进行新的板端读取。

## 交叉证据

- [基础硬件测试历史记录](hardware-test-record-20260725-154016.md)
- [P0 vendor 音频证据基线](../test/p0-vendor-audio-inventory-20260727.md)
- [P0 直接 ALSA 全双工验证](../test/p0-alsa-full-duplex-validation-20260728.md)
- [P0 Rockchip MPI preflight](../test/p0-rockchip-mpi-audio-preflight-20260728.md)
- [P0 Rockchip 3A HIL 构建验证](../test/p0-rockchip-3a-hil-build-validation-20260729.md)
- [RV1106 验证闸门](../test/rv1106-validation-gates.md)
